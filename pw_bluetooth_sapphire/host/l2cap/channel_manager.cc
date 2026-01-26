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

#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/a2dp_offload_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"

namespace bt::l2cap {

namespace {

constexpr const char* kInspectServicesNodeName = "services";
constexpr const char* kInspectServiceNodePrefix = "service_";
constexpr const char* kInspectLogicalLinksNodeName = "logical_links";
constexpr const char* kInspectLogicalLinkNodePrefix = "logical_link_";
constexpr const char* kInspectPsmPropertyName = "psm";

}  // namespace

class ChannelManagerImpl final : public ChannelManager {
 public:
  using LinkErrorCallback = fit::closure;

  ChannelManagerImpl(
      hci::AclDataChannel* acl_data_channel,
      hci::CommandChannel* cmd_channel,
      bool random_channel_ids,
      pw::async::Dispatcher& dispatcher,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);
  ~ChannelManagerImpl() override;

  void AddACLConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      l2cap::LinkErrorCallback link_error_callback,
      l2cap::SecurityUpgradeCallback security_callback,
      fit::callback<void(BrEdrFixedChannels)> fixed_channels_callback) override;

  [[nodiscard]] LEFixedChannels AddLEConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      LinkErrorCallback link_error_cb,
      LEConnectionParameterUpdateCallback conn_param_cb,
      SecurityUpgradeCallback security_cb) override;

  void RemoveConnection(hci_spec::ConnectionHandle handle) override;

  void AssignLinkSecurityProperties(hci_spec::ConnectionHandle handle,
                                    sm::SecurityProperties security) override;

  Channel::WeakPtr OpenFixedChannel(
      hci_spec::ConnectionHandle connection_handle,
      ChannelId channel_id) override;

  void OpenL2capChannel(hci_spec::ConnectionHandle handle,
                        Psm psm,
                        ChannelParameters params,
                        ChannelCallback cb) override;

  bool RegisterService(Psm psm,
                       ChannelParameters params,
                       ChannelCallback cb) override;
  void UnregisterService(Psm psm) override;

  void RequestConnectionParameterUpdate(
      hci_spec::ConnectionHandle handle,
      hci_spec::LEPreferredConnectionParameters params,
      ConnectionParameterUpdateRequestCallback request_cb) override;

  void AttachInspect(inspect::Node& parent, std::string name) override;

  internal::LogicalLink::WeakPtr LogicalLinkForTesting(
      hci_spec::ConnectionHandle handle) override;

  std::optional<std::unique_ptr<AutosniffSuppressInterface>> SuppressAutosniff(
      hci_spec::ConnectionHandle handle, const char* reason) override;

 private:
  // Returns a handler for data packets received from the Bluetooth controller
  // bound to this object.
  hci::ACLPacketHandler MakeInboundDataHandler();

  // Called when an ACL data packet is received from the controller. This method
  // is responsible for routing the packet to the corresponding LogicalLink.
  void OnACLDataReceived(hci::ACLDataPacketPtr data_packet);

  // Called by the various Register functions. Returns a pointer to the newly
  // added link.
  internal::LogicalLink* RegisterInternal(
      hci_spec::ConnectionHandle handle,
      bt::LinkType ll_type,
      pw::bluetooth::emboss::ConnectionRole role,
      size_t max_payload_size);

  std::optional<internal::LogicalLink::WeakPtr> GetLogicalLink(
      hci_spec::ConnectionHandle handle);

  // If a service (identified by |psm|) requested has been registered, return a
  // ServiceInfo object containing preferred channel parameters and a callback
  // that passes an inbound channel to the registrant. The callback may be
  // called repeatedly to pass multiple channels for |psm|, but should not be
  // stored because the service may be unregistered at a later time. Calls for
  // unregistered services return null.
  std::optional<ServiceInfo> QueryService(hci_spec::ConnectionHandle handle,
                                          Psm psm);

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  pw::async::Dispatcher& pw_dispatcher_;

  // Maximum sizes for data packet payloads from host to controller.
  size_t max_acl_payload_size_;
  size_t max_le_payload_size_;

  hci::AclDataChannel* acl_data_channel_;
  hci::CommandChannel* cmd_channel_;

  std::unique_ptr<A2dpOffloadManager> a2dp_offload_manager_;

  using LinkMap = std::unordered_map<hci_spec::ConnectionHandle,
                                     std::unique_ptr<internal::LogicalLink>>;
  LinkMap ll_map_;
  inspect::Node ll_node_;

  // Stores packets received on a connection handle before a link for it has
  // been created.
  using PendingPacketMap =
      std::unordered_map<hci_spec::ConnectionHandle,
                         std::queue<hci::ACLDataPacketPtr>>;
  PendingPacketMap pending_packets_;

  // Store information required to create and forward channels for locally-
  // hosted services.
  struct ServiceData {
    void AttachInspect(inspect::Node& parent);
    ServiceInfo info;
    Psm psm;
    inspect::Node node;
    inspect::StringProperty psm_property;
  };
  using ServiceMap = std::unordered_map<Psm, ServiceData>;
  ServiceMap services_;
  inspect::Node services_node_;
  inspect::Node node_;

  // Stored info on whether random channel ids are requested.
  bool random_channel_ids_;

  WeakSelf<ChannelManagerImpl> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ChannelManagerImpl);
};

ChannelManagerImpl::ChannelManagerImpl(
    hci::AclDataChannel* acl_data_channel,
    hci::CommandChannel* cmd_channel,
    bool random_channel_ids,
    pw::async::Dispatcher& dispatcher,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : wake_lease_provider_(wake_lease_provider),
      pw_dispatcher_(dispatcher),
      acl_data_channel_(acl_data_channel),
      cmd_channel_(cmd_channel),
      a2dp_offload_manager_(
          std::make_unique<A2dpOffloadManager>(cmd_channel_->AsWeakPtr())),
      random_channel_ids_(random_channel_ids),
      weak_self_(this) {
  PW_CHECK(acl_data_channel_);
  max_acl_payload_size_ = acl_data_channel_->GetBufferInfo().max_data_length();
  max_le_payload_size_ = acl_data_channel_->GetLeBufferInfo().max_data_length();
  acl_data_channel_->SetDataRxHandler(MakeInboundDataHandler());

  bt_log(DEBUG, "l2cap", "initialized");
}

ChannelManagerImpl::~ChannelManagerImpl() {
  bt_log(DEBUG, "l2cap", "shutting down");

  // Explicitly shut down all links to force associated L2CAP channels to
  // release their strong references.
  for (auto& [handle, link] : ll_map_) {
    link->Close();
  }
}

hci::ACLPacketHandler ChannelManagerImpl::MakeInboundDataHandler() {
  return [self = weak_self_.GetWeakPtr()](auto packet) {
    if (self.is_alive()) {
      self->OnACLDataReceived(std::move(packet));
    }
  };
}

void ChannelManagerImpl::AddACLConnection(
    hci_spec::ConnectionHandle handle,
    pw::bluetooth::emboss::ConnectionRole role,
    l2cap::LinkErrorCallback link_error_callback,
    l2cap::SecurityUpgradeCallback security_callback,
    fit::callback<void(BrEdrFixedChannels)> fixed_channels_callback) {
  bt_log(DEBUG, "l2cap", "register ACL link (handle: %#.4x)", handle);

  auto* ll =
      RegisterInternal(handle, bt::LinkType::kACL, role, max_acl_payload_size_);
  ll->set_error_callback(std::move(link_error_callback));
  ll->set_security_upgrade_callback(std::move(security_callback));
  ll->OpenFixedChannelAsync(
      kSMPChannelId,
      [cb = std::move(fixed_channels_callback)](Channel::WeakPtr smp) mutable {
        cb(BrEdrFixedChannels{.smp = std::move(smp)});
      });
}

ChannelManagerImpl::LEFixedChannels ChannelManagerImpl::AddLEConnection(
    hci_spec::ConnectionHandle handle,
    pw::bluetooth::emboss::ConnectionRole role,
    LinkErrorCallback link_error_cb,
    LEConnectionParameterUpdateCallback conn_param_cb,
    SecurityUpgradeCallback security_cb) {
  bt_log(DEBUG, "l2cap", "register LE link (handle: %#.4x)", handle);

  auto* ll =
      RegisterInternal(handle, bt::LinkType::kLE, role, max_le_payload_size_);
  ll->set_error_callback(std::move(link_error_cb));
  ll->set_security_upgrade_callback(std::move(security_cb));
  ll->set_connection_parameter_update_callback(std::move(conn_param_cb));

  Channel::WeakPtr att = OpenFixedChannel(handle, kATTChannelId);
  Channel::WeakPtr smp = OpenFixedChannel(handle, kLESMPChannelId);
  PW_CHECK(att.is_alive());
  PW_CHECK(smp.is_alive());
  return LEFixedChannels{.att = std::move(att), .smp = std::move(smp)};
}

void ChannelManagerImpl::RemoveConnection(hci_spec::ConnectionHandle handle) {
  bt_log(DEBUG, "l2cap", "unregister link (handle: %#.4x)", handle);

  pending_packets_.erase(handle);
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(DEBUG,
           "l2cap",
           "attempt to unregister unknown link (handle: %#.4x)",
           handle);
    return;
  }

  // Explicitly shut down the link to force associated L2CAP channels to release
  // their strong references.
  iter->second->Close();
  ll_map_.erase(iter);
}

void ChannelManagerImpl::AssignLinkSecurityProperties(
    hci_spec::ConnectionHandle handle, sm::SecurityProperties security) {
  bt_log(DEBUG,
         "l2cap",
         "received new security properties (handle: %#.4x)",
         handle);

  auto link = GetLogicalLink(handle);
  if (!link.has_value()) {
    bt_log(DEBUG, "l2cap", "ignoring new security properties on unknown link");
    return;
  }

  link.value()->AssignSecurityProperties(security);
}

Channel::WeakPtr ChannelManagerImpl::OpenFixedChannel(
    hci_spec::ConnectionHandle handle, ChannelId channel_id) {
  auto link = GetLogicalLink(handle);
  if (!link.has_value()) {
    bt_log(ERROR,
           "l2cap",
           "cannot open fixed channel on unknown connection handle: %#.4x",
           handle);
    return Channel::WeakPtr();
  }

  return link.value()->OpenFixedChannel(channel_id);
}

void ChannelManagerImpl::OpenL2capChannel(hci_spec::ConnectionHandle handle,
                                          Psm psm,
                                          ChannelParameters params,
                                          ChannelCallback cb) {
  auto link = GetLogicalLink(handle);
  if (!link.has_value()) {
    bt_log(ERROR,
           "l2cap",
           "Cannot open channel on unknown connection handle: %#.4x",
           handle);
    cb(Channel::WeakPtr());
    return;
  }

  link.value()->OpenChannel(psm, params, std::move(cb));
}

bool ChannelManagerImpl::RegisterService(Psm psm,
                                         ChannelParameters params,
                                         ChannelCallback cb) {
  // v5.0 Vol 3, Part A, Sec 4.2: PSMs shall be odd and the least significant
  // bit of the most significant byte shall be zero
  if (((psm & 0x0001) != 0x0001) || ((psm & 0x0100) != 0x0000)) {
    return false;
  }

  auto iter = services_.find(psm);
  if (iter != services_.end()) {
    return false;
  }

  ServiceData service{.info = ServiceInfo(params, std::move(cb)),
                      .psm = psm,
                      .node = {},
                      .psm_property = {}};
  service.AttachInspect(services_node_);
  services_.emplace(psm, std::move(service));
  return true;
}

void ChannelManagerImpl::UnregisterService(Psm psm) { services_.erase(psm); }

void ChannelManagerImpl::RequestConnectionParameterUpdate(
    hci_spec::ConnectionHandle handle,
    hci_spec::LEPreferredConnectionParameters params,
    ConnectionParameterUpdateRequestCallback request_cb) {
  auto link = GetLogicalLink(handle);
  if (!link.has_value()) {
    bt_log(DEBUG,
           "l2cap",
           "ignoring Connection Parameter Update request on unknown link");
    return;
  }

  link.value()->SendConnectionParameterUpdateRequest(params,
                                                     std::move(request_cb));
}

void ChannelManagerImpl::AttachInspect(inspect::Node& parent,
                                       std::string name) {
  if (!parent) {
    return;
  }

  node_ = parent.CreateChild(name);

  services_node_ = node_.CreateChild(kInspectServicesNodeName);
  for (auto& [psm, service] : services_) {
    service.AttachInspect(services_node_);
  }

  ll_node_ = node_.CreateChild(kInspectLogicalLinksNodeName);
  for (auto& [_, ll] : ll_map_) {
    ll->AttachInspect(ll_node_,
                      ll_node_.UniqueName(kInspectLogicalLinkNodePrefix));
  }
}

std::optional<internal::LogicalLink::WeakPtr>
ChannelManagerImpl::GetLogicalLink(hci_spec::ConnectionHandle handle) {
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    return std::nullopt;
  }
  return iter->second->GetWeakPtr();
}

internal::LogicalLink::WeakPtr ChannelManagerImpl::LogicalLinkForTesting(
    hci_spec::ConnectionHandle handle) {
  return GetLogicalLink(handle).value_or(internal::LogicalLink::WeakPtr());
}

std::optional<std::unique_ptr<AutosniffSuppressInterface>>
ChannelManagerImpl::SuppressAutosniff(hci_spec::ConnectionHandle handle,
                                      const char* reason) {
  std::optional<internal::LogicalLink::WeakPtr> link = GetLogicalLink(handle);
  if (!link.has_value() || !link->is_alive()) {
    return std::nullopt;
  }
  return link->get().SuppressAutosniff(reason);
}

// Called when an ACL data packet is received from the controller. This method
// is responsible for routing the packet to the corresponding LogicalLink.
void ChannelManagerImpl::OnACLDataReceived(hci::ACLDataPacketPtr packet) {
  auto handle = packet->connection_handle();
  TRACE_DURATION(
      "bluetooth", "ChannelManagerImpl::OnDataReceived", "handle", handle);

  auto link = GetLogicalLink(handle);
  PendingPacketMap::iterator pp_iter;

  // If a LogicalLink does not exist, we set up a queue for its packets to be
  // delivered when the LogicalLink gets created.
  if (!link.has_value()) {
    pp_iter =
        pending_packets_.emplace(handle, std::queue<hci::ACLDataPacketPtr>())
            .first;
  } else {
    // A logical link exists. |pp_iter| will be valid only if the drain task has
    // not run yet (see ChannelManagerImpl::RegisterInternal()).
    pp_iter = pending_packets_.find(handle);
  }

  if (pp_iter != pending_packets_.end()) {
    packet->set_trace_id(TRACE_NONCE());
    TRACE_FLOW_BEGIN("bluetooth",
                     "ChannelMaager::OnDataReceived queued",
                     packet->trace_id());
    pp_iter->second.push(std::move(packet));
    bt_log(TRACE, "l2cap", "queued rx packet on handle: %#.4x", handle);
    return;
  }

  link.value()->HandleRxPacket(std::move(packet));
}

internal::LogicalLink* ChannelManagerImpl::RegisterInternal(
    hci_spec::ConnectionHandle handle,
    bt::LinkType ll_type,
    pw::bluetooth::emboss::ConnectionRole role,
    size_t max_payload_size) {
  TRACE_DURATION(
      "bluetooth", "ChannelManagerImpl::RegisterInternal", "handle", handle);

  // TODO(armansito): Return nullptr instead of asserting. Callers shouldn't
  // assume this will succeed.
  auto link = GetLogicalLink(handle);
  PW_DCHECK(
      !link.has_value(), "connection handle re-used! (handle=%#.4x)", handle);

  auto ll = std::make_unique<internal::LogicalLink>(
      handle,
      ll_type,
      role,
      max_payload_size,
      fit::bind_member<&ChannelManagerImpl::QueryService>(this),
      acl_data_channel_,
      cmd_channel_,
      random_channel_ids_,
      *a2dp_offload_manager_,
      pw_dispatcher_,
      wake_lease_provider_);

  if (ll_node_) {
    ll->AttachInspect(ll_node_,
                      ll_node_.UniqueName(kInspectLogicalLinkNodePrefix));
  }

  // Route all pending packets to the link.
  auto pp_iter = pending_packets_.find(handle);
  if (pp_iter != pending_packets_.end()) {
    auto& packets = pp_iter->second;
    while (!packets.empty()) {
      auto packet = std::move(packets.front());
      packets.pop();
      TRACE_FLOW_END("bluetooth",
                     "ChannelManagerImpl::OnDataReceived queued",
                     packet->trace_id());
      ll->HandleRxPacket(std::move(packet));
    }
    pending_packets_.erase(pp_iter);
  }

  auto* ll_raw = ll.get();
  ll_map_[handle] = std::move(ll);

  return ll_raw;
}

std::optional<ChannelManager::ServiceInfo> ChannelManagerImpl::QueryService(
    hci_spec::ConnectionHandle, Psm psm) {
  auto iter = services_.find(psm);
  if (iter == services_.end()) {
    return std::nullopt;
  }

  // |channel_cb| will be called in LogicalLink. Each callback in |services_|
  // already trampolines to the appropriate dispatcher (passed to
  // RegisterService).
  return ServiceInfo(iter->second.info.channel_params,
                     iter->second.info.channel_cb.share());
}

void ChannelManagerImpl::ServiceData::AttachInspect(inspect::Node& parent) {
  if (!parent) {
    return;
  }
  node = parent.CreateChild(parent.UniqueName(kInspectServiceNodePrefix));
  psm_property = node.CreateString(kInspectPsmPropertyName, PsmToString(psm));
}

std::unique_ptr<ChannelManager> ChannelManager::Create(
    hci::AclDataChannel* acl_data_channel,
    hci::CommandChannel* cmd_channel,
    bool random_channel_ids,
    pw::async::Dispatcher& dispatcher,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider) {
  return std::make_unique<ChannelManagerImpl>(acl_data_channel,
                                              cmd_channel,
                                              random_channel_ids,
                                              dispatcher,
                                              wake_lease_provider);
}

}  // namespace bt::l2cap
