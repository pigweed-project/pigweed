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

#pragma once

#include <fidl/fuchsia.driver.framework/cpp/fidl.h>
#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/driver/devfs/cpp/connector.h>

#include <queue>

#include "lib/async/cpp/wait.h"

namespace bt_hci_virtual {

using AddChildCallback =
    fit::function<void(fuchsia_driver_framework::wire::NodeAddArgs)>;

class LoopbackDevice : public fidl::Server<fuchsia_hardware_bluetooth::Vendor> {
 public:
  static constexpr size_t kMaxSnoopQueueSize = 20;
  static constexpr size_t kMaxSnoopUnackedPackets = 10;
  static constexpr size_t kMaxReceiveQueueSize = 40;
  static constexpr size_t kMaxReceiveUnackedPackets = 10;

  explicit LoopbackDevice();

  // Methods to control the LoopbackDevice's lifecycle. These are used by the
  // VirtualController. `channel` speaks the HCI UART protocol. `name` is the
  // name to be used for the driver framework node. `callback` will be called
  // with the NodeAddArgs when LoopbackDevice should be added as a child node.
  zx_status_t Initialize(zx::channel channel,
                         std::string_view name,
                         AddChildCallback callback);

  // Called by driver_devfs::Connector.
  void Connect(fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor> request);

 private:
  // HCI UART packet indicators
  enum class PacketIndicator : uint8_t {
    kHciNone = 0,
    kHciCommand = 1,
    kHciAclData = 2,
    kHciSco = 3,
    kHciEvent = 4,
    kHciIso = 5,
  };

  class SnoopServer : public fidl::Server<fuchsia_hardware_bluetooth::Snoop> {
   public:
    SnoopServer(fidl::ServerEnd<fuchsia_hardware_bluetooth::Snoop> server_end,
                LoopbackDevice* device);

    // `buffer` should NOT have an indicator byte.
    void QueueSnoopPacket(
        uint8_t* buffer,
        size_t length,
        PacketIndicator indicator,
        fuchsia_hardware_bluetooth::PacketDirection direction);

   private:
    struct Packet {
      std::vector<uint8_t> packet;
      uint64_t sequence;
      PacketIndicator indicator;
      fuchsia_hardware_bluetooth::PacketDirection direction;
    };

    void SendSnoopPacket(uint8_t* buffer,
                         size_t length,
                         PacketIndicator indicator,
                         fuchsia_hardware_bluetooth::PacketDirection direction,
                         uint64_t sequence);

    // fidl::Server<fuchsia_hardware_bluetooth::Snoop> overrides:
    void AcknowledgePackets(
        AcknowledgePacketsRequest& request,
        AcknowledgePacketsCompleter::Sync& completer) override;
    void handle_unknown_method(
        fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Snoop> metadata,
        fidl::UnknownMethodCompleter::Sync& completer) override;

    void OnFidlError(fidl::UnbindInfo error);

    fidl::ServerBinding<fuchsia_hardware_bluetooth::Snoop> binding_;
    uint64_t next_sequence_ = 1u;
    uint64_t acked_sequence_ = 0u;
    uint32_t dropped_sent_ = 0u;
    uint32_t dropped_received_ = 0u;
    std::queue<Packet> queued_packets_;
    LoopbackDevice* device_;
  };

  class HciTransportServer
      : public fidl::Server<fuchsia_hardware_bluetooth::HciTransport> {
   public:
    HciTransportServer(
        LoopbackDevice* device,
        size_t binding_id,
        fidl::ServerEnd<fuchsia_hardware_bluetooth::HciTransport> server_end);

    // `buffer` must have an indicator byte.
    void OnReceive(uint8_t* buffer, size_t length);

    void OnUnbound(fidl::UnbindInfo info);

   private:
    void SendOnReceive(uint8_t* buffer, size_t length);
    void MaybeSendQueuedReceivePackets();

    // fuchsia_hardware_bluetooth::HciTransport overrides:
    void Send(SendRequest& request, SendCompleter::Sync& completer) override;
    void AckReceive(AckReceiveCompleter::Sync& completer) override;
    void ConfigureSco(ConfigureScoRequest& request,
                      ConfigureScoCompleter::Sync& completer) override;
    void handle_unknown_method(
        fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::HciTransport>
            metadata,
        fidl::UnknownMethodCompleter::Sync& completer) override;

    LoopbackDevice* device_;
    size_t binding_id_;
    size_t receive_credits_ = kMaxReceiveUnackedPackets;
    std::queue<std::vector<uint8_t>> receive_queue_;
    fidl::ServerBinding<fuchsia_hardware_bluetooth::HciTransport> binding_;
  };

  void OnLoopbackChannelSignal(async_dispatcher_t* dispatcher,
                               async::WaitBase* wait,
                               zx_status_t status,
                               const zx_packet_signal_t* signal);

  void OnHciTransportUnbound(size_t binding_id, fidl::UnbindInfo info);

  void ReadLoopbackChannel();
  void WriteLoopbackChannel(PacketIndicator indicator,
                            uint8_t* buffer,
                            size_t length);

  // fuchsia_hardware_bluetooth::Vendor overrides:
  void GetFeatures(GetFeaturesCompleter::Sync& completer) override;
  void EncodeCommand(EncodeCommandRequest& request,
                     EncodeCommandCompleter::Sync& completer) override;
  void OpenHci(OpenHciCompleter::Sync& completer) override;
  void OpenHciTransport(OpenHciTransportCompleter::Sync& completer) override;
  void OpenSnoop(OpenSnoopCompleter::Sync& completer) override;
  void GetCrashParameters(
      GetCrashParametersCompleter::Sync& completer) override;
  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Vendor> metadata,
      fidl::UnknownMethodCompleter::Sync& completer) override;

  zx::channel loopback_chan_;
  async::WaitMethod<LoopbackDevice, &LoopbackDevice::OnLoopbackChannelSignal>
      loopback_chan_wait_{this};

  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::Vendor>
      vendor_binding_group_;

  // Multiple HciTransport servers need to be supported: at least 1 for bt-host
  // and 1 for bt-snoop.
  std::unordered_map<size_t, HciTransportServer> hci_transport_servers_;
  size_t next_hci_transport_server_id_ = 0u;

  // Only support 1 Snoop binding at a time.
  std::optional<SnoopServer> snoop_;

  async_dispatcher_t* dispatcher_;

  // +1 for packet indicator
  std::array<uint8_t, fuchsia_hardware_bluetooth::kAclPacketMax + 1>
      read_buffer_;

  driver_devfs::Connector<fuchsia_hardware_bluetooth::Vendor> devfs_connector_;
};

}  // namespace bt_hci_virtual
