// Copyright 2023 The Pigweed Authors
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
#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_handle_map.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_advertiser.h"

namespace bt::hci {

class Transport;
class SequentialCommandRunner;

// AndroidExtendedLowEnergyAdvertiser implements chip-based multiple adverting
// via Android's vendor extensions. AndroidExtendedLowEnergyAdvertiser
// implements a LowEnergyAdvertiser but uses the Android vendor HCI extension
// commands to interface with the controller instead of standard Bluetooth Core
// Specification 5.0+. This enables power efficient multiple advertising for
// chipsets using pre-5.0 versions of Bluetooth.
//
// For more information, see
// https://source.android.com/devices/bluetooth/hci_requirements
class AndroidExtendedLowEnergyAdvertiser final
    : public LowEnergyAdvertiser,
      public WeakSelf<AndroidExtendedLowEnergyAdvertiser> {
 public:
  // Create an AndroidExtendedLowEnergyAdvertiser. The maximum number of
  // advertisements the controller can support (obtained via
  // hci_spec::vendor::android::LEGetVendorCapabilities) should be passed to the
  // constructor via the max_advertisements parameter.
  explicit AndroidExtendedLowEnergyAdvertiser(hci::Transport::WeakPtr hci,
                                              pw::async::Dispatcher& dispatcher,
                                              uint8_t max_advertisements);
  ~AndroidExtendedLowEnergyAdvertiser() override;

  // LowEnergyAdvertiser overrides:
  bool AllowsRandomAddressChange() const override { return !IsAdvertising(); }

  // Attempt to start advertising. See LowEnergyAdvertiser::StartAdvertising for
  // full documentation.
  //
  // The number of advertising sets that can be supported is not fixed and the
  // Controller can change it at any time. This method may report an error if
  // the controller cannot currently support another advertising set.
  void StartAdvertising(
      const DeviceAddress& address,
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      ConnectionCallback connect_callback,
      ResultFunction<AdvertisementId> result_callback) override;

  void StopAdvertising(fit::function<void(Result<>)> result_cb) override;
  void StopAdvertising(AdvertisementId advertisement_id,
                       fit::function<void(Result<>)> result_cb) override;
  using LowEnergyAdvertiser::StopAdvertising;

  void OnIncomingConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      const DeviceAddress& peer_address,
      const hci_spec::LEConnectionParameters& conn_params) override;

  size_t MaxAdvertisements() const override {
    return advertising_handle_map_.capacity();
  }

  // Returns the last used advertising handle that was used for an advertising
  // set when communicating with the controller.
  std::optional<hci_spec::AdvertisingHandle> LastUsedHandleForTesting() const {
    return advertising_handle_map_.LastUsedHandleForTesting();
  }

  void AttachInspect(inspect::Node& parent) override;

 private:
  struct StagedConnectionParameters {
    pw::bluetooth::emboss::ConnectionRole role;
    DeviceAddress peer_address;
    hci_spec::LEConnectionParameters conn_params;
  };

  struct PendingStart {
    DeviceAddress address;
    AdvertisingData data;
    AdvertisingData scan_rsp;
    AdvertisingOptions options;
    ConnectionCallback connect_callback;
    StartAdvertisingInternalCallback result_callback;
  };

  CommandPacket BuildEnablePacket(
      AdvertisementId advertisement_id,
      pw::bluetooth::emboss::GenericEnableParam enable) const override;

  std::optional<SetAdvertisingParams> BuildSetAdvertisingParams(
      const DeviceAddress& address,
      const AdvertisingEventProperties& properties,
      const AdvertisingOptions& options,
      pw::bluetooth::emboss::LEOwnAddressType own_address_type,
      const AdvertisingIntervalRange& interval) override;

  std::optional<CommandPacket> BuildSetAdvertisingRandomAddr(
      AdvertisementId advertisement_id) const override;

  std::vector<CommandPacket> BuildSetAdvertisingData(
      AdvertisementId advertisement_id,
      const AdvertisingData& data,
      AdvFlags flags) const override;

  CommandPacket BuildUnsetAdvertisingData(
      AdvertisementId advertisement_id) const override;

  std::vector<CommandPacket> BuildSetScanResponse(
      AdvertisementId advertisement_id,
      const AdvertisingData& data) const override;

  CommandPacket BuildUnsetScanResponse(
      AdvertisementId advertisement_id) const override;

  std::optional<CommandPacket> BuildRemoveAdvertisingSet(
      AdvertisementId advertisement_id) const override;

  void OnCurrentOperationComplete() override;

  // Event handler for the LE multi-advertising state change sub-event
  CommandChannel::EventCallbackResult OnAdvertisingStateChangedSubevent(
      const EventPacket& event);

  AdvertisingHandleMap advertising_handle_map_;
  std::queue<fit::closure> op_queue_;
  CommandChannel::EventHandlerId state_changed_event_handler_id_;

  std::unordered_map<uint64_t, PendingStart> pending_starts_;
  uint64_t next_pending_start_id_ = 0;

  // Incoming connections to Android LE Multiple Advertising occur through two
  // events: HCI_LE_Connection_Complete and LE multi-advertising state change
  // subevent. The HCI_LE_Connection_Complete event provides the connection
  // handle along with some other connection related parameters. Notably missing
  // is the advertising handle, which we need to obtain the advertised device
  // address. Until we receive the LE multi-advertising state change subevent,
  // we stage these parameters.
  std::unordered_map<hci_spec::ConnectionHandle, StagedConnectionParameters>
      staged_connections_map_;

  inspect::Node node_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AndroidExtendedLowEnergyAdvertiser);
};

}  // namespace bt::hci
