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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream.h"

#include <pw_assert/check.h>

#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_inbound_packet_assembler.h"
#include "pw_bytes/span.h"

namespace bt::iso {

// These values are unfortunately not available for extracting from the emboss
// definition directly.
constexpr size_t kTimestampSize = 4;
constexpr size_t kSduHeaderSize = 4;
constexpr size_t kFrameHeaderSize =
    pw::bluetooth::emboss::IsoDataFrameHeaderView::SizeInBytes();

// The next few functions are helpers for determining the size of packets and
// the buffer space required to send them.
//
// BT Core spec v5.4, Vol 4, Part E
//
// Sec 4.1.1
//   The ISO_Data_Packet_Length parameter [...] specifies the maximum buffer
//   size for each HCI ISO Data packet (excluding the header but including
//   optional fields such as ISO_SDU_Length).
//
// Sec 5.4.5
//   In the Host to Controller direction, Data_Total_Length shall be less than
//   or equal to the size of the buffer supported by the Controller (which is
//   returned using the ISO_Data_Packet_Length return parameter [...].
constexpr size_t OptionalFieldLength(bool has_timestamp, bool has_sdu_header) {
  return (has_timestamp ? kTimestampSize : 0) +
         (has_sdu_header ? kSduHeaderSize : 0);
}
constexpr size_t TotalDataLength(bool has_timestamp,
                                 bool has_sdu_header,
                                 size_t data_size) {
  // The total data length is the size of the payload plus optional fields.
  return OptionalFieldLength(has_timestamp, has_sdu_header) + data_size;
}
constexpr size_t TotalPacketSize(bool has_timestamp,
                                 bool has_sdu_header,
                                 size_t data_size) {
  // The entire packet also contains a fixed size header, this is not included
  // when calculating the size/maximum size for the controller buffers.
  return kFrameHeaderSize +
         TotalDataLength(has_timestamp, has_sdu_header, data_size);
}
constexpr size_t FragmentDataLength(bool has_timestamp,
                                    bool has_sdu_header,
                                    size_t data_size) {
  // The length of the actual SDU data contained in the fragment.
  return data_size - OptionalFieldLength(has_timestamp, has_sdu_header);
}

// Return two subspans of the provided span, the first containing elements
// indexed by the interval [0, at), the second containing the elements indexed
// by the interval [at, size()).
template <typename T>
std::pair<pw::span<T>, pw::span<T>> SplitSpan(pw::span<T> span, size_t at) {
  if (at > span.size()) {
    at = span.size();
  }

  return {span.subspan(0, at), span.subspan(at)};
}

class IsoStreamImpl final : public IsoStream {
 public:
  IsoStreamImpl(uint8_t cig_id,
                uint8_t cis_id,
                hci_spec::ConnectionHandle cis_handle,
                hci::Transport::WeakPtr hci,
                pw::async::Dispatcher& dispatcher,
                CisEstablishedCallback on_established_cb,
                pw::Callback<void()> on_closed_cb,
                pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);

  // IsoStream overrides
  bool OnCisEstablished(const hci::EventPacket& event) override;
  void SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection direction,
      const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
      const std::optional<std::vector<uint8_t>>& codec_configuration,
      uint32_t controller_delay_usecs,
      SetupDataPathCallback&& on_complete_cb,
      IncomingDataHandler&& on_incoming_data_available_cb) override;
  hci_spec::ConnectionHandle cis_handle() const override {
    return cis_hci_handle_;
  }
  bool is_established() const override {
    return state_ == IsoStreamState::kEstablished;
  }
  void Close() override;
  std::optional<IsoDataPacket> ReadNextQueuedIncomingPacket() override;
  void Send(pw::ConstByteSpan data) override;
  IsoStream::WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

  // IsoDataChannel::ConnectionInterface override
  void ReceiveInboundPacket(pw::span<const std::byte> packet) override;
  std::optional<DynamicByteBuffer> GetNextOutboundPdu() override;

 private:
  struct SduHeaderInfo {
    uint16_t packet_sequence_number;
    uint16_t iso_sdu_length;
  };

  void HandleCompletePacket(const pw::span<const std::byte>& packet);
  DynamicByteBuffer BuildPacketForSending(
      const pw::span<const std::byte>& data,
      pw::bluetooth::emboss::IsoDataPbFlag pb_flag,
      std::optional<SduHeaderInfo> sdu_header = std::nullopt,
      std::optional<uint32_t> time_stamp = std::nullopt);

  void UpdateWakeLease();

  enum class IsoStreamState {
    kNotEstablished,
    kEstablished,
    kClosing,
    kClosed,
  } state_;

  static const char* StateToString(IsoStreamState state) {
    switch (state) {
      case IsoStreamState::kNotEstablished:
        return "NotEstablished";
      case IsoStreamState::kEstablished:
        return "Established";
      case IsoStreamState::kClosing:
        return "Closing";
      case IsoStreamState::kClosed:
        return "Closed";
    }
  }

  uint8_t cig_id_ __attribute__((unused));
  uint8_t cis_id_ __attribute__((unused));

  // Connection parameters, only valid after CIS is established
  CisEstablishedParameters cis_params_;

  // Handle assigned by the controller
  hci_spec::ConnectionHandle cis_hci_handle_;

  // Called after HCI_LE_CIS_Established event is received and handled
  CisEstablishedCallback cis_established_cb_;

  IsoInboundPacketAssembler inbound_assembler_;

  IncomingDataHandler on_incoming_data_available_cb_;

  // When true, we will send a notification to the client when the next packet
  // arrives. Otherwise, we will just queue it up.
  bool inbound_client_is_waiting_ = false;

  std::queue<IsoDataPacket> incoming_data_queue_;
  std::queue<DynamicByteBuffer> outbound_pdu_queue_;

  // Called when stream is closed
  pw::Callback<void()> on_closed_cb_;

  // Has the data path been configured?
  enum class DataPathState {
    kNotSetUp,
    kSettingUp,
    kSetUp,
  };
  DataPathState input_data_path_state_ = DataPathState::kNotSetUp;
  DataPathState output_data_path_state_ = DataPathState::kNotSetUp;

  hci::CommandChannel::EventHandlerId cis_established_handler_;

  pw::chrono::SystemClock::time_point reference_time_;
  uint16_t next_sdu_sequence_number_ = 0;
  uint32_t iso_interval_usec_ = 0;
  // Created on HCI_LE_CIS_Established event with success status.
  std::optional<hci::Connection> link_;
  hci::Transport::WeakPtr hci_;
  pw::async::Dispatcher& dispatcher_;

  std::optional<pw::bluetooth_sapphire::Lease> wake_lease_;
  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  WeakSelf<IsoStreamImpl> weak_self_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(IsoStreamImpl);
};

IsoStreamImpl::IsoStreamImpl(
    uint8_t cig_id,
    uint8_t cis_id,
    hci_spec::ConnectionHandle cis_handle,
    hci::Transport::WeakPtr hci,
    pw::async::Dispatcher& dispatcher,
    CisEstablishedCallback on_established_cb,
    pw::Callback<void()> on_closed_cb,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : IsoStream(),
      state_(IsoStreamState::kNotEstablished),
      cig_id_(cig_id),
      cis_id_(cis_id),
      cis_hci_handle_(cis_handle),
      cis_established_cb_(std::move(on_established_cb)),
      inbound_assembler_(
          fit::bind_member<&IsoStreamImpl::HandleCompletePacket>(this)),
      on_closed_cb_(std::move(on_closed_cb)),
      hci_(std::move(hci)),
      dispatcher_(dispatcher),
      wake_lease_provider_(wake_lease_provider),
      weak_self_(this) {
  PW_CHECK(hci_.is_alive());

  auto weak_self = weak_self_.GetWeakPtr();
  cis_established_handler_ = hci_->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLECISEstablishedSubeventCode,
      [self = std::move(weak_self)](const hci::EventPacket& event) {
        if (!self.is_alive()) {
          return hci::CommandChannel::EventCallbackResult::kRemove;
        }
        if (self->OnCisEstablished(event)) {
          // On failure, this object will have been destroyed.
          return hci::CommandChannel::EventCallbackResult::kRemove;
        }
        return hci::CommandChannel::EventCallbackResult::kContinue;
      });
  PW_CHECK(cis_established_handler_ != 0u);
}

bool IsoStreamImpl::OnCisEstablished(const hci::EventPacket& event) {
  PW_CHECK(event.event_code() == hci_spec::kLEMetaEventCode);
  PW_CHECK(event.view<pw::bluetooth::emboss::LEMetaEventView>()
               .subevent_code()
               .Read() == hci_spec::kLECISEstablishedSubeventCode);
  auto view = event.view<pw::bluetooth::emboss::LECISEstablishedSubeventView>();

  // Ignore any events intended for another CIS
  hci_spec::ConnectionHandle handle = view.connection_handle().Read();
  if (handle != cis_hci_handle_) {
    bt_log(
        INFO,
        "iso",
        "Ignoring CIS established notification for handle 0x%x (target: 0x%x)",
        handle,
        cis_hci_handle_);

    // Event not handled
    return false;
  }

  pw::bluetooth::emboss::StatusCode status = view.status().Read();
  bt_log(INFO,
         "iso",
         "Handling CIS established notification for handle 0x%x (status: %s)",
         handle,
         hci_spec::StatusCodeToString(status).c_str());

  if (status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    cis_established_cb_(status, std::nullopt, std::nullopt);
    // Destroys this object.
    Close();
    return true;
  }

  state_ = IsoStreamState::kEstablished;

  auto on_disconnection_complete = [hci = hci_,
                                    cis_handle = cis_hci_handle_]() {
    hci->iso_data_channel()->ClearControllerPacketCount(cis_handle);
  };
  link_.emplace(
      cis_hci_handle_, hci_, dispatcher_, std::move(on_disconnection_complete));
  link_->set_peer_disconnect_callback(
      [this](const hci::Connection&, pw::bluetooth::emboss::StatusCode) {
        bt_log(INFO, "iso", "CIS Disconnected at handle %#x", cis_hci_handle_);
        if (state_ != IsoStreamState::kClosed) {
          state_ = IsoStreamState::kClosed;
          if (on_closed_cb_) {
            on_closed_cb_();
          }
        }
      });

  // General stream attributes
  cis_params_.cig_sync_delay = view.cig_sync_delay().Read();
  cis_params_.cis_sync_delay = view.cis_sync_delay().Read();
  cis_params_.max_subevents = view.nse().Read();
  cis_params_.iso_interval = view.iso_interval().Read();

  // Central => Peripheral stream attributes
  CisEstablishedParameters::CisUnidirectionalParams* params =
      &cis_params_.c_to_p_params;
  params->transport_latency = view.transport_latency_c_to_p().Read();
  params->phy = view.phy_c_to_p().Read();
  params->burst_number = view.bn_c_to_p().Read();
  params->flush_timeout = view.ft_c_to_p().Read();
  params->max_pdu_size = view.max_pdu_c_to_p().Read();

  // Peripheral => Central stream attributes
  params = &cis_params_.p_to_c_params;
  params->transport_latency = view.transport_latency_p_to_c().Read();
  params->phy = view.phy_p_to_c().Read();
  params->burst_number = view.bn_p_to_c().Read();
  params->flush_timeout = view.ft_p_to_c().Read();
  params->max_pdu_size = view.max_pdu_p_to_c().Read();

  cis_established_cb_(status, GetWeakPtr(), cis_params_);

  reference_time_ = dispatcher_.now();

  iso_interval_usec_ = cis_params_.iso_interval *
                       CisEstablishedParameters::kIsoIntervalToMicroseconds;

  // Event handled
  return true;
}

void IsoStreamImpl::SetupDataPath(
    pw::bluetooth::emboss::DataPathDirection direction,
    const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
    const std::optional<std::vector<uint8_t>>& codec_configuration,
    uint32_t controller_delay_usecs,
    SetupDataPathCallback&& on_complete_cb,
    IncomingDataHandler&& on_incoming_data_available_cb) {
  if (state_ != IsoStreamState::kEstablished) {
    bt_log(WARN, "iso", "failed to setup data path - CIS not established");
    on_complete_cb(kCisNotEstablished);
    return;
  }

  DataPathState* target_data_path_state;
  const char* direction_as_str;
  switch (direction) {
    case pw::bluetooth::emboss::DataPathDirection::INPUT:
      target_data_path_state = &input_data_path_state_;
      direction_as_str = "Input";
      break;
    case pw::bluetooth::emboss::DataPathDirection::OUTPUT:
      target_data_path_state = &output_data_path_state_;
      direction_as_str = "Output";
      break;
    default:
      bt_log(WARN,
             "iso",
             "invalid data path direction (%u)",
             static_cast<unsigned>(direction));
      on_complete_cb(kInvalidArgs);
      return;
  }

  if (*target_data_path_state != DataPathState::kNotSetUp) {
    bt_log(WARN,
           "iso",
           "attempt to setup %s CIS path - already setup",
           direction_as_str);
    on_complete_cb(kStreamAlreadyExists);
    return;
  }

  bt_log(INFO, "iso", "setting up CIS data path for %s", direction_as_str);
  size_t packet_size =
      pw::bluetooth::emboss::LESetupISODataPathCommand::MinSizeInBytes() +
      (codec_configuration.has_value() ? codec_configuration->size() : 0);
  auto cmd_packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LESetupISODataPathCommandWriter>(
      hci_spec::kLESetupISODataPath, packet_size);
  auto cmd_view = cmd_packet.view_t();
  cmd_view.connection_handle().Write(cis_hci_handle_);
  cmd_view.data_path_direction().Write(direction);
  cmd_view.data_path_id().Write(0);
  cmd_view.codec_id().CopyFrom(
      const_cast<bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>&>(
          codec_id)
          .view());
  cmd_view.controller_delay().Write(controller_delay_usecs);
  if (codec_configuration.has_value()) {
    cmd_view.codec_configuration_length().Write(codec_configuration->size());
    std::memcpy(cmd_view.codec_configuration().BackingStorage().data(),
                codec_configuration->data(),
                codec_configuration->size());
  } else {
    cmd_view.codec_configuration_length().Write(0);
  }

  *target_data_path_state = DataPathState::kSettingUp;
  WeakSelf<IsoStreamImpl>::WeakPtr self = weak_self_.GetWeakPtr();

  bt_log(INFO, "iso", "sending LE_Setup_ISO_Data_Path command");
  hci_->command_channel()
      ->SendCommand(
          std::move(cmd_packet),
          [on_complete_callback = std::move(on_complete_cb),
           self,
           cis_handle = cis_hci_handle_,
           target_data_path_state,
           direction,
           on_incoming_data_available_callback =
               std::move(on_incoming_data_available_cb)](
              auto, const hci::EventPacket& cmd_complete) mutable {
            if (!self.is_alive()) {
              on_complete_callback(kStreamClosed);
              return;
            }

            auto return_params =
                cmd_complete
                    .view<pw::bluetooth::emboss::
                              LESetupISODataPathCommandCompleteEventView>();
            pw::bluetooth::emboss::StatusCode status =
                return_params.status().Read();
            hci_spec::ConnectionHandle connection_handle =
                return_params.connection_handle().Read();

            if (status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
              bt_log(ERROR,
                     "iso",
                     "failed to setup ISO data path for handle 0x%x (status: "
                     "0x%x)",
                     connection_handle,
                     static_cast<uint8_t>(status));
              *target_data_path_state = DataPathState::kNotSetUp;
              on_complete_callback(kStreamRejectedByController);
              return;
            }

            // It's hard to know what is the right thing to do here. The
            // controller accepted our request, but we don't agree on the
            // connection handle ID. Something is amiss, so we will refuse to
            // consider the data path setup even though the controller may think
            // otherwise.
            if (connection_handle != cis_handle) {
              bt_log(ERROR,
                     "iso",
                     "handle mismatch in ISO data path setup completion "
                     "(expected: "
                     "0x%x, actual: %x)",
                     cis_handle,
                     connection_handle);
              *target_data_path_state = DataPathState::kNotSetUp;
              on_complete_callback(kStreamRejectedByController);
              return;
            }

            // Note that |direction| is a spec-defined value of dataflow
            // direction relative to the controller, so this may look backwards.
            if (direction == pw::bluetooth::emboss::DataPathDirection::OUTPUT) {
              self->on_incoming_data_available_cb_ =
                  std::move(on_incoming_data_available_callback);
            }
            *target_data_path_state = DataPathState::kSetUp;
            bt_log(INFO, "iso", "successfully set up data path");
            on_complete_callback(kSuccess);
          })
      .IgnoreError();
}

void IsoStreamImpl::ReceiveInboundPacket(pw::span<const std::byte> packet) {
  size_t packet_size = packet.size();
  auto packet_view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      packet.data(), packet.size());
  if (!packet_view.Ok()) {
    bt_log(ERROR,
           "iso",
           "Incoming ISO frame failed consistency checks - ignoring");
    return;
  }

  size_t data_total_length = packet_view.header().data_total_length().Read();
  size_t header_size =
      pw::bluetooth::emboss::IsoDataFrameHeaderView::SizeInBytes();
  size_t packet_actual_size = data_total_length + header_size;

  // This condition should have been caught by Emboss
  PW_CHECK(packet_size >= packet_actual_size,
           "Packet too short to hold data specified in header");

  // Truncate any extra data at end of packet
  if (packet_size > packet_actual_size) {
    packet = packet.subspan(0, packet_actual_size);
  }

  inbound_assembler_.ProcessNext(packet);
}

std::optional<DynamicByteBuffer> IsoStreamImpl::GetNextOutboundPdu() {
  if (outbound_pdu_queue_.empty()) {
    return std::nullopt;
  }
  DynamicByteBuffer pdu = std::move(outbound_pdu_queue_.front());
  outbound_pdu_queue_.pop();
  UpdateWakeLease();
  return pdu;
}

void IsoStreamImpl::HandleCompletePacket(
    const pw::span<const std::byte>& packet) {
  if (!on_incoming_data_available_cb_) {
    bt_log(WARN,
           "iso",
           "Incoming data received for stream whose data path has not yet been "
           "set up - ignoring");
    return;
  }

  if (inbound_client_is_waiting_) {
    inbound_client_is_waiting_ = false;
    if (on_incoming_data_available_cb_(packet)) {
      // Packet was processed successfully - we're done here
      return;
    }
    // This is not a hard error, but it is a bit unusual and probably worth
    // noting.
    bt_log(INFO,
           "iso",
           "ISO incoming packet client previously requested packets, now not "
           "accepting new ones");
  }

  // Client not ready to handle packet, queue it up until they ask for it
  incoming_data_queue_.emplace(packet.begin(), packet.end());
  UpdateWakeLease();
}

DynamicByteBuffer IsoStreamImpl::BuildPacketForSending(
    const pw::span<const std::byte>& data,
    pw::bluetooth::emboss::IsoDataPbFlag pb_flag,
    std::optional<SduHeaderInfo> sdu_header,
    std::optional<uint32_t> time_stamp) {
  PW_CHECK((pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT ||
            pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU) ==
               sdu_header.has_value(),
           "Header required for first and complete fragments.");

  DynamicByteBuffer packet(TotalPacketSize(
      time_stamp.has_value(), sdu_header.has_value(), data.size()));
  auto view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      packet.mutable_data(), packet.size());

  view.header().connection_handle().Write(cis_hci_handle_);
  view.header().pb_flag().Write(pb_flag);
  view.header().ts_flag().Write(
      time_stamp.has_value()
          ? pw::bluetooth::emboss::TsFlag::TIMESTAMP_PRESENT
          : pw::bluetooth::emboss::TsFlag::TIMESTAMP_NOT_PRESENT);
  view.header().data_total_length().Write(TotalDataLength(
      time_stamp.has_value(), sdu_header.has_value(), data.size()));

  if (time_stamp.has_value()) {
    view.time_stamp().Write(*time_stamp);
  }

  if (sdu_header.has_value()) {
    view.packet_sequence_number().Write(sdu_header->packet_sequence_number);
    view.iso_sdu_length().Write(sdu_header->iso_sdu_length);
    // This flag is RFU when sending to controller, the valid data flag is all
    // zeros as required (see BT Core spec v5.4, Vol 4, Part E, Sec 5.4.5).
    view.packet_status_flag().Write(
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA);
  }

  memcpy(view.iso_sdu_fragment().BackingStorage().data(),
         data.data(),
         data.size());

  return packet;
}

std::optional<IsoDataPacket> IsoStreamImpl::ReadNextQueuedIncomingPacket() {
  if (incoming_data_queue_.empty()) {
    inbound_client_is_waiting_ = true;
    return std::nullopt;
  }

  IsoDataPacket packet = std::move(incoming_data_queue_.front());
  incoming_data_queue_.pop();
  UpdateWakeLease();
  return packet;
}

void IsoStreamImpl::Send(pw::ConstByteSpan data) {
  PW_CHECK(data.size() <= std::numeric_limits<uint16_t>::max());
  const size_t max_length =
      hci_->iso_data_channel()->buffer_info().max_data_length();

  // Calculate the current interval sequence number
  auto now = dispatcher_.now();
  auto elapsed_time = now - reference_time_;
  uint64_t elapsed_usec =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed_time)
          .count();
  uint16_t interval_sequence_num =
      static_cast<uint16_t>(elapsed_usec / iso_interval_usec_);

  uint16_t current_sequence_num = next_sdu_sequence_number_;

  // Handle missed interval
  if (current_sequence_num < interval_sequence_num) {
    bt_log(INFO,
           "iso",
           "Skipped interval: advancing sequence number from %u to current "
           "interval %u",
           current_sequence_num,
           interval_sequence_num);
    current_sequence_num = interval_sequence_num;
  }

  std::optional<SduHeaderInfo> sdu_header = SduHeaderInfo{
      .packet_sequence_number = current_sequence_num,
      .iso_sdu_length = static_cast<uint16_t>(data.size()),
  };

  // Fragmentation loop.
  while (!data.empty()) {
    // Determine if this is the first fragment of the SDU
    const bool is_first = sdu_header.has_value();

    size_t length_remaining = TotalDataLength(/*has_timestamp=*/false,
                                              /*has_sdu_header=*/is_first,
                                              /*data_size=*/data.size());
    // This is the last fragment if there is sufficient buffer space.
    const bool is_last = length_remaining <= max_length;

    pw::bluetooth::emboss::IsoDataPbFlag flag;
    if (is_first && is_last) {
      flag = pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU;
    } else if (is_first && !is_last) {
      flag = pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT;
    } else if (!is_first && is_last) {
      flag = pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT;
    } else if (!is_first && !is_last) {
      flag = pw::bluetooth::emboss::IsoDataPbFlag::INTERMEDIATE_FRAGMENT;
    } else {
      PW_UNREACHABLE;
    }

    // Send the largest possible fragment, reduce `data` to remaining span.
    pw::ConstByteSpan fragment;
    size_t fragment_length = FragmentDataLength(false, is_first, max_length);
    std::tie(fragment, data) = SplitSpan(data, fragment_length);
    outbound_pdu_queue_.emplace(
        BuildPacketForSending(fragment, flag, sdu_header));
    sdu_header.reset();
  }
  next_sdu_sequence_number_ = current_sequence_num + 1;

  hci_->iso_data_channel()->TrySendPackets();

  UpdateWakeLease();
}

void IsoStreamImpl::Close() {
  if (state_ == IsoStreamState::kClosing || state_ == IsoStreamState::kClosed) {
    bt_log(DEBUG,
           "iso",
           "Close() called on stream (%#x) in state %s",
           cis_hci_handle_,
           StateToString(state_));
    return;
  }
  if (state_ == IsoStreamState::kEstablished) {
    state_ = IsoStreamState::kClosing;
    link_->Disconnect(
        pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
  } else {
    state_ = IsoStreamState::kClosed;
    on_closed_cb_();
  }
}

void IsoStreamImpl::UpdateWakeLease() {
  if (outbound_pdu_queue_.empty() && incoming_data_queue_.empty()) {
    wake_lease_.reset();
  } else if (!wake_lease_) {
    wake_lease_ = PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_, "IsoStream")
                      .value_or(pw::bluetooth_sapphire::Lease());
  }
}

std::unique_ptr<IsoStream> IsoStream::Create(
    uint8_t cig_id,
    uint8_t cis_id,
    hci_spec::ConnectionHandle cis_handle,
    hci::Transport::WeakPtr hci,
    pw::async::Dispatcher& dispatcher,
    CisEstablishedCallback on_established_cb,
    pw::Callback<void()> on_closed_cb,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider) {
  return std::make_unique<IsoStreamImpl>(cig_id,
                                         cis_id,
                                         cis_handle,
                                         std::move(hci),
                                         dispatcher,
                                         std::move(on_established_cb),
                                         std::move(on_closed_cb),
                                         wake_lease_provider);
}

}  // namespace bt::iso
