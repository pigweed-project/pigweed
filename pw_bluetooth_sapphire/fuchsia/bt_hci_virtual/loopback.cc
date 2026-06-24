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

#include "loopback.h"

#include <lib/driver/logging/cpp/logger.h>

namespace bt_hci_virtual {

LoopbackDevice::LoopbackDevice()
    : dispatcher_(fdf::Dispatcher::GetCurrent()->async_dispatcher()),
      devfs_connector_(fit::bind_member<&LoopbackDevice::Connect>(this)) {}

zx_status_t LoopbackDevice::Initialize(zx::channel channel,
                                       std::string_view name,
                                       AddChildCallback callback) {
  // Setup up incoming channel waiter
  loopback_chan_ = std::move(channel);
  loopback_chan_wait_.set_object(loopback_chan_.get());
  loopback_chan_wait_.set_trigger(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED);
  ZX_ASSERT(loopback_chan_wait_.Begin(dispatcher_) == ZX_OK);

  // Create args to add loopback as a child node on behalf of VirtualController
  zx::result connector = devfs_connector_.Bind(dispatcher_);
  if (connector.is_error()) {
    fdf::error("Failed to bind devfs connecter to dispatcher: {}", connector);
    return connector.error_value();
  }

  fidl::Arena args_arena;
  auto devfs = fuchsia_driver_framework::wire::DevfsAddArgs::Builder(args_arena)
                   .connector(std::move(connector.value()))
                   .class_name("bt-hci")
                   .Build();
  auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(args_arena)
                  .name(name.data())
                  .devfs_args(devfs)
                  .Build();

  callback(args);

  return ZX_OK;
}

LoopbackDevice::SnoopServer::SnoopServer(
    fidl::ServerEnd<fuchsia_hardware_bluetooth::Snoop> server_end,
    LoopbackDevice* device)
    : binding_(device->dispatcher_,
               std::move(server_end),
               this,
               std::mem_fn(&LoopbackDevice::SnoopServer::OnFidlError)),
      device_(device) {}

void LoopbackDevice::SnoopServer::QueueSnoopPacket(
    uint8_t* buffer,
    size_t length,
    PacketIndicator indicator,
    fuchsia_hardware_bluetooth::PacketDirection direction) {
  uint64_t sequence = next_sequence_++;

  if (sequence > acked_sequence_ + kMaxSnoopUnackedPackets) {
    if (queued_packets_.size() >= kMaxSnoopQueueSize) {
      // Drop oldest packet.
      fuchsia_hardware_bluetooth::PacketDirection direction =
          queued_packets_.front().direction;
      queued_packets_.pop();
      if (direction ==
          fuchsia_hardware_bluetooth::PacketDirection::kHostToController) {
        dropped_sent_++;
      } else {
        dropped_received_++;
      }
    }
    std::vector<uint8_t> packet(buffer, buffer + length);
    queued_packets_.push(
        Packet{std::move(packet), sequence, indicator, direction});
    return;
  }

  SendSnoopPacket(buffer, length, indicator, direction, sequence);
}

void LoopbackDevice::SnoopServer::SendSnoopPacket(
    uint8_t* buffer,
    size_t length,
    PacketIndicator indicator,
    fuchsia_hardware_bluetooth::PacketDirection direction,
    uint64_t sequence) {
  fidl::VectorView vec_view =
      fidl::VectorView<uint8_t>::FromExternal(buffer, length);
  fidl::ObjectView obj_view =
      fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&vec_view);
  fuchsia_hardware_bluetooth::wire::SnoopPacket packet;
  switch (indicator) {
    case PacketIndicator::kHciCommand:
      packet =
          fuchsia_hardware_bluetooth::wire::SnoopPacket::WithCommand(obj_view);
      break;
    case PacketIndicator::kHciAclData:
      packet = fuchsia_hardware_bluetooth::wire::SnoopPacket::WithAcl(obj_view);
      break;
    case PacketIndicator::kHciSco:
      packet = fuchsia_hardware_bluetooth::wire::SnoopPacket::WithSco(obj_view);
      break;
    case PacketIndicator::kHciEvent:
      packet =
          fuchsia_hardware_bluetooth::wire::SnoopPacket::WithEvent(obj_view);
      break;
    case PacketIndicator::kHciIso:
      packet = fuchsia_hardware_bluetooth::wire::SnoopPacket::WithIso(obj_view);
      break;
    default:
      fdf::warn("Unknown indicator in {}", __func__);
      return;
  }

  fidl::Arena arena;
  fuchsia_hardware_bluetooth::wire::SnoopOnObservePacketRequest request =
      fuchsia_hardware_bluetooth::wire::SnoopOnObservePacketRequest::Builder(
          arena)
          .sequence(sequence)
          .direction(direction)
          .packet(packet)
          .Build();

  fidl::OneWayStatus observe_status =
      fidl::WireSendEvent(binding_)->OnObservePacket(request);
  if (!observe_status.ok()) {
    fdf::warn("Failed to send OnObservePacket on Snoop: {}",
              observe_status.status_string());
  }
}

void LoopbackDevice::SnoopServer::AcknowledgePackets(
    AcknowledgePacketsRequest& request,
    AcknowledgePacketsCompleter::Sync& completer) {
  if (request.sequence() <= acked_sequence_) {
    return;
  }
  acked_sequence_ = request.sequence();

  // Send Snoop.OnDroppedPackets if necessary before sending next
  // Snoop.ObservePacket.
  if (dropped_sent_ != 0 || dropped_received_ != 0) {
    fuchsia_hardware_bluetooth::SnoopOnDroppedPacketsRequest request;
    request.sent() = dropped_sent_;
    request.received() = dropped_received_;
    fit::result<fidl::OneWayError> result =
        fidl::SendEvent(binding_)->OnDroppedPackets(request);
    if (result.is_error()) {
      fdf::warn("Failed to send Snoop.OnDroppedPackets event: {}",
                result.error_value().status_string());
    }
    dropped_sent_ = 0;
    dropped_received_ = 0;
  }

  while (!queued_packets_.empty() &&
         queued_packets_.front().sequence <=
             acked_sequence_ + kMaxSnoopUnackedPackets) {
    Packet& packet = queued_packets_.front();
    SendSnoopPacket(packet.packet.data(),
                    packet.packet.size(),
                    packet.indicator,
                    packet.direction,
                    packet.sequence);
    queued_packets_.pop();
  }
}

void LoopbackDevice::SnoopServer::OnFidlError(fidl::UnbindInfo error) {
  if (!error.is_user_initiated()) {
    fdf::info("Snoop closed: {}", error.status_string());
  }
  device_->snoop_.reset();
}

void LoopbackDevice::SnoopServer::handle_unknown_method(
    fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Snoop> metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  fdf::warn("Unknown Snoop method received");
}

void LoopbackDevice::Connect(
    fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor> request) {
  vendor_binding_group_.AddBinding(
      dispatcher_, std::move(request), this, fidl::kIgnoreBindingClosure);
}

void LoopbackDevice::OnLoopbackChannelSignal(async_dispatcher_t* dispatcher,
                                             async::WaitBase* wait,
                                             zx_status_t status,
                                             const zx_packet_signal_t* signal) {
  if (status == ZX_ERR_CANCELED) {
    return;
  }
  if (status != ZX_OK) {
    fdf::error("Loopback channel wait error: {}", zx_status_get_string(status));
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    ReadLoopbackChannel();
  }

  if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    fdf::error("Loopback channel peer closed");
    return;
  }

  loopback_chan_wait_.Begin(dispatcher_);
}

void LoopbackDevice::OnHciTransportUnbound(size_t binding_id,
                                           fidl::UnbindInfo info) {
  hci_transport_servers_.erase(binding_id);
}

void LoopbackDevice::ReadLoopbackChannel() {
  while (true) {
    uint32_t actual_bytes = 0;
    zx_status_t read_status = loopback_chan_.read(
        /*flags=*/0,
        read_buffer_.data(),
        /*handles=*/nullptr,
        static_cast<uint32_t>(read_buffer_.size()),
        /*num_handles=*/0,
        &actual_bytes,
        /*actual_handles=*/nullptr);

    if (read_status == ZX_ERR_SHOULD_WAIT) {
      return;
    }

    if (read_status != ZX_OK) {
      fdf::error("failed to read from loopback channel {}",
                 zx_status_get_string(read_status));
      return;
    }

    if (actual_bytes == 0) {
      fdf::warn("ignoring empty packet when reading loopback channel");
      continue;
    }

    for (auto& [_, server] : hci_transport_servers_) {
      server.OnReceive(read_buffer_.data(), actual_bytes);
    }

    if (snoop_.has_value()) {
      snoop_->QueueSnoopPacket(
          read_buffer_.data() + 1,
          actual_bytes - 1,
          static_cast<PacketIndicator>(read_buffer_[0]),
          fuchsia_hardware_bluetooth::PacketDirection::kControllerToHost);
    }
  }
}

void LoopbackDevice::WriteLoopbackChannel(PacketIndicator indicator,
                                          uint8_t* buffer,
                                          size_t length) {
  // We tack on an indicator byte to the beginning of the payload.
  // Use an iovec to avoid a large allocation + copy.
  zx_channel_iovec_t iovs[2];
  iovs[0] = {
      .buffer = &indicator, .capacity = sizeof(indicator), .reserved = 0};
  iovs[1] = {.buffer = buffer,
             .capacity = static_cast<uint32_t>(length),
             .reserved = 0};

  zx_status_t write_status =
      loopback_chan_.write(/*flags=*/ZX_CHANNEL_WRITE_USE_IOVEC,
                           /*bytes=*/iovs,
                           /*num_bytes=*/2,
                           /*handles=*/nullptr,
                           /*num_handles=*/0);
  if (write_status != ZX_OK) {
    fdf::error("Failed to write to loopback channel: {}",
               zx_status_get_string(write_status));
    return;
  }

  if (snoop_.has_value()) {
    snoop_->QueueSnoopPacket(
        buffer,
        length,
        indicator,
        fuchsia_hardware_bluetooth::PacketDirection::kHostToController);
  }
}

void LoopbackDevice::GetFeatures(GetFeaturesCompleter::Sync& completer) {
  completer.Reply(fuchsia_hardware_bluetooth::VendorFeatures());
}

void LoopbackDevice::EncodeCommand(EncodeCommandRequest& request,
                                   EncodeCommandCompleter::Sync& completer) {
  completer.Reply(fit::error(ZX_ERR_NOT_SUPPORTED));
}

void LoopbackDevice::OpenHci(OpenHciCompleter::Sync& completer) {
  completer.Reply(fit::error(ZX_ERR_NOT_SUPPORTED));
}

void LoopbackDevice::OpenHciTransport(
    OpenHciTransportCompleter::Sync& completer) {
  auto endpoints =
      fidl::CreateEndpoints<fuchsia_hardware_bluetooth::HciTransport>();
  if (endpoints.is_error()) {
    fdf::error("Failed to create HciTransport endpoints: {}", endpoints);
    completer.Reply(fit::error(ZX_ERR_INTERNAL));
    return;
  }

  size_t binding_id = next_hci_transport_server_id_++;
  auto [_, inserted] = hci_transport_servers_.try_emplace(
      binding_id, this, binding_id, std::move(endpoints->server));
  ZX_ASSERT(inserted);
  completer.Reply(fit::ok(std::move(endpoints->client)));
}

void LoopbackDevice::OpenSnoop(OpenSnoopCompleter::Sync& completer) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_bluetooth::Snoop>();
  if (endpoints.is_error()) {
    fdf::error("Failed to create Snoop endpoints: {}", endpoints);
    completer.Reply(fit::error(ZX_ERR_INTERNAL));
    return;
  }
  snoop_.emplace(std::move(std::move(endpoints->server)), this);
  completer.Reply(fit::ok(std::move(endpoints->client)));
}

void LoopbackDevice::GetCrashParameters(
    GetCrashParametersCompleter::Sync& completer) {
  completer.Reply(fit::error(ZX_ERR_NOT_SUPPORTED));
}

void LoopbackDevice::handle_unknown_method(
    fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Vendor> metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  fdf::error(
      "Unknown method in Vendor request, closing with ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

LoopbackDevice::HciTransportServer::HciTransportServer(
    LoopbackDevice* device,
    size_t binding_id,
    fidl::ServerEnd<fuchsia_hardware_bluetooth::HciTransport> server_end)
    : device_(device),
      binding_id_(binding_id),
      binding_(device_->dispatcher_,
               std::move(server_end),
               this,
               std::mem_fn(&HciTransportServer::OnUnbound)) {}

void LoopbackDevice::HciTransportServer::OnReceive(uint8_t* buffer,
                                                   size_t length) {
  if (receive_credits_ == 0 || !receive_queue_.empty()) {
    if (receive_queue_.size() == kMaxReceiveQueueSize) {
      fdf::error("Receive queue reached max size, dropping packet");
      return;
    }
    std::vector<uint8_t> packet(buffer, buffer + length);
    receive_queue_.push(std::move(packet));
    return;
  }
  SendOnReceive(buffer, length);
}

void LoopbackDevice::HciTransportServer::OnUnbound(fidl::UnbindInfo info) {
  device_->hci_transport_servers_.erase(binding_id_);
}

void LoopbackDevice::HciTransportServer::SendOnReceive(uint8_t* buffer,
                                                       size_t length) {
  ZX_ASSERT(receive_credits_ != 0);
  receive_credits_--;

  // Omit the indicator byte in ReceivedPacket.
  fidl::VectorView vec_view =
      fidl::VectorView<uint8_t>::FromExternal(buffer + 1, length - 1);
  fidl::ObjectView obj_view =
      fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&vec_view);
  fuchsia_hardware_bluetooth::wire::ReceivedPacket packet;
  switch (static_cast<PacketIndicator>(buffer[0])) {
    case PacketIndicator::kHciEvent:
      packet =
          fuchsia_hardware_bluetooth::wire::ReceivedPacket::WithEvent(obj_view);
      break;
    case PacketIndicator::kHciAclData:
      packet =
          fuchsia_hardware_bluetooth::wire::ReceivedPacket::WithAcl(obj_view);
      break;
    case PacketIndicator::kHciIso:
      packet =
          fuchsia_hardware_bluetooth::wire::ReceivedPacket::WithIso(obj_view);
      break;
    default:
      fdf::warn("Received invalid packet indicator on loopback channel");
      return;
  }
  fidl::OneWayStatus status = fidl::WireSendEvent(binding_)->OnReceive(packet);
  if (!status.ok()) {
    fdf::warn("Error sending OnReceive event: {}", status.status_string());
  }
}

void LoopbackDevice::HciTransportServer::MaybeSendQueuedReceivePackets() {
  while (receive_credits_ != 0 && !receive_queue_.empty()) {
    SendOnReceive(receive_queue_.front().data(), receive_queue_.front().size());
    receive_queue_.pop();
  }
}

void LoopbackDevice::HciTransportServer::Send(SendRequest& request,
                                              SendCompleter::Sync& completer) {
  completer.Reply();

  switch (request.Which()) {
    case fuchsia_hardware_bluetooth::SentPacket::Tag::kCommand:
      device_->WriteLoopbackChannel(PacketIndicator::kHciCommand,
                                    request.command()->data(),
                                    request.command()->size());
      break;
    case fuchsia_hardware_bluetooth::SentPacket::Tag::kAcl:
      device_->WriteLoopbackChannel(PacketIndicator::kHciAclData,
                                    request.acl()->data(),
                                    request.acl()->size());
      break;
    case fuchsia_hardware_bluetooth::SentPacket::Tag::kIso:
      device_->WriteLoopbackChannel(PacketIndicator::kHciIso,
                                    request.iso()->data(),
                                    request.iso()->size());
      break;
    default:
      fdf::warn("Unknown SentPacket type");
  }
}

void LoopbackDevice::HciTransportServer::AckReceive(
    AckReceiveCompleter::Sync& completer) {
  receive_credits_++;
  MaybeSendQueuedReceivePackets();
}

void LoopbackDevice::HciTransportServer::ConfigureSco(
    ConfigureScoRequest& request, ConfigureScoCompleter::Sync& completer) {}

void LoopbackDevice::HciTransportServer::handle_unknown_method(
    fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::HciTransport>
        metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  fdf::error(
      "Unknown method in HciTransport request, closing with "
      "ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

}  // namespace bt_hci_virtual
