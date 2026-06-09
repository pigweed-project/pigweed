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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {
using hci_spec::ConnectionHandle;
using hci_spec::LEConnectionParameters;
using pw::bluetooth::emboss::ConnectionRole;
using pw::bluetooth::emboss::GenericEnableParam;
using pw::bluetooth::emboss::LEAddressType;
using pw::bluetooth::emboss::LEConnectionCompleteSubeventView;
using pw::bluetooth::emboss::LECreateConnectionCancelCommandView;
using pw::bluetooth::emboss::LECreateConnectionCommandWriter;
using pw::bluetooth::emboss::LEEnhancedConnectionCompleteSubeventV1View;
using pw::bluetooth::emboss::LEExtendedCreateConnectionCommandV1Writer;
using pw::bluetooth::emboss::LEMetaEventView;
using pw::bluetooth::emboss::LEOwnAddressType;
using pw::bluetooth::emboss::LEPeerAddressTypeNoAnon;
using pw::bluetooth::emboss::StatusCode;

LowEnergyConnector::PendingRequest::PendingRequest(
    const DeviceAddress& init_peer_address, StatusCallback init_status_callback)
    : peer_address(init_peer_address),
      status_callback(std::move(init_status_callback)) {}

LowEnergyConnector::LowEnergyConnector(
    Transport::WeakPtr hci,
    LocalAddressDelegate* local_addr_delegate,
    pw::async::Dispatcher& dispatcher,
    IncomingConnectionDelegate delegate,
    bool use_extended_operations)
    : pw_dispatcher_(dispatcher),
      hci_(std::move(hci)),
      local_addr_delegate_(local_addr_delegate),
      delegate_(std::move(delegate)),
      use_extended_operations_(use_extended_operations),
      weak_self_(this) {
  request_timeout_task_.set_function(
      [this](pw::async::Context& /*ctx*/, pw::Status status) {
        if (status.ok()) {
          OnCreateConnectionTimeout();
        }
      });

  CommandChannel::EventHandlerId id =
      hci_->command_channel()->AddLEMetaEventHandler(
          hci_spec::kLEConnectionCompleteSubeventCode,
          [this](const EventPacket& event) {
            OnConnectionCompleteEvent<LEConnectionCompleteSubeventView>(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  event_handler_ids_.insert(id);

  id = hci_->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLEEnhancedConnectionCompleteSubeventCode,
      [this](const EventPacket& event) {
        OnConnectionCompleteEvent<LEEnhancedConnectionCompleteSubeventV1View>(
            event);
        return CommandChannel::EventCallbackResult::kContinue;
      });
  event_handler_ids_.insert(id);
}

LowEnergyConnector::~LowEnergyConnector() {
  if (request_pending()) {
    Cancel();
  }

  if (hci_.is_alive() && hci_->command_channel()) {
    for (CommandChannel::EventHandlerId id : event_handler_ids_) {
      hci_->command_channel()->RemoveEventHandler(id);
    }
  }
}

bool LowEnergyConnector::CreateConnection(
    bool use_accept_list,
    const DeviceAddress& peer_address,
    uint16_t scan_interval,
    uint16_t scan_window,
    const hci_spec::LEPreferredConnectionParameters& initial_parameters,
    StatusCallback status_callback,
    pw::chrono::SystemClock::duration timeout) {
  PW_DCHECK(status_callback);
  PW_DCHECK(timeout.count() > 0);

  if (request_pending()) {
    return false;
  }

  PW_DCHECK(!request_timeout_task_.is_pending());
  pending_request_ = PendingRequest(peer_address, std::move(status_callback));

  if (use_local_identity_address_) {
    // Use the identity address if privacy override was enabled.
    DeviceAddress address = local_addr_delegate_->identity_address();
    CreateConnectionInternal(address,
                             use_accept_list,
                             peer_address,
                             scan_interval,
                             scan_window,
                             initial_parameters,
                             std::move(status_callback),
                             timeout);
    return true;
  }

  local_addr_delegate_->EnsureLocalAddress(
      /*address_type=*/std::nullopt,
      [this,
       use_accept_list,
       peer_address,
       scan_interval,
       scan_window,
       initial_parameters,
       timeout,
       callback = std::move(status_callback)](
          fit::result<HostError, const DeviceAddress> result) mutable {
        if (result.is_error()) {
          callback(fit::error(result.error_value()), nullptr);
          return;
        }
        CreateConnectionInternal(result.value(),
                                 use_accept_list,
                                 peer_address,
                                 scan_interval,
                                 scan_window,
                                 initial_parameters,
                                 std::move(callback),
                                 timeout);
      });

  return true;
}

std::optional<DeviceAddress> LowEnergyConnector::pending_peer_address() const {
  if (pending_request_) {
    return pending_request_->peer_address;
  }

  return std::nullopt;
}

void LowEnergyConnector::CreateConnectionInternal(
    const DeviceAddress& local_address,
    bool use_accept_list,
    const DeviceAddress& peer_address,
    uint16_t scan_interval,
    uint16_t scan_window,
    const hci_spec::LEPreferredConnectionParameters& initial_params,
    StatusCallback,
    pw::chrono::SystemClock::duration timeout) {
  if (!hci_.is_alive()) {
    return;
  }

  // Check if the connection request was canceled via Cancel().
  if (!pending_request_ || pending_request_->canceled) {
    bt_log(DEBUG,
           "hci-le",
           "connection request was canceled while obtaining local address");
    pending_request_.reset();
    return;
  }

  PW_DCHECK(!pending_request_->initiating);

  pending_request_->initiating = true;
  pending_request_->local_address = local_address;

  // HCI Command Status Event will be sent as our completion callback.
  auto self = weak_self_.GetWeakPtr();
  auto complete_cb = [self, timeout](auto, const EventPacket& event) {
    PW_DCHECK(event.event_code() == hci_spec::kCommandStatusEventCode);

    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (result.is_error()) {
      self->OnCreateConnectionComplete(result, nullptr);
      return;
    }

    // The request was started but has not completed; initiate the command
    // timeout period. NOTE: The request will complete when the controller
    // asynchronously notifies us of with a LE Connection Complete event.
    self->request_timeout_task_.Cancel();
    self->request_timeout_task_.PostAfter(timeout);
  };

  std::optional<CommandPacket> request;
  if (use_extended_operations_) {
    request.emplace(BuildExtendedCreateConnectionPacket(local_address,
                                                        peer_address,
                                                        initial_params,
                                                        use_accept_list,
                                                        scan_interval,
                                                        scan_window));
  } else {
    request.emplace(BuildCreateConnectionPacket(local_address,
                                                peer_address,
                                                initial_params,
                                                use_accept_list,
                                                scan_interval,
                                                scan_window));
  }

  hci_->command_channel()
      ->SendCommand(std::move(request.value()),
                    complete_cb,
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

CommandPacket LowEnergyConnector::BuildExtendedCreateConnectionPacket(
    const DeviceAddress& local_address,
    const DeviceAddress& peer_address,
    const hci_spec::LEPreferredConnectionParameters& initial_params,
    bool use_accept_list,
    uint16_t scan_interval,
    uint16_t scan_window) {
  // The LE Extended Create Connection Command ends with a variable amount of
  // data: per PHY connection settings. Depending on the PHYs we select to scan
  // on when connecting, the variable amount of data at the end of the packet
  // grows. Currently, we scan on all available PHYs. Thus, we use the maximum
  // size of this packet.
  size_t max_size = pw::bluetooth::emboss::LEExtendedCreateConnectionCommandV1::
      MaxSizeInBytes();

  auto packet = CommandPacket::New<LEExtendedCreateConnectionCommandV1Writer>(
      hci_spec::kLEExtendedCreateConnection, max_size);
  auto params = packet.view_t();

  if (use_accept_list) {
    params.initiator_filter_policy().Write(GenericEnableParam::ENABLE);
  } else {
    params.initiator_filter_policy().Write(GenericEnableParam::DISABLE);
  }

  // TODO(b/328311582): Use the resolved address types for <5.0 LE
  // Privacy.
  if (peer_address.IsPublic()) {
    params.peer_address_type().Write(LEPeerAddressTypeNoAnon::PUBLIC);
  } else {
    params.peer_address_type().Write(LEPeerAddressTypeNoAnon::RANDOM);
  }

  if (local_address.IsPublic()) {
    params.own_address_type().Write(LEOwnAddressType::PUBLIC);
  } else {
    params.own_address_type().Write(LEOwnAddressType::RANDOM);
  }

  params.peer_address().CopyFrom(peer_address.value().view());

  // We scan on all available PHYs for a connection
  params.initiating_phys().le_1m().Write(true);
  params.initiating_phys().le_2m().Write(true);
  params.initiating_phys().le_coded().Write(true);

  for (int i = 0; i < params.num_entries().Read(); i++) {
    params.data()[i].scan_interval().Write(scan_interval);
    params.data()[i].scan_window().Write(scan_window);
    params.data()[i].connection_interval_min().Write(
        initial_params.min_interval());
    params.data()[i].connection_interval_max().Write(
        initial_params.max_interval());
    params.data()[i].max_latency().Write(initial_params.max_latency());
    params.data()[i].supervision_timeout().Write(
        initial_params.supervision_timeout());

    // These fields provide the expected minimum and maximum duration of
    // connection events. We have no preference, so we set them to zero and let
    // the Controller decide. Some Controllers require
    // max_ce_length < max_connection_interval * 2.
    params.data()[i].min_connection_event_length().Write(0x0000);
    params.data()[i].max_connection_event_length().Write(0x0000);
  }

  return packet;
}

CommandPacket LowEnergyConnector::BuildCreateConnectionPacket(
    const DeviceAddress& local_address,
    const DeviceAddress& peer_address,
    const hci_spec::LEPreferredConnectionParameters& initial_params,
    bool use_accept_list,
    uint16_t scan_interval,
    uint16_t scan_window) {
  auto packet = CommandPacket::New<LECreateConnectionCommandWriter>(
      hci_spec::kLECreateConnection);
  auto params = packet.view_t();

  if (use_accept_list) {
    params.initiator_filter_policy().Write(GenericEnableParam::ENABLE);
  } else {
    params.initiator_filter_policy().Write(GenericEnableParam::DISABLE);
  }

  // TODO(b/328311582): Use the resolved address types for <5.0 LE
  // Privacy.
  if (peer_address.IsPublic()) {
    params.peer_address_type().Write(LEAddressType::PUBLIC);
  } else {
    params.peer_address_type().Write(LEAddressType::RANDOM);
  }

  if (local_address.IsPublic()) {
    params.own_address_type().Write(LEOwnAddressType::PUBLIC);
  } else {
    params.own_address_type().Write(LEOwnAddressType::RANDOM);
  }

  params.le_scan_interval().Write(scan_interval);
  params.le_scan_window().Write(scan_window);
  params.peer_address().CopyFrom(peer_address.value().view());
  params.connection_interval_min().Write(initial_params.min_interval());
  params.connection_interval_max().Write(initial_params.max_interval());
  params.max_latency().Write(initial_params.max_latency());
  params.supervision_timeout().Write(initial_params.supervision_timeout());

  // These fields provide the expected minimum and maximum duration of
  // connection events. We have no preference, so we set them to zero and let
  // the Controller decide. Some Controllers require
  // max_ce_length < max_connection_interval * 2.
  params.min_connection_event_length().Write(0x0000);
  params.max_connection_event_length().Write(0x0000);

  return packet;
}

void LowEnergyConnector::CancelInternal(bool timed_out) {
  PW_DCHECK(request_pending());

  if (pending_request_->canceled) {
    bt_log(WARN, "hci-le", "connection attempt already canceled!");
    return;
  }

  // At this point we do not know whether the pending connection request has
  // completed or not (it may have completed in the controller but that does not
  // mean that we have processed the corresponding LE Connection Complete
  // event). Below we mark the request as canceled and tell the controller to
  // cancel its pending connection attempt.
  pending_request_->canceled = true;
  pending_request_->timed_out = timed_out;

  request_timeout_task_.Cancel();

  // Tell the controller to cancel the connection initiation attempt if a
  // request is outstanding. Otherwise there is no need to talk to the
  // controller.
  if (pending_request_->initiating && hci_.is_alive()) {
    bt_log(
        DEBUG, "hci-le", "telling controller to cancel LE connection attempt");
    auto complete_cb = [](auto, const EventPacket& event) {
      HCI_IS_ERROR(
          event, WARN, "hci-le", "failed to cancel connection request");
    };
    auto cancel = CommandPacket::New<LECreateConnectionCancelCommandView>(
        hci_spec::kLECreateConnectionCancel);
    hci_->command_channel()
        ->SendCommand(std::move(cancel), complete_cb)
        .IgnoreError();

    // A connection complete event will be generated by the controller after
    // processing the cancel command.
    return;
  }

  bt_log(DEBUG, "hci-le", "connection initiation aborted");
  OnCreateConnectionComplete(ToResult(HostError::kCanceled), nullptr);
}

template <typename T>
void LowEnergyConnector::OnConnectionCompleteEvent(const EventPacket& event) {
  auto params = event.view<T>();

  std::optional<DeviceAddress::Type> address_type =
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read());
  if (!address_type.has_value()) {
    bt_log(WARN,
           "hci-le",
           "connection complete event ignored: invalid peer address type "
           "(type: 0x%02x)",
           static_cast<uint8_t>(params.peer_address_type().Read()));
    if (pending_request_) {
      OnCreateConnectionComplete(ToResult(HostError::kPacketMalformed),
                                 nullptr);
    }
    return;
  }

  DeviceAddressBytes address_bytes = DeviceAddressBytes(params.peer_address());
  DeviceAddress peer_address = DeviceAddress(*address_type, address_bytes);

  // First check if this event is related to the currently pending request.
  const bool matches_pending_request =
      pending_request_ &&
      (pending_request_->peer_address.value() == peer_address.value());

  if (Result<> result = event.ToResult(); result.is_error()) {
    if (!matches_pending_request) {
      bt_log(WARN,
             "hci-le",
             "unexpected connection complete event with error received: %s",
             bt_str(result));
      return;
    }

    // The "Unknown Connect Identifier" error code is returned if this event
    // was sent due to a successful cancelation via the
    // HCI_LE_Create_Connection_Cancel command (sent by Cancel()).
    if (pending_request_->timed_out) {
      result = ToResult(HostError::kTimedOut);
    } else if (params.status().Read() == StatusCode::UNKNOWN_CONNECTION_ID) {
      result = ToResult(HostError::kCanceled);
    }

    OnCreateConnectionComplete(result, nullptr);
    return;
  }

  ConnectionRole role = params.role().Read();
  ConnectionHandle handle = params.connection_handle().Read();
  LEConnectionParameters connection_params =
      LEConnectionParameters(params.connection_interval().Read(),
                             params.peripheral_latency().Read(),
                             params.supervision_timeout().Read());

  // If the connection did not match a pending request then we pass the
  // information down to the incoming connection delegate.
  if (!matches_pending_request) {
    delegate_(handle, role, peer_address, connection_params);
    return;
  }

  // A new link layer connection was created. Create an object to track this
  // connection. Destroying this object will disconnect the link.
  auto connection =
      std::make_unique<LowEnergyConnection>(handle,
                                            pending_request_->local_address,
                                            peer_address,
                                            connection_params,
                                            role,
                                            hci_,
                                            pw_dispatcher_);

  Result<> result = fit::ok();
  if (pending_request_->timed_out) {
    result = ToResult(HostError::kTimedOut);
  } else if (pending_request_->canceled) {
    result = ToResult(HostError::kCanceled);
  }

  // If we were requested to cancel the connection after the logical link
  // is created we disconnect it.
  if (result.is_error()) {
    connection = nullptr;
  }

  OnCreateConnectionComplete(result, std::move(connection));
}

void LowEnergyConnector::OnCreateConnectionComplete(
    Result<> result, std::unique_ptr<LowEnergyConnection> link) {
  PW_DCHECK(pending_request_);
  bt_log(DEBUG, "hci-le", "connection complete - status: %s", bt_str(result));

  request_timeout_task_.Cancel();

  auto status_cb = std::move(pending_request_->status_callback);
  pending_request_.reset();

  status_cb(result, std::move(link));
}

void LowEnergyConnector::OnCreateConnectionTimeout() {
  PW_DCHECK(pending_request_);
  bt_log(INFO, "hci-le", "create connection timed out: canceling request");

  // TODO(armansito): This should cancel the connection attempt only if the
  // connection attempt isn't using the filter accept list.
  CancelInternal(true);
}
}  // namespace bt::hci
