// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0

#include "pw_bluetooth_proxy/proxy_host.h"

namespace pw::bluetooth::proxy {

Status ProxyHost::SetDispatcher(async2::Dispatcher&) {
  // Not needed in sync mode.
  return Status::Unimplemented();
}

void ProxyHost::HandleH4HciFromHost(H4PacketWithH4&& h4_packet) {
  DoHandleH4HciFromHost(std::move(h4_packet));
}

void ProxyHost::HandleH4HciFromController(H4PacketWithHci&& h4_packet) {
  DoHandleH4HciFromController(std::move(h4_packet));
}

void ProxyHost::Reset() { DoReset(); }

pw::Result<L2capCoc> ProxyHost::AcquireL2capCoc(
    multibuf::MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    L2capCoc::CocConfig rx_config,
    L2capCoc::CocConfig tx_config,
    Function<void(multibuf::MultiBuf&& payload)>&& receive_fn,
    ChannelEventCallback&& event_fn) {
  return DoAcquireL2capCoc(rx_multibuf_allocator,
                           connection_handle,
                           rx_config,
                           tx_config,
                           std::move(receive_fn),
                           std::move(event_fn));
}

pw::Result<BasicL2capChannel> ProxyHost::AcquireBasicL2capChannel(
    multibuf::MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid,
    AclTransportType transport,
    OptionalPayloadReceiveCallback&& payload_from_controller_fn,
    OptionalPayloadReceiveCallback&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  return DoAcquireBasicL2capChannel(rx_multibuf_allocator,
                                    connection_handle,
                                    local_cid,
                                    remote_cid,
                                    transport,
                                    std::move(payload_from_controller_fn),
                                    std::move(payload_from_host_fn),
                                    std::move(event_fn));
}

pw::Result<GattNotifyChannel> ProxyHost::AcquireGattNotifyChannel(
    int16_t connection_handle,
    uint16_t attribute_handle,
    ChannelEventCallback&& event_fn) {
  return DoAcquireGattNotifyChannel(
      connection_handle, attribute_handle, std::move(event_fn));
}

bool ProxyHost::HasSendLeAclCapability() const {
  return DoHasSendLeAclCapability();
}

bool ProxyHost::HasSendBrEdrAclCapability() const {
  return DoHasSendBrEdrAclCapability();
}

uint16_t ProxyHost::GetNumFreeLeAclPackets() const {
  return DoGetNumFreeLeAclPackets();
}

uint16_t ProxyHost::GetNumFreeBrEdrAclPackets() const {
  return DoGetNumFreeBrEdrAclPackets();
}

void ProxyHost::RegisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  DoRegisterL2capStatusDelegate(delegate);
}

void ProxyHost::UnregisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  DoUnregisterL2capStatusDelegate(delegate);
}

Result<UniquePtr<ChannelProxy>> ProxyHost::DoInterceptBasicModeChannel(
    ConnectionHandle connection_handle,
    uint16_t local_channel_id,
    uint16_t remote_channel_id,
    AclTransportType transport,
    BufferReceiveFunction&& payload_from_controller_fn,
    BufferReceiveFunction&& payload_from_host_fn,
    ChannelEventCallback&& event_fn,
    bool allow_data_loss) {
  return InternalDoInterceptBasicModeChannel(
      connection_handle,
      local_channel_id,
      remote_channel_id,
      transport,
      std::move(payload_from_controller_fn),
      std::move(payload_from_host_fn),
      std::move(event_fn),
      allow_data_loss);
}

}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC == 0
