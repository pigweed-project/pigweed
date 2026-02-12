// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#define PW_LOG_MODULE_NAME "I3C"

#include "pw_i2c_mcuxpresso/i3c_initiator.h"

#include <mutex>

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_function/scope_guard.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

using cpp23::to_underlying;

namespace pw::i2c {
namespace {
pw::Status HalStatusToPwStatus(status_t status) {
  switch (status) {
    case kStatus_Success:
      return pw::OkStatus();
    case kStatus_I3C_Timeout:
      return pw::Status::DeadlineExceeded();
    case kStatus_I3C_Nak:
    case kStatus_I3C_Busy:
    case kStatus_I3C_IBIWon:
    case kStatus_I3C_WriteAbort:
      return pw::Status::Unavailable();
    case kStatus_I3C_HdrParityError:
    case kStatus_I3C_CrcError:
    case kStatus_I3C_MsgError:
      return pw::Status::DataLoss();
    default:
      return pw::Status::Unknown();
  }
}

constexpr uint32_t kI3cInitOpenDrainBaudRate = 2000000;
constexpr uint32_t kI3cInitPushPullBaudRate = 4000000;
constexpr bool kI3cInitEnableOpenDrainHigh = false;
constexpr uint8_t kBroadcastAddressRaw = 0x7E;
constexpr pw::i2c::Address kBroadcastAddress =
    pw::i2c::Address::SevenBit<kBroadcastAddressRaw>();
std::array kDisecBuffer = {std::byte{0x0b}};
}  // namespace

// inclusive-language: disable
void I3cMcuxpressoInitiator::Enable() {
  std::lock_guard lock(mutex_);
  if (enabled_) {
    return;
  }

  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  i3c_master_config_t masterConfig;
  I3C_MasterGetDefaultConfig(&masterConfig);

  masterConfig.baudRate_Hz.i2cBaud = config_.i2c_baud_rate;
  masterConfig.baudRate_Hz.i3cOpenDrainBaud = config_.i3c_open_drain_baud_rate;
  masterConfig.baudRate_Hz.i3cPushPullBaud = config_.i3c_push_pull_baud_rate;
  masterConfig.enableOpenDrainStop = config_.enable_open_drain_stop;
  masterConfig.enableOpenDrainHigh = config_.enable_open_drain_high;

  I3C_MasterInit(base_, &masterConfig, CLOCK_GetI3cClkFreq());

  // The I3C handle differs in that it takes a struct of three callbacks.
  initiator_callbacks_ = {
      .slave2Master = nullptr,
      .ibiCallback = nullptr,
      .transferComplete = I3cMcuxpressoInitiator::TransferCompleteCallback};

  // Create the handle for the non-blocking transfer and register callback.
#if PW_I2C_MCUXPRESSO_I3C_USE_DMA
  I3C_MasterTransferCreateHandleDMA(base_,
                                    &handle_,
                                    &initiator_callbacks_,
                                    this,
                                    config_.rx_dma_channel.handle(),
                                    config_.tx_dma_channel.handle());
#else   // PW_I2C_MCUXPRESSO_I3C_USE_DMA
  I3C_MasterTransferCreateHandle(base_, &handle_, &initiator_callbacks_, this);
#endif  // PW_I2C_MCUXPRESSO_I3C_USE_DMA

  enabled_ = true;
}

void I3cMcuxpressoInitiator::Disable() {
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return;
  }

  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  I3C_MasterDeinit(base_);
  enabled_ = false;
}

I3cMcuxpressoInitiator::~I3cMcuxpressoInitiator() { Disable(); }

void I3cMcuxpressoInitiator::TransferCompleteCallback(
    I3C_Type*,
    I3cMcuxpressoInitiator::Handle*,
    status_t status,
    void* initiator_ptr) {
  I3cMcuxpressoInitiator& initiator =
      *static_cast<I3cMcuxpressoInitiator*>(initiator_ptr);
  initiator.transfer_status_ = status;

  // We cannot release clock_tree_element_ here since we are in an ISR.
  // It is released where callback_complete_notification_ is waited on.
  initiator.callback_complete_notification_.release();
}

Status I3cMcuxpressoInitiator::InitiateNonBlockingTransferUntil(
    chrono::SystemClock::time_point deadline, i3c_master_transfer_t* transfer) {
  // Acquire the clock_tree_element. Make sure it's released on any function
  // exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  const status_t status = I3C_MasterTransferFunc(base_, &handle_, transfer);
  if (status != kStatus_Success) {
    return HalStatusToPwStatus(status);
  }

  if (!callback_complete_notification_.try_acquire_until(deadline)) {
    I3C_MasterTransferAbortFunc(base_, &handle_);
    return Status::DeadlineExceeded();
  }

  if (transfer_status_ != kStatus_Success) {
    PW_LOG_INFO("NonBlockingTransfer failed transfer status %d",
                transfer_status_.load());
  }
  return HalStatusToPwStatus(transfer_status_);
}

pw::Status I3cMcuxpressoInitiator::SetDynamicAddressList(
    pw::span<const Address> dynamic_address_list) {
  pw::Vector<Address, I3C_MAX_DEVCNT> address_list_temp;
  size_t dynamic_address_num = dynamic_address_list.size();
  if (dynamic_address_num > I3C_MAX_DEVCNT) {
    PW_LOG_WARN("Only the first %d dynamic addresses are accepted",
                I3C_MAX_DEVCNT);
    dynamic_address_num = I3C_MAX_DEVCNT;
  }
  address_list_temp.resize(dynamic_address_num, Address(0));
  std::copy(dynamic_address_list.begin(),
            dynamic_address_list.begin() + dynamic_address_num,
            address_list_temp.begin());
  i3c_dynamic_address_list_.emplace(address_list_temp);

  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::SetDynamicAddressList(
    pw::span<const uint8_t> dynamic_address_list) {
  pw::Vector<Address, I3C_MAX_DEVCNT> address_list_temp;
  size_t dynamic_address_num = dynamic_address_list.size();
  if (dynamic_address_num > I3C_MAX_DEVCNT) {
    PW_LOG_WARN("Only the first %d dynamic addresses are accepted",
                I3C_MAX_DEVCNT);
    dynamic_address_num = I3C_MAX_DEVCNT;
  }
  address_list_temp.resize(dynamic_address_num, Address(0));
  for (size_t i = 0; i < dynamic_address_num; ++i) {
    address_list_temp[i] =
        Address::SevenBit(static_cast<uint16_t>(dynamic_address_list[i]));
  }
  i3c_dynamic_address_list_.emplace(address_list_temp);

  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::SetStaticAddressList(
    pw::span<const Address> static_address_list) {
  pw::Vector<Address, I3C_MAX_DEVCNT> address_list_temp;
  size_t static_address_num = static_address_list.size();
  if (static_address_num > I3C_MAX_DEVCNT) {
    PW_LOG_WARN("Only the first %d static addresses are accepted",
                I3C_MAX_DEVCNT);
    static_address_num = I3C_MAX_DEVCNT;
  }
  address_list_temp.resize(static_address_num, Address(0));
  std::copy(static_address_list.begin(),
            static_address_list.begin() + static_address_num,
            address_list_temp.begin());
  i3c_static_address_list_.emplace(address_list_temp);

  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::DoSetDasa(pw::i2c::Address static_addr) {
  std::array<std::byte, 1> dasa_buffer = {
      static_cast<std::byte>(static_addr.GetAddress() << 1)};
  PW_TRY(DoTransferCcc(
      I3cCccAction::kWrite, I3cCcc::kSetdasaDirect, static_addr, dasa_buffer));

  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::DoResetAddressing() {
  if (pw::Status status = DoTransferCcc(I3cCccAction::kWrite,
                                        I3cCcc::kRstdaaBroadcast,
                                        kBroadcastAddress,
                                        pw::ByteSpan());
      !(status.ok())) {
    if (status != pw::Status::Unavailable()) {
      return status;
    }
    PW_LOG_WARN("Failed to broadcast first RSTDAA, trying again...");
    PW_TRY(DoTransferCcc(I3cCccAction::kWrite,
                         I3cCcc::kRstdaaBroadcast,
                         kBroadcastAddress,
                         pw::ByteSpan()));
  }
  i3c_assigned_addresses_.clear();
  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::ResetAddressing() {
  std::lock_guard lock(mutex_);
  return DoResetAddressing();
}

pw::Status I3cMcuxpressoInitiator::AddAssignedI3cAddress(
    pw::i2c::Address address) {
  if (i3c_assigned_addresses_.full()) {
    PW_LOG_ERROR("Too many i3c devices on bus, can't add 0x%02x",
                 address.GetAddress());
    return pw::Status::ResourceExhausted();
  }
  if (std::find(i3c_assigned_addresses_.begin(),
                i3c_assigned_addresses_.end(),
                address) == i3c_assigned_addresses_.end()) {
    i3c_assigned_addresses_.push_back(address);
  }
  return pw::OkStatus();
}

void I3cMcuxpressoInitiator::ForgetAssignedAddress(pw::i2c::Address address) {
  auto to_erase = std::find(
      i3c_assigned_addresses_.begin(), i3c_assigned_addresses_.end(), address);
  if (to_erase != i3c_assigned_addresses_.end()) {
    i3c_assigned_addresses_.erase(to_erase);
  }
}

pw::Status I3cMcuxpressoInitiator::SetMaxReadLength(pw::i2c::Address address,
                                                    uint16_t max_read_length) {
  std::lock_guard lock(mutex_);
  std::array<std::byte, 2> writemrl_buffer = {
      static_cast<std::byte>(max_read_length >> 8),
      static_cast<std::byte>(max_read_length & 0xff),
  };
  return DoTransferCcc(
      I3cCccAction::kWrite, I3cCcc::kSetmrlDirect, address, writemrl_buffer);
}

pw::Result<uint16_t> I3cMcuxpressoInitiator::GetMaxReadLength(
    pw::i2c::Address address) {
  std::lock_guard lock(mutex_);
  std::array<std::byte, 3> readmrl_buffer;
  PW_TRY(DoTransferCcc(
      I3cCccAction::kRead, I3cCcc::kGetmrlDirect, address, readmrl_buffer));
  return static_cast<uint8_t>(readmrl_buffer[0]) << 8 |
         static_cast<uint8_t>(readmrl_buffer[1]);
}

pw::Status I3cMcuxpressoInitiator::SetMaxWriteLength(
    pw::i2c::Address address, uint16_t max_write_length) {
  std::lock_guard lock(mutex_);
  std::array<std::byte, 2> writewrl_buffer = {
      static_cast<std::byte>(max_write_length >> 8),
      static_cast<std::byte>(max_write_length & 0xff),
  };
  return DoTransferCcc(
      I3cCccAction::kWrite, I3cCcc::kSetmwlDirect, address, writewrl_buffer);
}

pw::Result<uint16_t> I3cMcuxpressoInitiator::GetMaxWriteLength(
    pw::i2c::Address address) {
  std::lock_guard lock(mutex_);
  std::array<std::byte, 2> readmwl_buffer;
  PW_TRY(DoTransferCcc(
      I3cCccAction::kRead, I3cCcc::kGetmwlDirect, address, readmwl_buffer));
  return static_cast<uint8_t>(readmwl_buffer[0]) << 8 |
         static_cast<uint8_t>(readmwl_buffer[1]);
}

pw::Status I3cMcuxpressoInitiator::Initialize() {
  std::lock_guard lock(mutex_);
  i3c_master_config_t masterConfig;

  // Acquire the clock_tree_element. Make sure it's released on any function
  // exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  // Initialize I3C master with low I3C speed to match I3C timing requirement
  // (mipi_I3C-Basic_specification_v1-1-1 section 6.2 Table 86 I3C Open Drain
  // Timing Parameters)
  I3C_MasterGetDefaultConfig(&masterConfig);
  masterConfig.baudRate_Hz.i2cBaud = config_.i2c_baud_rate;
  masterConfig.baudRate_Hz.i3cOpenDrainBaud = kI3cInitOpenDrainBaudRate;
  masterConfig.baudRate_Hz.i3cPushPullBaud = kI3cInitPushPullBaudRate;
  masterConfig.enableOpenDrainStop = config_.enable_open_drain_stop;
  masterConfig.enableOpenDrainHigh = kI3cInitEnableOpenDrainHigh;
  I3C_MasterInit(base_, &masterConfig, CLOCK_GetI3cClkFreq());

  PW_TRY(DoResetAddressing());

  // Broadcast DISEC 0x0b
  PW_TRY(DoTransferCcc(I3cCccAction::kWrite,
                       I3cCcc::kDisecBroadcast,
                       kBroadcastAddress,
                       kDisecBuffer));

  // SETDASA
  if (i3c_static_address_list_.has_value()) {
    for (const Address static_addr : *i3c_static_address_list_) {
      PW_TRY(DoSetDasa(static_addr));
    }
  }

  status_t hal_status = kStatus_Success;
  if (i3c_dynamic_address_list_.has_value() &&
      !i3c_dynamic_address_list_->empty()) {
    // ENTDAA
    std::array<uint8_t, I3C_MAX_DEVCNT> address_list;
    for (size_t i = 0; i < i3c_dynamic_address_list_->size(); ++i) {
      address_list[i] = i3c_dynamic_address_list_->at(i).GetAddress();
    }
    hal_status =
        I3C_MasterProcessDAA(base_, address_list.data(), address_list.size());
    if (hal_status != kStatus_Success) {
      PW_LOG_ERROR("Failed to initialize the I3C bus... %ld", hal_status);
    }

    // Examine the found devices
    uint8_t dev_count = 0;
    i3c_device_info_t* devlist =
        I3C_MasterGetDeviceListAfterDAA(base_, &dev_count);
    for (uint8_t i = 0; i < dev_count; ++i) {
      i3c_device_info_t* info = devlist + i;
      PW_TRY(
          AddAssignedI3cAddress(pw::i2c::Address::SevenBit(info->dynamicAddr)));
      PW_LOG_INFO("Found dynamic i3c device: 0x%02x vendor=0x%04x",
                  info->dynamicAddr,
                  info->vendorID);
    }
  }

  // Re-initialize I3C master with user provided speed.
  I3C_MasterGetDefaultConfig(&masterConfig);
  masterConfig.baudRate_Hz.i2cBaud = config_.i2c_baud_rate;
  masterConfig.baudRate_Hz.i3cOpenDrainBaud = config_.i3c_open_drain_baud_rate;
  masterConfig.baudRate_Hz.i3cPushPullBaud = config_.i3c_push_pull_baud_rate;
  masterConfig.enableOpenDrainStop = config_.enable_open_drain_stop;
  masterConfig.enableOpenDrainHigh = config_.enable_open_drain_high;
  I3C_MasterInit(base_, &masterConfig, CLOCK_GetI3cClkFreq());

  return HalStatusToPwStatus(hal_status);
}

pw::Status I3cMcuxpressoInitiator::SetDasa(pw::i2c::Address static_addr) {
  std::lock_guard lock(mutex_);
  PW_TRY(DoSetDasa(static_addr));
  PW_TRY(AddAssignedI3cAddress(static_addr));
  return pw::OkStatus();
}

pw::Status I3cMcuxpressoInitiator::DoTransferCcc(I3cCccAction rnw,
                                                 I3cCcc ccc_id,
                                                 pw::i2c::Address address,
                                                 pw::ByteSpan buffer) {
  // CCC commands are all small packets that should not incur any delay.
  // Hitting a timeout is unexpected. We set a large delay here so that
  // any timeout is noticeable.
  constexpr auto kCccTimeout = std::chrono::seconds(1);

  chrono::SystemClock::time_point kCccDeadline =
      chrono::SystemClock::TimePointAfterAtLeast(kCccTimeout);

  status_t status;
  i3c_master_transfer_t transfer;

  if (!enabled_) {
    return pw::Status::FailedPrecondition();
  }

  // Acquire the clock_tree_element. Make sure it's released on any function
  // exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  if (!(to_underlying(ccc_id) & kCccDirectBit)) {  // broadcast
    transfer.flags = kI3C_TransferDefaultFlag;
    transfer.slaveAddress = kBroadcastAddressRaw;
    transfer.direction = kI3C_Write;
    transfer.subaddress = static_cast<uint32_t>(ccc_id);
    transfer.subaddressSize = 1;
    transfer.data = buffer.data();
    transfer.dataSize = buffer.size();
    transfer.busType = kI3C_TypeI3CSdr;
    status = I3C_MasterTransferBlocking(base_, &transfer);
  } else {  // direct
    transfer.flags = kI3C_TransferNoStopFlag;
    transfer.slaveAddress = kBroadcastAddressRaw;
    transfer.direction = kI3C_Write;
    transfer.subaddress = static_cast<uint32_t>(ccc_id);
    transfer.subaddressSize = 1;
    transfer.data = nullptr;
    transfer.dataSize = 0;
    transfer.busType = kI3C_TypeI3CSdr;

    PW_TRY(InitiateNonBlockingTransferUntil(kCccDeadline, &transfer));

    transfer.flags = kI3C_TransferRepeatedStartFlag;
    transfer.slaveAddress = uint32_t{address.GetSevenBit()};
    transfer.direction = (rnw == I3cCccAction::kWrite) ? kI3C_Write : kI3C_Read;
    transfer.subaddress = 0;
    transfer.subaddressSize = 0;
    transfer.data = buffer.data();
    transfer.dataSize = buffer.size();
    transfer.busType = kI3C_TypeI3CSdr;

    return InitiateNonBlockingTransferUntil(kCccDeadline, &transfer);
  }
  return HalStatusToPwStatus(status);
}

// Performs a sequence of non-blocking I3C reads and writes.
Status I3cMcuxpressoInitiator::DoTransferFor(
    span<const Message> messages, chrono::SystemClock::duration timeout) {
  PW_TRY_ASSIGN(i3c_bus_type_t bus_type,
                ValidateAndDetermineProtocol(messages));

  chrono::SystemClock::time_point deadline =
      chrono::SystemClock::TimePointAfterAtLeast(timeout);
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return pw::Status::FailedPrecondition();
  }

  // Acquire the clock_tree element here so we don't acquire/release it for
  // each message. Make sure it's released on any function exits through a
  // scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  for (unsigned int i = 0; i < messages.size(); ++i) {
    if (chrono::SystemClock::now() > deadline) {
      return pw::Status::DeadlineExceeded();
    }

    const Message& msg = messages[i];

    uint32_t i3c_flags = kI3C_TransferDefaultFlag;

    if (msg.IsWriteContinuation()) {
      i3c_flags |= kI3C_TransferNoStartFlag;
    } else if (i > 0) {
      // Use repeated start flag for all but the first message.
      i3c_flags |= kI3C_TransferRepeatedStartFlag;
    }

    // No stop flag prior to the final message.
    if (i < messages.size() - 1) {
      i3c_flags |= kI3C_TransferNoStopFlag;
    }

    i3c_master_transfer_t transfer{
        .flags = i3c_flags,
        .slaveAddress =
            msg.GetAddress().GetSevenBit(),  // Will CHECK if >7 bits.
        .direction = msg.IsRead() ? kI3C_Read : kI3C_Write,
        .subaddress = 0,
        .subaddressSize = 0,
        // Cast GetData() here because GetMutableData() is for Writes only.
        .data = const_cast<std::byte*>(msg.GetData().data()),
        .dataSize = msg.GetData().size(),
        .busType = bus_type,
        .ibiResponse = kI3C_IbiRespNack};

    if (bus_type == kI3C_TypeI2C) {
      // The i2c mode is not working with NonBlocking transfers.
      // TODO(b/406239331): investigate NonBlocking issue further.
      status_t sdk_status = I3C_MasterTransferBlocking(base_, &transfer);
      PW_TRY(HalStatusToPwStatus(sdk_status));
    } else {
      PW_TRY(InitiateNonBlockingTransferUntil(deadline, &transfer));
    }
  }

  return pw::OkStatus();
}

pw::Result<i3c_bus_type_t> I3cMcuxpressoInitiator::ValidateAndDetermineProtocol(
    span<const Message> messages) const {
  // Establish whether the address is an i2c or i3c client, and that all
  // messages are of that same type.
  i3c_bus_type_t bus_type = kI3C_TypeI2C;
  for (unsigned j = 0; j < messages.size(); ++j) {
    if (j > 0 && messages[j].GetAddress() == messages[j - 1].GetAddress()) {
      // Optimization: the most likely case is that all messages have the same
      // address, don't search the dynamic address list again.
      continue;
    }

    // Search the dynamic address list to see if this is an i3c client.
    i3c_bus_type_t current_bus_type;
    if (std::find(i3c_assigned_addresses_.begin(),
                  i3c_assigned_addresses_.end(),
                  messages[j].GetAddress()) != i3c_assigned_addresses_.end()) {
      current_bus_type = kI3C_TypeI3CSdr;
    } else {
      current_bus_type = kI3C_TypeI2C;
    }

    if (j == 0) {
      bus_type = current_bus_type;
    } else if (current_bus_type != bus_type) {
      // i2c/i3c type doesn't match between messages.
      PW_LOG_ERROR("Mismatch of i2c/i3c messages in call.");
      return pw::Status::InvalidArgument();
    }
  }
  return bus_type;
}
// inclusive-language: enable

}  // namespace pw::i2c
