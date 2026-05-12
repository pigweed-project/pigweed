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
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

class SequentialCommandRunner;

class ExtendedLowEnergyAdvertiser final : public LowEnergyAdvertiser {
 public:
  explicit ExtendedLowEnergyAdvertiser(hci::Transport::WeakPtr hci,
                                       pw::async::Dispatcher& dispatcher,
                                       uint16_t max_advertising_data_length_);
  ~ExtendedLowEnergyAdvertiser() override;

  bool AllowsRandomAddressChange() const override { return !IsAdvertising(); }

  // Attempt to start advertising. See LowEnergyAdvertiser::StartAdvertising for
  // full documentation.
  //
  // According to the Bluetooth Spec, Volume 4, Part E, Section 7.8.58, "the
  // number of advertising sets that can be supported is not fixed and the
  // Controller can change it at any time. The memory used to store advertising
  // sets can also be used for other purposes."
  //
  // This method may report an error if the controller cannot currently support
  // another advertising set.
  void StartAdvertising(
      const DeviceAddress& address,
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      ConnectionCallback connect_callback,
      ResultFunction<hci::AdvertisementId> result_callback) override;

  void StopAdvertising(fit::function<void(Result<>)> result_cb) override;
  void StopAdvertising(hci::AdvertisementId advertisement_id,
                       fit::function<void(Result<>)> result_cb) override;
  using LowEnergyAdvertiser::StopAdvertising;

  void OnIncomingConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      const DeviceAddress& peer_address,
      const hci_spec::LEConnectionParameters& conn_params) override;

  bool IsExtendedAdvertiser() const override { return true; }

  size_t MaxAdvertisements() const override {
    return advertising_handle_map_.capacity();
  }

  // Returns the last used advertising handle that was used for an advertising
  // set when communicating with the controller.
  std::optional<hci_spec::AdvertisingHandle> LastUsedHandleForTesting() const {
    return advertising_handle_map_.LastUsedHandleForTesting();
  }

  void AttachInspect(inspect::Node& node) override;

 private:
  struct StagedConnectionParameters {
    pw::bluetooth::emboss::ConnectionRole role;
    DeviceAddress peer_address;
    hci_spec::LEConnectionParameters conn_params;
  };

  struct StagedAdvertisingParameters {
    bool include_tx_power_level = false;
    int8_t selected_tx_power_level = 0;
    bool extended_pdu = false;

    void clear() {
      include_tx_power_level = false;
      selected_tx_power_level = 0;
      extended_pdu = false;
    }
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

  void OnSetAdvertisingParamsComplete(const EventPacket&) override;

  CommandPacket BuildAdvertisingDataFragmentPacket(
      hci_spec::AdvertisingHandle handle,
      const BufferView& data,
      pw::bluetooth::emboss::LESetExtendedAdvDataOp operation,
      pw::bluetooth::emboss::LEExtendedAdvFragmentPreference
          fragment_preference) const;

  CommandPacket BuildScanResponseDataFragmentPacket(
      hci_spec::AdvertisingHandle handle,
      const BufferView& data,
      pw::bluetooth::emboss::LESetExtendedAdvDataOp operation,
      pw::bluetooth::emboss::LEExtendedAdvFragmentPreference
          fragment_preference) const;

  void OnCurrentOperationComplete() override;

  // Event handler for the HCI LE Advertising Set Terminated event
  void OnAdvertisingSetTerminatedEvent(const EventPacket& event);
  CommandChannel::EventHandlerId event_handler_id_;

  AdvertisingHandleMap advertising_handle_map_;
  std::queue<fit::closure> op_queue_;
  StagedAdvertisingParameters staged_advertising_parameters_;

  // Core Spec Volume 4, Part E, Section 7.8.56: Incoming connections to LE
  // Extended Advertising occur through two events: HCI_LE_Connection_Complete
  // and HCI_LE_Advertising_Set_Terminated. The HCI_LE_Connection_Complete event
  // provides the connection handle along with some other connection related
  // parameters. Notably missing is the advertising handle, which we need to
  // obtain the advertised device address. Until we receive the
  // HCI_LE_Advertising_Set_Terminated event, we stage these parameters.
  std::unordered_map<hci_spec::ConnectionHandle, StagedConnectionParameters>
      staged_connections_;

  inspect::Node node_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ExtendedLowEnergyAdvertiser);
};

}  // namespace bt::hci
