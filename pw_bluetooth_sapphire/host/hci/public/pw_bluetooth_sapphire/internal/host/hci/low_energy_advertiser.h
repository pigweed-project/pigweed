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
#include <pw_assert/assert.h>

#include <memory>

#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_handle_map.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt {
class AdvertisingData;

namespace hci {
class Transport;

class AdvertisingIntervalRange final {
 public:
  // Constructs an advertising interval range. When sending an HCI command to
  // the Controller, legacy advertising uses a two byte interval (0x0020,
  // 0x4000) and extended advertising uses a three byte interval (0x000020,
  // 0xffffff). AdvertisingIntervalRange is used in both legacy and extended
  // advertising and therefore uses uint32_t instead of uint16_t for its
  // interval ranges. Validation that the interval is within range occurs
  // elsewhere (i.e. LowEnergyAdvertisingManager).
  constexpr AdvertisingIntervalRange(uint32_t min, uint32_t max)
      : min_(min), max_(max) {
    PW_ASSERT(min <= max);
  }

  uint32_t min() const { return min_; }
  uint32_t max() const { return max_; }

 private:
  uint32_t min_;
  uint32_t max_;
};

class LowEnergyAdvertiser : public LocalAddressClient {
 public:
  explicit LowEnergyAdvertiser(hci::Transport::WeakPtr hci,
                               pw::async::Dispatcher& dispatcher,
                               uint16_t max_advertising_data_length);
  ~LowEnergyAdvertiser() override = default;

  using ConnectionCallback =
      fit::function<void(AdvertisementId advertisement_id,
                         std::unique_ptr<hci::LowEnergyConnection> link)>;

  // TODO(armansito): The |address| parameter of this function doesn't always
  // correspond to the advertised device address as the local address for an
  // advertisement cannot always be configured by the advertiser. This is the
  // case especially in the following conditions:
  //
  //   1. The type of |address| is "LE Public". The advertised address always
  //   corresponds to the
  //      controller's BD_ADDR. This is the case in both legacy and extended
  //      advertising.
  //
  //   2. The type of |address| is "LE Random" and the advertiser implements
  //   legacy advertising.
  //      Since the controller local address is shared between scan, initiation,
  //      and advertising procedures, the advertiser cannot configure this
  //      address without interfering with the state of other ongoing
  //      procedures.
  //
  // We should either revisit this interface or update the documentation to
  // reflect the fact that the |address| is sometimes a hint and may or may not
  // end up being advertised. Currently the GAP layer decides which address to
  // pass to this call but the layering should be revisited when we add support
  // for extended advertising.
  //
  // -----
  //
  // Attempt to start advertising |data| with |options.flags| and scan response
  // |scan_rsp| using advertising address |address|. If |options.anonymous| is
  // set, |address| is ignored.
  //
  // If |address| is currently advertised, the advertisement is updated.
  //
  // If |connect_callback| is provided, the advertisement will be connectable,
  // and the provided |status_callback| will be called with a connection
  // reference when this advertisement is connected to and the advertisement has
  // been stopped.
  //
  // |options.interval| must be a value in "controller timeslices". See
  // hci-spec/hci_constants.h for the valid range.
  //
  // Provides results in |status_callback|. If advertising is setup, the final
  // interval of advertising is provided in |interval| and |status| is kSuccess.
  // Otherwise, |status| indicates the type of error and |interval| has no
  // meaning.
  //
  // |status_callback| may be called before this function returns, but will be
  // called before any calls to |connect_callback|.
  //
  // The maximum advertising and scan response data sizes are determined by the
  // Bluetooth controller (4.x supports up to 31 bytes while 5.x is extended up
  // to 251). If |data| and |scan_rsp| exceed this internal limit, a
  // HostError::kAdvertisingDataTooLong or HostError::kScanResponseTooLong error
  // will be generated.
  struct AdvertisingOptions {
    AdvertisingOptions(AdvertisingIntervalRange init_interval,
                       AdvFlags init_flags,
                       bool init_extended_pdu,
                       bool init_anonymous,
                       bool init_include_tx_power_level,
                       int8_t init_max_tx_power =
                           hci_spec::kLEExtendedAdvertisingTxPowerNoPreference)
        : interval(init_interval),
          flags(init_flags),
          extended_pdu(init_extended_pdu),
          include_tx_power_level(init_include_tx_power_level),
          max_tx_power(init_max_tx_power),
          anonymous(init_anonymous) {}

    AdvertisingIntervalRange interval;
    AdvFlags flags;
    bool extended_pdu;
    bool include_tx_power_level;
    int8_t max_tx_power;

    // TODO(b/42157563): anonymous advertising is currently not supported
    bool anonymous;
  };

  // Core Spec Version 5.4, Volume 4, Part E, Section 7.8.53: These fields are
  // the same as those defined in advertising event properties.
  //
  // TODO(fxbug.dev/333129711): LEAdvertisingEventProperties is
  // currently defined in Emboss as a bits field. Unfortunately, this means that
  // we cannot use it as storage within our own code. Instead, we have to
  // redefine a struct with the same fields in it if we want to use it as
  // storage.
  struct AdvertisingEventProperties {
    bool connectable = false;
    bool scannable = false;
    bool directed = false;
    bool high_duty_cycle_directed_connectable = false;
    bool use_legacy_pdus = false;
    bool anonymous_advertising = false;
    bool include_tx_power = false;

    bool IsDirected() const {
      return directed || high_duty_cycle_directed_connectable;
    }
  };

  // Determine the properties of an advertisement based on the parameters the
  // client has passed in. For example, if the client has included a scan
  // response, the advertisement should be scannable.
  static AdvertisingEventProperties GetAdvertisingEventProperties(
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      const ConnectionCallback& connect_callback);

  // Convert individual advertisement properties (e.g. connecatble, scannable,
  // directed, etc) to a legacy LEAdvertisingType
  static pw::bluetooth::emboss::LEAdvertisingType
  AdvertisingEventPropertiesToLEAdvertisingType(
      const AdvertisingEventProperties& p);

  // Asynchronously starts advertising. The result will be reported via
  // |result_callback|. On success, an AdvertisingHandle will be returned that
  // can be used to stop advertising.
  virtual void StartAdvertising(
      const DeviceAddress& address,
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      ConnectionCallback connect_callback,
      ResultFunction<hci::AdvertisementId> result_callback) = 0;

  // Stops advertisement on all currently advertising addresses. Idempotent and
  // asynchronous.
  virtual void StopAdvertising(fit::function<void(Result<>)> result_cb) = 0;
  void StopAdvertising() { StopAdvertising(nullptr); }

  // Stops the advertisement identified by |handle|. Idempotent and
  // asynchronous.
  virtual void StopAdvertising(hci::AdvertisementId advertisement_id,
                               fit::function<void(Result<>)> result_cb) = 0;
  void StopAdvertising(hci::AdvertisementId advertisement_id) {
    StopAdvertising(advertisement_id, nullptr);
  }

  // Callback for an incoming LE connection. This function should be called in
  // reaction to any connection that was not initiated locally. This object will
  // determine if it was a result of an active advertisement and route the
  // connection accordingly.
  virtual void OnIncomingConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      const DeviceAddress& peer_address,
      const hci_spec::LEConnectionParameters& conn_params) = 0;

  // Returns true if currently advertising at all
  virtual bool IsAdvertising() const { return !connection_callbacks_.empty(); }

  // Returns true if currently advertising for the given advertising handle.
  bool IsAdvertising(hci::AdvertisementId advertisement_id) const {
    return connection_callbacks_.count(advertisement_id) != 0;
  }

  // Returns the number of advertisements currently registered
  size_t NumAdvertisements() const { return connection_callbacks_.size(); }

  // Returns the maximum number of advertisements that can be supported
  virtual size_t MaxAdvertisements() const = 0;

  virtual bool IsExtendedAdvertiser() const { return false; }

  virtual void AttachInspect(inspect::Node& /*parent*/) {}

 protected:
  using StartAdvertisingInternalResult =
      fit::result<std::tuple<Error, std::optional<hci::AdvertisementId>>,
                  hci::AdvertisementId>;
  using StartAdvertisingInternalCallback =
      fit::callback<void(StartAdvertisingInternalResult result)>;

  // Return value of BuildSetAdvertisingParams.
  struct SetAdvertisingParams {
    CommandPacket packet;
    hci::AdvertisementId advertisement_id;
  };

  // Build the HCI command packet to enable advertising for the flavor of low
  // energy advertising being implemented.
  virtual CommandPacket BuildEnablePacket(
      AdvertisementId advertisement_id,
      pw::bluetooth::emboss::GenericEnableParam enable) const = 0;

  // Build the HCI command packet to set the advertising parameters for the
  // flavor of low energy advertising being implemented. Also returns the
  // AdvertisingHandle used for this advertising set.
  virtual std::optional<SetAdvertisingParams> BuildSetAdvertisingParams(
      const DeviceAddress& address,
      const AdvertisingEventProperties& properties,
      const AdvertisingOptions& options,
      pw::bluetooth::emboss::LEOwnAddressType own_address_type,
      const AdvertisingIntervalRange& interval) = 0;

  // Build the HCI command packet to set the random address to use for the
  // flavor of low energy advertising being implemented. If no command packet is
  // needed for this advertising mechanism, return a nullopt.
  virtual std::optional<CommandPacket> BuildSetAdvertisingRandomAddr(
      AdvertisementId advertisement_id) const = 0;

  // Build the HCI command packet to set the advertising data for the flavor of
  // low energy advertising being implemented.
  virtual std::vector<CommandPacket> BuildSetAdvertisingData(
      AdvertisementId advertisement_id,
      const AdvertisingData& data,
      AdvFlags flags) const = 0;

  // Build the HCI command packet to delete the advertising parameters from the
  // controller for the flavor of low energy advertising being implemented. This
  // method is used when stopping an advertisement.
  virtual CommandPacket BuildUnsetAdvertisingData(
      AdvertisementId advertisement_id) const = 0;

  // Build the HCI command packet to set the data sent in a scan response (if
  // requested) for the flavor of low energy advertising being implemented.
  virtual std::vector<CommandPacket> BuildSetScanResponse(
      AdvertisementId advertisement_id,
      const AdvertisingData& scan_rsp) const = 0;

  // Build the HCI command packet to delete the advertising parameters from the
  // controller for the flavor of low energy advertising being implemented.
  virtual CommandPacket BuildUnsetScanResponse(
      AdvertisementId advertisement_id) const = 0;

  // Build the HCI command packet to remove the advertising set entirely from
  // the controller's memory for the flavor of low energy advertising being
  // implemented.
  virtual std::optional<CommandPacket> BuildRemoveAdvertisingSet(
      AdvertisementId advertisement_id) const = 0;

  // Called when the command packet created with BuildSetAdvertisingParams
  // returns with a result
  virtual void OnSetAdvertisingParamsComplete(const EventPacket&) {}

  // Called when a sequence of HCI commands that form a single operation (e.g.
  // start advertising, stop advertising) completes in its entirety. Subclasses
  // can override this method to be notified when the HCI command runner is
  // available once again.
  virtual void OnCurrentOperationComplete() {}

  // Get the current limit in bytes of the advertisement data supported.
  size_t GetSizeLimit(const AdvertisingEventProperties& properties,
                      const AdvertisingOptions& options) const;

  // Check whether we can actually start advertising given the combination of
  // input parameters (e.g. check that the requested advertising data and scan
  // response will actually fit within the size limitations of the advertising
  // PDUs)
  fit::result<HostError> CanStartAdvertising(
      const DeviceAddress& address,
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      const ConnectionCallback& connect_callback) const;

  // Unconditionally start advertising (all checks must be performed in the
  // methods that call this one).
  void StartAdvertisingInternal(const DeviceAddress& address,
                                const AdvertisingData& data,
                                const AdvertisingData& scan_rsp,
                                const AdvertisingOptions& options,
                                ConnectionCallback connect_callback,
                                StartAdvertisingInternalCallback callback);

  // Unconditionally stop all advertising (all checks muts be performed in the
  // methods that call this one).
  void StopAdvertisingInternal(fit::function<void(Result<>)> result_cb);

  // Unconditionally stop advertising (all checks muts be performed in the
  // methods that call this one).
  void StopAdvertisingInternal(AdvertisementId advertisement_id,
                               fit::function<void(Result<>)> result_cb);

  // Handle shared housekeeping tasks when an incoming connection is completed
  // (e.g. clean up internal state, call callbacks, etc)
  void CompleteIncomingConnection(
      hci_spec::ConnectionHandle connection_handle,
      pw::bluetooth::emboss::ConnectionRole role,
      const DeviceAddress& local_address,
      const DeviceAddress& peer_address,
      const hci_spec::LEConnectionParameters& conn_params,
      std::optional<AdvertisementId> advertisement_id);

  SequentialCommandRunner& hci_cmd_runner() const { return *hci_cmd_runner_; }
  hci::Transport::WeakPtr hci() const { return hci_; }

 private:
  struct StagedParameters {
    AdvertisingData data;
    AdvertisingData scan_rsp;

    void reset() {
      AdvertisingData blank;
      blank.Copy(&data);
      blank.Copy(&scan_rsp);
    }
  };

  // Continuation function for starting advertising, called automatically via
  // callbacks in StartAdvertisingInternal. Developers should not call this
  // function directly.
  void StartAdvertisingInternalStep2(
      AdvertisementId advertisement_id,
      const DeviceAddress& address,
      const AdvertisingOptions& options,
      ConnectionCallback connect_callback,
      StartAdvertisingInternalCallback result_callback);

  // Enqueue onto the HCI command runner the HCI commands necessary to stop
  // advertising and completely remove a given address from the controller's
  // memory. If even one of the HCI commands cannot be generated for some
  // reason, no HCI commands are enqueued.
  bool EnqueueStopAdvertisingCommands(hci::AdvertisementId advertisement_id);

  hci::Transport::WeakPtr hci_;
  pw::async::Dispatcher& dispatcher_;
  std::unique_ptr<SequentialCommandRunner> hci_cmd_runner_;
  StagedParameters staged_parameters_;

  std::unordered_map<hci::AdvertisementId, ConnectionCallback>
      connection_callbacks_;

  uint16_t max_advertising_data_length_ = hci_spec::kMaxLEAdvertisingDataLength;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyAdvertiser);
};

}  // namespace hci
}  // namespace bt
