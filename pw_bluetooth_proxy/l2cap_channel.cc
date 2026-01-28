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

#include "pw_bluetooth_proxy/internal/l2cap_channel.h"

#include <mutex>
#include <optional>

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/internal/generic_l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy {

namespace {

template <class... Ts>
struct Visitors : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Visitors(Ts...) -> Visitors<Ts...>;

}  // namespace

L2capChannel::~L2capChannel() {
  // Block until there are no outstanding borrows. Callers (namely
  // L2capChannelManager) MUST NOT be holding the `static_mutex_` when this
  // destructor is called.
  std::unique_lock lock(impl_.mutex_);
  impl_.BlockWhileBorrowed(lock);

  PW_LOG_INFO(
      "btproxy: L2capChannel dtor - transport_: %u, connection_handle_ : "
      "%#x, local_cid_: %#x, remote_cid_: %#x, state_: %u",
      cpp23::to_underlying(transport_),
      connection_handle(),
      local_cid(),
      remote_cid(),
      cpp23::to_underlying(state_));

  // Most channels are explicitly closed, with the exception of the signaling
  // channels. Ensure those that are not are deregistered before destruction.
  if (state_ != State::kClosed) {
    l2cap_channel_manager_.DeregisterChannel(*this);
  }
  impl_.ClearQueue();
}

void L2capChannel::Stop() {
  std::lock_guard lock(impl_.mutex_);
  PW_LOG_INFO(
      "btproxy: L2capChannel::Stop - transport_: %u, connection_handle_: %#x, "
      "local_cid_: %#x, remote_cid_: %#x, previous state_: %u",
      cpp23::to_underlying(transport_),
      connection_handle(),
      local_cid(),
      remote_cid(),
      cpp23::to_underlying(state_));

  PW_CHECK(state_ != State::kNew && state_ != State::kClosed);
  state_ = State::kStopped;
  impl_.ClearQueue();
}

void L2capChannel::Close(L2capChannelEvent event) {
  {
    std::lock_guard lock(impl_.mutex_);
    PW_LOG_INFO(
        "btproxy: L2capChannel::Close - transport_: %u, "
        "connection_handle_: %#x, local_cid_: %#x, remote_cid_: %#x, previous "
        "state_: %u",
        cpp23::to_underlying(transport_),
        connection_handle(),
        local_cid(),
        remote_cid(),
        cpp23::to_underlying(state_));

    PW_CHECK(state_ != State::kNew);
    if (state_ == State::kClosed) {
      return;
    }
    state_ = State::kClosed;
    impl_.ClearQueue();
  }

  impl_.SendEvent(event);
  impl_.Close();
}

StatusWithMultiBuf L2capChannel::Write(FlatConstMultiBuf&& payload) {
  StatusWithMultiBuf result = WriteDuringRx(std::move(payload));
  DrainChannelQueuesIfNewTx();
  return result;
}

bool L2capChannel::HandlePduFromController(pw::span<uint8_t> l2cap_pdu) {
  if (state() != State::kRunning) {
    PW_LOG_ERROR(
        "btproxy: L2capChannel::OnPduReceivedFromController on non-running "
        "channel. local_cid: %#x, remote_cid: %#x, state: %u",
        local_cid(),
        remote_cid(),
        cpp23::to_underlying(state()));
    impl_.SendEvent(L2capChannelEvent::kRxWhileStopped);
    return true;
  }

  internal::RxEngine::HandlePduFromControllerReturnValue result;
  {
    std::lock_guard rx_lock(rx_mutex_);
    result = rx_engine().HandlePduFromController(l2cap_pdu);
  }

  return std::visit(
      Visitors{
          [](std::monostate) {
            // Do nothing and consume the packet.
            return true;
          },
          [this](L2capChannelEvent event) {
            StopAndSendEvent(event);
            // Consume the packet that caused the event.
            return true;
          },
          [this](FlatMultiBufInstance&& buffer) {
            // MultiBufs are only returned by CreditBasedFlowControlRxEngine,
            // which is used with PayloadReceiveCallback.
            if (auto* receive_fn =
                    std::get_if<MultiBufReceiveFunction>(&from_controller_fn_);
                *receive_fn != nullptr) {
              (*receive_fn)(std::move(MultiBufAdapter::Unwrap(buffer)));
            }
            return true;
          },
          [this](span<uint8_t> buffer) {
            return std::visit(
                Visitors{[](std::monostate) { return false; },
                         [this, buffer](OptionalPayloadReceiveCallback& cb) {
                           return SendPayloadToClient(buffer, &cb);
                         },
                         [this, buffer](OptionalBufferReceiveFunction& fn) {
                           return SendPayloadToClient(buffer, &fn);
                         },
                         [buffer](PayloadSpanReceiveCallback& cb) {
                           return cb(buffer);
                         },
                         [this, buffer](SpanReceiveFunction& fn) {
                           return fn(as_bytes(buffer),
                                     ConnectionHandle{connection_handle()},
                                     local_cid(),
                                     remote_cid());
                         },
                         [](MultiBufReceiveFunction&) {
                           // CreditBasedFlowControlRxEngine only uses MultiBufs
                           PW_CRASH("Invalid from controller callback");
                           return false;
                         }

                },
                from_controller_fn_);
          },
      },
      std::move(result));
}

L2capChannel::State L2capChannel::state() const {
  std::lock_guard lock(impl_.mutex_);
  return state_;
}

L2capChannel::L2capChannel(L2capChannelManager& l2cap_channel_manager,
                           MultiBufAllocator* rx_multibuf_allocator,
                           uint16_t connection_handle,
                           AclTransportType transport,
                           uint16_t local_cid,
                           uint16_t remote_cid,
                           ChannelEventCallback&& event_fn)
    : l2cap_channel_manager_(l2cap_channel_manager),
      transport_(transport),
      local_handle_(*this, MakeKey(connection_handle, local_cid)),
      remote_handle_(*this, MakeKey(connection_handle, remote_cid)),
      event_fn_(std::move(event_fn)),
      rx_multibuf_allocator_(rx_multibuf_allocator),
      impl_(*this) {
  PW_LOG_INFO(
      "btproxy: L2capChannel ctor - transport_: %u, connection_handle_ : %u, "
      "local_cid_ : %#x, remote_cid_: %#x",
      cpp23::to_underlying(transport_),
      connection_handle,
      local_cid,
      remote_cid);
  PW_CHECK(AreValidParameters(connection_handle, local_cid, remote_cid));
}

Status L2capChannel::Start() {
  PW_LOG_INFO(
      "btproxy: L2capChannel initialized: "
      "transport_: %u, connection_handle_ : %u, "
      "local_cid_ : %#x, remote_cid_: %#x",
      cpp23::to_underlying(transport_),
      connection_handle(),
      local_cid(),
      remote_cid());
  PW_TRY(l2cap_channel_manager_.RegisterChannel(*this));

  std::lock_guard lock(impl_.mutex_);
  state_ = State::kRunning;
  return OkStatus();
}

bool L2capChannel::AreValidParameters(uint16_t connection_handle,
                                      uint16_t local_cid,
                                      uint16_t remote_cid) {
  if (connection_handle > kMaxValidConnectionHandle) {
    PW_LOG_ERROR(
        "Invalid connection handle %#x. Maximum connection handle is 0x0EFF.",
        connection_handle);
    return false;
  }
  if (local_cid == 0 || remote_cid == 0) {
    PW_LOG_ERROR("L2CAP channel identifier 0 is not valid.");
    return false;
  }
  return true;
}

pw::Result<H4PacketWithH4> L2capChannel::PopulateTxL2capPacket(
    uint16_t data_length) {
  return PopulateL2capPacket(data_length);
}

namespace {

constexpr size_t H4SizeForL2capData(uint16_t data_length) {
  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + data_length;
  const size_t acl_packet_size =
      emboss::AclDataFrameHeader::IntrinsicSizeInBytes() + l2cap_packet_size;
  return sizeof(emboss::H4PacketType) + acl_packet_size;
}

}  // namespace

pw::Result<H4PacketWithH4> L2capChannel::PopulateL2capPacket(
    uint16_t data_length) {
  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + data_length;
  const size_t h4_packet_size = H4SizeForL2capData(data_length);

  pw::Result<H4PacketWithH4> h4_packet_res =
      l2cap_channel_manager_.GetAclH4Packet(h4_packet_size);
  if (!h4_packet_res.ok()) {
    return h4_packet_res.status();
  }
  H4PacketWithH4 h4_packet = std::move(h4_packet_res.value());
  h4_packet.SetH4Type(emboss::H4PacketType::ACL_DATA);

  PW_TRY_ASSIGN(
      auto acl,
      MakeEmbossWriter<emboss::AclDataFrameWriter>(h4_packet.GetHciSpan()));
  acl.header().handle().Write(connection_handle());
  // TODO: https://pwbug.dev/360932103 - Support packet segmentation, so this
  // value will not always be FIRST_NON_FLUSHABLE.
  acl.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  acl.header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  acl.data_total_length().Write(l2cap_packet_size);

  PW_TRY_ASSIGN(auto l2cap_header,
                MakeEmbossWriter<emboss::BasicL2capHeaderWriter>(
                    acl.payload().BackingStorage().data(),
                    emboss::BasicL2capHeader::IntrinsicSizeInBytes()));
  l2cap_header.pdu_length().Write(data_length);
  l2cap_header.channel_id().Write(remote_cid());

  return h4_packet;
}

std::optional<uint16_t> L2capChannel::MaxL2capPayloadSize() {
  std::optional<uint16_t> max_acl_length =
      channel_manager().MaxDataPacketLengthForTransport(transport());
  if (!max_acl_length.has_value()) {
    return std::nullopt;
  }
  if (*max_acl_length <= emboss::BasicL2capHeader::IntrinsicSizeInBytes()) {
    return std::nullopt;
  }
  return *max_acl_length - emboss::BasicL2capHeader::IntrinsicSizeInBytes();
}

void L2capChannel::ReportNewTxPacketsOrCredits() {
  impl_.ReportNewTxPacketsOrCredits();
}

void L2capChannel::DrainChannelQueuesIfNewTx() {
  l2cap_channel_manager_.DrainChannelQueuesIfNewTx();
}

//-------
//  Rx (protected)
//-------

bool L2capChannel::SendPayloadToClient(pw::span<uint8_t> payload,
                                       SendPayloadToClientCallback callback) {
  if (std::visit([](auto&& cb) { return *cb == nullptr; }, callback)) {
    return false;
  }

  if (!rx_multibuf_allocator_) {
    PW_LOG_ERROR(
        "btproxy: rx_multibuf_allocator_ is null so unable to create multibuf "
        "to pass to client. Will passthrough instead. "
        "connection: %#x, local_cid: %#x ",
        connection_handle(),
        local_cid());
    return false;
  }

  auto result =
      MultiBufAdapter::Create(*rx_multibuf_allocator_, payload.size());
  if (!result.has_value()) {
    PW_LOG_ERROR(
        "btproxy: rx_multibuf_allocator_ is out of memory. So stopping "
        "channel and reporting it needs to be closed."
        "connection: %#x, local_cid: %#x ",
        connection_handle(),
        local_cid());
    StopAndSendEvent(L2capChannelEvent::kRxOutOfMemory);
    return true;
  }

  FlatMultiBufInstance buffer = std::move(result.value());
  MultiBufAdapter::Copy(buffer, 0, as_bytes(payload));

  // If client returned multibuf to us, we copy it to the payload and indicate
  // to the caller that packet should be forwarded.
  // In the future when whole path is operating with multibuf's, we could pass
  // it back up to container to be forwarded and avoid the two copies of
  // payload.
  auto client_multibuf = std::visit(
      Visitors{[&buffer](OptionalPayloadReceiveCallback* cb) {
                 return (*cb)(std::move(MultiBufAdapter::Unwrap(buffer)));
               },
               [this, &buffer](OptionalBufferReceiveFunction* fn) {
                 return (*fn)(std::move(MultiBufAdapter::Unwrap(buffer)),
                              ConnectionHandle{connection_handle()},
                              local_cid(),
                              remote_cid());
               }},
      callback);
  if (client_multibuf.has_value()) {
    MultiBufAdapter::Copy(as_writable_bytes(payload), client_multibuf.value());
    return false;
  }
  return true;
}

pw::Status L2capChannel::StartRecombinationBuf(Direction direction,
                                               size_t payload_size,
                                               size_t extra_header_size) {
  std::optional<MultiBufInstance>& buf_optref =
      GetRecombinationBufOptRef(direction);
  PW_CHECK(!buf_optref.has_value());

  if (rx_multibuf_allocator_ == nullptr) {
    // TODO: https://pwbug.dev/423695410 - Should eventually recombine for these
    // cases to allow channel to make handle/unhandle decision.
    PW_LOG_WARN(
        "Cannot start recombination without an allocator."
        "connection: %#x, local_cid: %#x ",
        connection_handle(),
        local_cid());
    return Status::FailedPrecondition();
  }

  buf_optref = MultiBufAdapter::Create(
      *rx_multibuf_allocator_, extra_header_size, payload_size);
  if (!buf_optref.has_value()) {
    return Status::ResourceExhausted();
  }

  return pw::OkStatus();
}

void L2capChannel::EndRecombinationBuf(Direction direction) {
  GetRecombinationBufOptRef(direction) = std::nullopt;
}

Status L2capChannel::InitBasic(FromControllerFn&& from_controller_fn,
                               FromHostFn&& from_host_fn) {
  from_controller_fn_ = std::move(from_controller_fn);
  from_host_fn_ = std::move(from_host_fn);

  {
    std::lock_guard rx_lock(rx_mutex_);
    std::lock_guard lock(impl_.mutex_);
    tx_engine_.emplace<internal::BasicModeTxEngine>(
        connection_handle(), remote_cid(), *this);
    rx_engine_.emplace<internal::BasicModeRxEngine>(local_cid());
  }

  return impl_.Init();
}

Status L2capChannel::InitCreditBasedFlowControl(
    ConnectionOrientedChannelConfig rx_config,
    ConnectionOrientedChannelConfig tx_config,
    MultiBufReceiveFunction&& receive_fn) {
  if (tx_config.mps < emboss::L2capLeCreditBasedConnectionReq::min_mps() ||
      tx_config.mps > emboss::L2capLeCreditBasedConnectionReq::max_mps()) {
    PW_LOG_ERROR("Tx MPS (%" PRIu16
                 " octets) invalid. L2CAP implementations shall support a "
                 "minimum MPS of %" PRIi32
                 " octets and may support an MPS up to %" PRIi32 " octets.",
                 tx_config.mps,
                 emboss::L2capLeCreditBasedConnectionReq::min_mps(),
                 emboss::L2capLeCreditBasedConnectionReq::max_mps());
    return Status::InvalidArgument();
  }

  if (!rx_multibuf_allocator_) {
    return Status::FailedPrecondition();
  }

  from_controller_fn_.emplace<MultiBufReceiveFunction>(std::move(receive_fn));

  {
    std::lock_guard rx_lock(rx_mutex_);
    std::lock_guard lock(impl_.mutex_);
    tx_engine_.emplace<internal::CreditBasedFlowControlTxEngine>(
        tx_config, connection_handle(), local_cid(), *this);
    rx_engine_.emplace<internal::CreditBasedFlowControlRxEngine>(
        rx_config,
        *rx_multibuf_allocator_,
        pw::bind_member<&L2capChannel::ReplenishRxCredits>(this));
  }

  return impl_.Init();
}

Status L2capChannel::InitGattNotify(uint16_t attribute_handle) {
  if (attribute_handle == 0) {
    PW_LOG_ERROR("Attribute handle cannot be 0.");
    return pw::Status::InvalidArgument();
  }

  {
    std::lock_guard rx_lock(rx_mutex_);
    std::lock_guard lock(impl_.mutex_);
    rx_engine_.emplace<internal::GattNotifyRxEngine>();
    tx_engine_.emplace<internal::GattNotifyTxEngine>(
        connection_handle(), remote_cid(), attribute_handle, *this);
  }

  return impl_.Init();
}

Status L2capChannel::ReplenishRxCredits(uint16_t credits) {
  PW_CHECK(rx_multibuf_allocator());
  // SendFlowControlCreditInd logs if status is not ok, so no need to log here.
  return channel_manager().SendFlowControlCreditInd(
      connection_handle(), local_cid(), credits, *rx_multibuf_allocator());
}

Result<H4PacketWithH4> L2capChannel::AllocateH4(uint16_t length) {
  return l2cap_channel_manager_.GetAclH4Packet(length);
}

internal::RxEngine& L2capChannel::rx_engine() {
  return std::visit(
      [](auto&& arg) -> internal::RxEngine& {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          PW_CRASH("RxEngine is monostate");
        else
          return arg;
      },
      rx_engine_);
}

internal::TxEngine& L2capChannel::tx_engine() {
  return std::visit(
      [](auto&& arg) -> internal::TxEngine& {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          PW_CRASH("TxEngine is monostate");
        else
          return arg;
      },
      tx_engine_);
}

bool L2capChannel::HandlePduFromHost(pw::span<uint8_t> l2cap_pdu) {
  internal::TxEngine::HandlePduFromHostReturnValue result;
  {
    std::lock_guard lock(impl_.mutex_);
    result = tx_engine().HandlePduFromHost(l2cap_pdu);
  }

  if (!result.send_to_client.has_value()) {
    return !result.forward_to_controller;
  }
  span<uint8_t> buffer = result.send_to_client.value();

  return std::visit(
      Visitors{[](std::monostate) { return false; },
               [this, &buffer](OptionalPayloadReceiveCallback& cb) {
                 return SendPayloadToClient(buffer, &cb);
               },
               [this, &buffer](OptionalBufferReceiveFunction& fn) {
                 return SendPayloadToClient(buffer, &fn);
               },
               [&buffer](PayloadSpanReceiveCallback& cb) { return cb(buffer); },
               [this, &buffer](SpanReceiveFunction& fn) {
                 return fn(as_bytes(buffer),
                           ConnectionHandle{connection_handle()},
                           local_cid(),
                           remote_cid());
               }},
      from_host_fn_);
}

Status L2capChannel::AddTxCredits(uint16_t credits) {
  Result<bool> result;
  {
    std::lock_guard lock(impl_.mutex_);
    result = tx_engine().AddCredits(credits);
  }
  if (!result.ok()) {
    StopAndSendEvent(L2capChannelEvent::kRxInvalid);
    return result.status();
  }
  if (result.value()) {
    ReportNewTxPacketsOrCredits();
  }
  return OkStatus();
}

Status L2capChannel::SendAdditionalRxCredits(uint16_t additional_rx_credits) {
  if (state() != State::kRunning) {
    return Status::FailedPrecondition();
  }
  std::lock_guard lock(rx_mutex_);
  Status status = ReplenishRxCredits(additional_rx_credits);

  if (status.ok()) {
    status = rx_engine().AddRxCredits(additional_rx_credits);
  }

  DrainChannelQueuesIfNewTx();
  return status;
}

std::optional<H4PacketWithH4> L2capChannel::GenerateNextTxPacket(
    const FlatConstMultiBuf& payload, bool& keep_payload) {
  Result<H4PacketWithH4> result =
      tx_engine().GenerateNextPacket(payload, keep_payload);
  if (!result.ok()) {
    // TODO: https://pwbug.dev/450060983 - Return the result
    return std::nullopt;
  }
  return std::move(result.value());
}

}  // namespace pw::bluetooth::proxy
