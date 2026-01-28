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

#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"

#include <mutex>
#include <optional>

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/acl_data_channel.h"
#include "pw_bluetooth_proxy/internal/channel_proxy_impl.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/flat_map.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy {

namespace {

// Cast subset SourceVariant to a superset TargerVariant.
template <typename TargetVariant, typename SourceVariant>
TargetVariant variant_cast(SourceVariant&& source) {
  return std::visit(
      [](auto&& arg) -> TargetVariant {
        return TargetVariant(std::forward<decltype(arg)>(arg));
      },
      std::forward<SourceVariant>(source));
}

}  // namespace

internal::Mutex internal::L2capChannelManagerImpl::channels_mutex_;

L2capChannelManager::~L2capChannelManager() {
  std::lock_guard lock(links_mutex_);
  ResetLogicalLinksLocked();
}

Result<L2capCoc> L2capChannelManager::AcquireL2capCoc(
    MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    ConnectionOrientedChannelConfig rx_config,
    ConnectionOrientedChannelConfig tx_config,
    Function<void(FlatConstMultiBuf&& payload)>&& receive_fn,
    ChannelEventCallback&& event_fn) {
  std::lock_guard links_lock(links_mutex_);
  auto link_iter = logical_links_.find(connection_handle);
  if (link_iter == logical_links_.end()) {
    PW_LOG_WARN("Attempt to create L2capCoc for non-existent connection: %#x",
                connection_handle);
    return Status::InvalidArgument();
  }
  if (!acl_data_channel_.HasAclConnection(connection_handle)) {
    return Status::Unavailable();
  }

  if (!L2capChannel::AreValidParameters(connection_handle,
                                        /*local_cid=*/rx_config.cid,
                                        /*remote_cid=*/tx_config.cid)) {
    return Status::InvalidArgument();
  }

  UniquePtr<L2capChannel> channel =
      impl_.allocator().MakeUnique<L2capChannel>(*this,
                                                 &rx_multibuf_allocator,
                                                 connection_handle,
                                                 AclTransportType::kLe,
                                                 /*local_cid=*/rx_config.cid,
                                                 /*remote_cid=*/tx_config.cid,
                                                 std::move(event_fn));
  if (channel == nullptr) {
    return Status::ResourceExhausted();
  }

  PW_TRY(channel->InitCreditBasedFlowControl(
      rx_config, tx_config, std::move(receive_fn)));
  L2capCoc client_channel(*channel, tx_config.mtu);
  PW_TRY(client_channel.Init());
  channel.Release();
  return client_channel;
}

Result<BasicL2capChannel> L2capChannelManager::AcquireBasicL2capChannel(
    MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid,
    AclTransportType transport,
    OptionalPayloadReceiveCallback&& payload_from_controller_fn,
    OptionalPayloadReceiveCallback&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  std::lock_guard links_lock(links_mutex_);
  auto link_iter = logical_links_.find(connection_handle);
  if (link_iter == logical_links_.end()) {
    PW_LOG_WARN(
        "Attempt to create BasicL2capChannel for non-existent connection: %#x",
        connection_handle);
    return Status::InvalidArgument();
  }
  if (!acl_data_channel_.HasAclConnection(connection_handle)) {
    return Status::Unavailable();
  }
  if (!L2capChannel::AreValidParameters(
          connection_handle, local_cid, remote_cid)) {
    return Status::InvalidArgument();
  }
  UniquePtr<L2capChannel> channel =
      impl_.allocator().MakeUnique<L2capChannel>(*this,
                                                 &rx_multibuf_allocator,
                                                 connection_handle,
                                                 transport,
                                                 local_cid,
                                                 remote_cid,
                                                 std::move(event_fn));
  if (channel == nullptr) {
    return Status::ResourceExhausted();
  }
  PW_TRY(channel->InitBasic(std::move(payload_from_controller_fn),
                            std::move(payload_from_host_fn)));
  BasicL2capChannel client_channel(*channel);
  PW_TRY(client_channel.Init());
  channel.Release();
  return client_channel;
}

Result<GattNotifyChannel> L2capChannelManager::AcquireGattNotifyChannel(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    ChannelEventCallback&& event_fn) {
  std::lock_guard links_lock(links_mutex_);
  auto link_iter = logical_links_.find(connection_handle);
  if (link_iter == logical_links_.end()) {
    PW_LOG_WARN(
        "Attempt to create GattNotifyChannel for non-existent connection: %#x",
        connection_handle);
    return Status::InvalidArgument();
  }
  if (!acl_data_channel_.HasAclConnection(connection_handle)) {
    return Status::Unavailable();
  }
  if (!L2capChannel::AreValidParameters(
          connection_handle,
          static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL),
          static_cast<uint16_t>(
              emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL))) {
    return Status::InvalidArgument();
  }
  UniquePtr<L2capChannel> channel = impl_.allocator().MakeUnique<L2capChannel>(
      *this,
      /*rx_multibuf_allocator=*/nullptr,
      connection_handle,
      AclTransportType::kLe,
      /*local_cid=*/
      static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL),
      /*remote_cid=*/
      static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL),
      std::move(event_fn));
  if (channel == nullptr) {
    return Status::ResourceExhausted();
  }
  PW_TRY(channel->InitGattNotify(attribute_handle));
  GattNotifyChannel client_channel(*channel, attribute_handle);
  PW_TRY(client_channel.Init());
  channel.Release();
  return client_channel;
}

Status L2capChannelManager::RegisterChannel(L2capChannel& channel) {
  std::lock_guard lock(channels_mutex());
  return RegisterChannelLocked(channel);
}

Status L2capChannelManager::RegisterChannelLocked(L2capChannel& channel) {
  auto result = channels_by_local_cid_.insert(channel.local_handle());
  if (!result.second) {
    L2capChannel& previous = **(result.first);
    if (!previous.IsStale()) {
      PW_LOG_WARN(
          "Attempt to register channel that matches an existing channel: %#x "
          "(local CID: %#x, remote CID: %#x)",
          channel.connection_handle(),
          channel.local_cid(),
          channel.remote_cid());
      return Status::AlreadyExists();
    }
    DeregisterChannelLocked(previous);
    previous.Close();
    impl_.allocator().Delete(&previous);
    result = channels_by_local_cid_.insert(channel.local_handle());
  }
  PW_CHECK(result.second);
  PW_CHECK(channels_by_remote_cid_.insert(channel.remote_handle()).second);
  impl_.OnRegister();
  return OkStatus();
}

void L2capChannelManager::DeregisterChannel(L2capChannel& channel) {
  std::lock_guard lock(channels_mutex());
  DeregisterChannelLocked(channel);
}

void L2capChannelManager::DeregisterChannelLocked(L2capChannel& channel) {
  impl_.OnDeregister(channel);
  channels_by_local_cid_.erase(channel.local_handle());
  channels_by_remote_cid_.erase(channel.remote_handle());
  impl_.OnDeletion();
}

void L2capChannelManager::DeregisterAndCloseChannels(L2capChannelEvent event) {
  std::lock_guard links_lock(links_mutex_);
  ResetLogicalLinksLocked();
  {
    std::lock_guard channels_lock(channels_mutex());
    channels_by_remote_cid_.clear();
    for (auto iter = channels_by_local_cid_.begin();
         iter != channels_by_local_cid_.end();) {
      L2capChannel& channel = **iter;
      iter = channels_by_local_cid_.erase(iter);
      channel.Close(event);
      stale_.insert(channel.local_handle());
    }
    impl_.OnDeletion();
  }
  DeleteStaleChannels();
}

void L2capChannelManager::DeleteStaleChannels() {
  L2capChannelMap stale;
  {
    std::lock_guard channels_lock(channels_mutex());
    stale.swap(stale_);
  }
  for (auto iter = stale.begin(); iter != stale.end();) {
    L2capChannel& channel = **iter;
    iter = stale.erase(iter);
    impl_.allocator_.Delete(&channel);
  }
}

Result<H4PacketWithH4> L2capChannelManager::GetAclH4Packet(uint16_t size) {
  // Use Allocate instead of New to avoid tracking the size, which would either
  // be a breaking change to H4PacketWithH4 or not fit in Function's default
  // capture size of 1 pointer.
  void* allocation =
      impl_.allocator().Allocate(allocator::Layout(size, alignof(uint8_t)));
  if (allocation == nullptr) {
    PW_LOG_WARN("Could not allocate H4 buffer of size %hu", size);
    return Status::Unavailable();
  }
  span<uint8_t> h4_buff(static_cast<uint8_t*>(allocation), size);

  H4PacketWithH4 h4_packet(
      h4_buff,
      /*release_fn=*/[this](const uint8_t* buffer) {
        // This const_cast is needed to avoid changing the
        // function signature and breaking downstream
        // users.
        impl_.allocator().Deallocate(const_cast<uint8_t*>(buffer));
        // TODO: https://pwbug.dev/421249712 - Only report
        // if we were previously out of buffers.
        ForceDrainChannelQueues();
      });
  h4_packet.SetH4Type(emboss::H4PacketType::ACL_DATA);

  return h4_packet;
}

void L2capChannelManager::ForceDrainChannelQueues() {
  ReportNewTxPacketsOrCredits();
  DrainChannelQueuesIfNewTx();
}

std::optional<LockedL2capChannel> L2capChannelManager::FindChannelByLocalCid(
    uint16_t connection_handle, uint16_t local_cid) PW_NO_LOCK_SAFETY_ANALYSIS {
  // Lock annotations don't work with unique_lock
  std::unique_lock lock(channels_mutex());
  L2capChannel* channel =
      FindChannelByLocalCidLocked(connection_handle, local_cid);
  if (!channel) {
    return std::nullopt;
  }
  return LockedL2capChannel(*channel, std::move(lock));
}

std::optional<LockedL2capChannel> L2capChannelManager::FindChannelByRemoteCid(
    uint16_t connection_handle,
    uint16_t remote_cid) PW_NO_LOCK_SAFETY_ANALYSIS {
  // Lock annotations don't work with unique_lock
  std::unique_lock lock(channels_mutex());
  L2capChannel* channel =
      FindChannelByRemoteCidLocked(connection_handle, remote_cid);
  if (!channel) {
    return std::nullopt;
  }
  return LockedL2capChannel(*channel, std::move(lock));
}

L2capChannel* L2capChannelManager::FindChannelByLocalCidLocked(
    uint16_t connection_handle, uint16_t local_cid) PW_NO_LOCK_SAFETY_ANALYSIS {
  uint32_t key = L2capChannel::MakeKey(connection_handle, local_cid);
  auto it = channels_by_local_cid_.find(key);
  if (it == channels_by_local_cid_.end() || (*it)->IsStale()) {
    return nullptr;
  }
  return it->get();
}

L2capChannel* L2capChannelManager::FindChannelByRemoteCidLocked(
    uint16_t connection_handle,
    uint16_t remote_cid) PW_NO_LOCK_SAFETY_ANALYSIS {
  uint32_t key = L2capChannel::MakeKey(connection_handle, remote_cid);
  auto it = channels_by_remote_cid_.find(key);
  if (it == channels_by_remote_cid_.end() || (*it)->IsStale()) {
    return nullptr;
  }
  return it->get();
}

void L2capChannelManager::Advance(L2capChannelIterator& it) {
  if (++it == channels_by_local_cid_.end()) {
    it = channels_by_local_cid_.begin();
  }
}

void L2capChannelManager::RegisterStatusDelegate(
    L2capStatusDelegate& delegate) {
  status_tracker_.RegisterDelegate(delegate);
}

void L2capChannelManager::UnregisterStatusDelegate(
    L2capStatusDelegate& delegate) {
  status_tracker_.UnregisterDelegate(delegate);
}

void L2capChannelManager::HandleConnectionComplete(
    const L2capChannelConnectionInfo& info) {
  status_tracker_.HandleConnectionComplete(info);
}

void L2capChannelManager::HandleConfigurationChanged(
    const L2capChannelConfigurationInfo& info) {
  status_tracker_.HandleConfigurationChanged(info);
}

void L2capChannelManager::HandleAclDisconnectionComplete(
    uint16_t connection_handle) {
  PW_LOG_INFO(
      "btproxy: L2capChannelManager::HandleAclDisconnectionComplete - "
      "connection_handle: %u",
      connection_handle);
  {
    std::lock_guard links_lock(links_mutex_);
    auto link_it = logical_links_.find(connection_handle);
    if (link_it != logical_links_.end()) {
      internal::L2capLogicalLinkInterface* link = &(*link_it);
      logical_links_.erase(link_it);
      impl_.allocator().Delete(link);
    }

    std::lock_guard lock(channels_mutex());
    uint32_t key = L2capChannel::MakeKey(connection_handle, 0);
    auto channel_it = channels_by_local_cid_.lower_bound(key);
    while (channel_it != channels_by_local_cid_.end()) {
      L2capChannel& channel = **channel_it++;
      if (channel.connection_handle() == connection_handle &&
          channel.state() == L2capChannel::State::kRunning) {
        DeregisterChannelLocked(channel);
        channel.Close();
        stale_.insert(channel.local_handle());
      }
    }
  }
  DeleteStaleChannels();

  status_tracker_.HandleAclDisconnectionComplete(connection_handle);
}

void L2capChannelManager::HandleDisconnectionCompleteLocked(
    const L2capStatusTracker::DisconnectParams& params)
    PW_NO_LOCK_SAFETY_ANALYSIS {
  // Must be called under channels_lock_ but we can't use proper lock annotation
  // here since the call comes via signaling channel.
  // TODO: https://pwbug.dev/390511432 - Figure out way to add annotations to
  // enforce this invariant.
  uint32_t key =
      L2capChannel::MakeKey(params.connection_handle, params.local_cid);
  auto it = channels_by_local_cid_.find(key);
  if (it != channels_by_local_cid_.end()) {
    L2capChannel& channel = **it;
    DeregisterChannelLocked(channel);
    channel.Close();
    impl_.allocator().Delete(&channel);
  }
  status_tracker_.HandleDisconnectionComplete(params);
}

void L2capChannelManager::DeliverPendingEvents() {
  status_tracker_.DeliverPendingEvents();
}

Status L2capChannelManager::AddConnection(uint16_t connection_handle,
                                          AclTransportType transport) {
  std::lock_guard lock(links_mutex_);
  auto iter = logical_links_.find(connection_handle);
  if (iter != logical_links_.end()) {
    return Status::AlreadyExists();
  }

  auto link = impl_.allocator().MakeUnique<internal::L2capLogicalLink>(
      connection_handle, transport, *this, acl_data_channel_);
  if (link == nullptr) {
    return Status::ResourceExhausted();
  }
  PW_TRY(link->Init());
  PW_LOG_INFO("Added L2CAP connection %#x", connection_handle);
  logical_links_.insert(*link.Release());
  return OkStatus();
}

Status L2capChannelManager::SendFlowControlCreditInd(
    uint16_t connection_handle,
    uint16_t channel_id,
    uint16_t credits,
    MultiBufAllocator& multibuf_allocator) {
  std::lock_guard lock(links_mutex_);
  auto iter = logical_links_.find(connection_handle);
  if (iter == logical_links_.end()) {
    return Status::NotFound();
  }
  return iter->SendFlowControlCreditInd(
      channel_id, credits, multibuf_allocator);
}

std::optional<uint16_t> L2capChannelManager::MaxDataPacketLengthForTransport(
    AclTransportType transport) const {
  return acl_data_channel_.MaxDataPacketLengthForTransport(transport);
}

Result<uint16_t> L2capChannelManager::MaxL2capPayloadSize(
    AclTransportType transport) const {
  std::optional<uint16_t> max_acl_length =
      MaxDataPacketLengthForTransport(transport);
  if (!max_acl_length.has_value()) {
    return Status::FailedPrecondition();
  }
  if (*max_acl_length <= emboss::BasicL2capHeader::IntrinsicSizeInBytes()) {
    return Status::FailedPrecondition();
  }
  return *max_acl_length - emboss::BasicL2capHeader::IntrinsicSizeInBytes();
}

void L2capChannelManager::ResetLogicalLinksLocked() {
  for (auto iter = logical_links_.begin(); iter != logical_links_.end();) {
    internal::L2capLogicalLinkInterface* link = &(*iter);
    iter = logical_links_.erase(iter);
    impl_.allocator().Delete(link);
  }
}

Result<UniquePtr<ChannelProxy>>
L2capChannelManager::DoInterceptBasicModeChannel(
    ConnectionHandle connection_handle,
    uint16_t local_channel_id,
    uint16_t remote_channel_id,
    AclTransportType transport,
    BufferReceiveFunction&& payload_from_controller_fn,
    BufferReceiveFunction&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  std::lock_guard links_lock(links_mutex_);
  auto link_iter =
      logical_links_.find(static_cast<uint16_t>(connection_handle));
  if (link_iter == logical_links_.end()) {
    PW_LOG_WARN(
        "Attempt to create BasicL2capChannel for non-existent connection: %#x",
        static_cast<uint16_t>(connection_handle));
    return Status::InvalidArgument();
  }
  if (!acl_data_channel_.HasAclConnection(
          static_cast<uint16_t>(connection_handle))) {
    return Status::Unavailable();
  }
  if (!L2capChannel::AreValidParameters(
          static_cast<uint16_t>(connection_handle),
          local_channel_id,
          remote_channel_id)) {
    return Status::InvalidArgument();
  }

  UniquePtr<L2capChannel> channel = impl_.allocator().MakeUnique<L2capChannel>(
      *this,
      /*rx_multibuf_allocator=*/&multibuf_allocator_,
      static_cast<uint16_t>(connection_handle),
      transport,
      local_channel_id,
      remote_channel_id,
      std::move(event_fn));

  if (channel == nullptr) {
    return Status::ResourceExhausted();
  }

  std::optional<uint16_t> max_l2cap_payload_size =
      channel->MaxL2capPayloadSize();
  if (!max_l2cap_payload_size.has_value()) {
    PW_LOG_ERROR("Maximum L2CAP payload size is not set.");
    return Status::FailedPrecondition();
  }

  UniquePtr<internal::ChannelProxyImpl> channel_proxy =
      impl_.allocator().MakeUnique<internal::ChannelProxyImpl>(
          max_l2cap_payload_size.value(), *channel);
  if (channel_proxy == nullptr) {
    return Status::ResourceExhausted();
  }

  PW_TRY(channel->InitBasic(
      variant_cast<L2capChannel::FromControllerFn>(
          std::move(payload_from_controller_fn)),
      variant_cast<L2capChannel::FromHostFn>(std::move(payload_from_host_fn))));

  PW_TRY(channel_proxy->Init());
  channel.Release();
  return channel_proxy;
}

Result<UniquePtr<ChannelProxy>>
L2capChannelManager::DoInterceptCreditBasedFlowControlChannel(
    ConnectionHandle connection_handle,
    ConnectionOrientedChannelConfig rx_config,
    ConnectionOrientedChannelConfig tx_config,
    MultiBufReceiveFunction&& receive_fn,
    ChannelEventCallback&& event_fn) {
  std::lock_guard links_lock(links_mutex_);
  auto link_iter = logical_links_.find(cpp23::to_underlying(connection_handle));
  if (link_iter == logical_links_.end()) {
    PW_LOG_WARN("Attempt to create L2capCoc for non-existent connection: %#x",
                cpp23::to_underlying(connection_handle));
    return Status::InvalidArgument();
  }
  if (!acl_data_channel_.HasAclConnection(
          cpp23::to_underlying(connection_handle))) {
    return Status::Unavailable();
  }

  if (!L2capChannel::AreValidParameters(cpp23::to_underlying(connection_handle),
                                        /*local_cid=*/rx_config.cid,
                                        /*remote_cid=*/tx_config.cid)) {
    return Status::InvalidArgument();
  }

  UniquePtr<L2capChannel> channel = impl_.allocator().MakeUnique<L2capChannel>(
      *this,
      /*rx_multibuf_allocator=*/&multibuf_allocator_,
      cpp23::to_underlying(connection_handle),
      AclTransportType::kLe,
      /*local_cid=*/rx_config.cid,
      /*remote_cid=*/tx_config.cid,
      std::move(event_fn));
  if (channel == nullptr) {
    return Status::ResourceExhausted();
  }

  std::optional<uint16_t> max_l2cap_payload_size =
      channel->MaxL2capPayloadSize();
  if (!max_l2cap_payload_size.has_value()) {
    PW_LOG_ERROR("Maximum L2CAP payload size is not set.");
    return Status::FailedPrecondition();
  }

  UniquePtr<internal::ChannelProxyImpl> channel_proxy =
      impl_.allocator().MakeUnique<internal::ChannelProxyImpl>(
          max_l2cap_payload_size.value(), *channel);
  if (channel_proxy == nullptr) {
    return Status::ResourceExhausted();
  }

  PW_TRY(channel->InitCreditBasedFlowControl(
      rx_config, tx_config, std::move(receive_fn)));
  PW_TRY(channel_proxy->Init());
  channel.Release();
  return channel_proxy;
}

namespace internal {

void L2capChannelManagerImpl::OnDeregister(const L2capChannel& channel) {
  if (lrd_channel_ != manager_.channels_by_local_cid_.end() &&
      lrd_channel_->get() == &channel) {
    manager_.Advance(lrd_channel_);
  }
  if (round_robin_terminus_ != manager_.channels_by_local_cid_.end() &&
      round_robin_terminus_->get() == &channel) {
    manager_.Advance(round_robin_terminus_);
  }
}

void L2capChannelManagerImpl::OnDeletion() {
  if (manager_.channels_by_local_cid_.empty()) {
    lrd_channel_ = manager_.channels_by_local_cid_.end();
    round_robin_terminus_ = manager_.channels_by_local_cid_.end();
  }
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy
