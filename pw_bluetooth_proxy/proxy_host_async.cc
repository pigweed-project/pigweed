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

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include <mutex>

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/proxy_host_async.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_log/log.h"

namespace pw::bluetooth::proxy {

using BasicRequest = internal::ProxyHostImpl::BasicRequest;
using ChannelRequest = internal::ProxyHostImpl::ChannelRequest;

Status ProxyHost::SetDispatcher(async2::Dispatcher& dispatcher) {
  PW_TRY(l2cap_channel_manager_.impl().Init(dispatcher));
  impl_.Start();
  return OkStatus();
}

void ProxyHost::HandleH4HciFromHost(H4PacketWithH4&& h4_packet) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    DoHandleH4HciFromHost(std::move(h4_packet));
    return;
  }
  // TODO(b/411168474): The H4 packet must remain valid as it is asynchronously
  // handled by the ProxyHost. Currently, the `H4PacketWithH4` wraps a span that
  // is backed by some container memory. If the `release_fn_` field is set on
  // the packet, it is assumed that the packet should clean itself up. If not,
  // the container may drop or free this memory when this method returns.
  //
  // Ideally, `H4PacketWithH4` would wrap a multibuf that managed the lifetime
  // of the memory backing the packet. Until then, we check whether a
  // `ReleaseFn` is set, and if not, copy its data to an allocated packet with
  // a `ReleaseFn`.
  if (h4_packet.HasReleaseFn()) {
    impl_.SendH4PacketFromHost(std::move(h4_packet));
    return;
  }
  Allocator& allocator = l2cap_channel_manager_.impl().allocator();
  Result<H4PacketWithH4> owned_h4_packet_result =
      H4PacketWithH4::CopyFrom(allocator, h4_packet);
  if (!owned_h4_packet_result.ok()) {
    PW_LOG_ERROR(
        "Dropping H4 packet from host: unable to allocate storage to handle "
        "asynchronously!");
    return;
  }
  impl_.SendH4PacketFromHost(std::move(owned_h4_packet_result.value()));
}

void ProxyHost::HandleH4HciFromController(H4PacketWithHci&& h4_packet) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    DoHandleH4HciFromController(std::move(h4_packet));
    return;
  }
  // TODO(b/411168474): See the corresponding note in `HandleH4HciFromHost`.
  // As with that method, if a `ReleaseFn` is not set, copy its data to an
  // allocated packet with a `ReleaseFn`.
  if (h4_packet.HasReleaseFn()) {
    impl_.SendH4PacketFromController(std::move(h4_packet));
    return;
  }
  Allocator& allocator = l2cap_channel_manager_.impl().allocator();
  Result<H4PacketWithHci> owned_h4_packet_result =
      H4PacketWithHci::CopyFrom(allocator, h4_packet);
  if (!owned_h4_packet_result.ok()) {
    PW_LOG_ERROR(
        "Dropping H4 packet from controller: unable to allocate storage to "
        "handle asynchronously!");
    return;
  }
  impl_.SendH4PacketFromController(std::move(owned_h4_packet_result.value()));
}

void ProxyHost::Reset() {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    DoReset();
  } else {
    BasicRequest request{
        .type = BasicRequest::Type::kReset,
    };
    impl_.BasicSendAndReceive(std::move(request));
  }
}

Result<L2capCoc> ProxyHost::AcquireL2capCoc(
    multibuf::MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    L2capCoc::CocConfig rx_config,
    L2capCoc::CocConfig tx_config,
    Function<void(multibuf::MultiBuf&& payload)>&& receive_fn,
    ChannelEventCallback&& event_fn) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoAcquireL2capCoc(rx_multibuf_allocator,
                             connection_handle,
                             rx_config,
                             tx_config,
                             std::move(receive_fn),
                             std::move(event_fn));
  }
  ChannelRequest request{
      .params =
          ChannelRequest::L2capCocParams{
              .rx_multibuf_allocator = &rx_multibuf_allocator,
              .connection_handle = connection_handle,
              .rx_config = rx_config,
              .tx_config = tx_config,
              .receive_fn = std::move(receive_fn),
              .event_fn = std::move(event_fn),
          },
  };
  PW_TRY_ASSIGN(auto channel, impl_.ChannelSendAndReceive(std::move(request)));
  return Result<L2capCoc>(std::move(std::get<L2capCoc>(channel)));
}

Result<BasicL2capChannel> ProxyHost::AcquireBasicL2capChannel(
    multibuf::MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid,
    AclTransportType transport,
    OptionalPayloadReceiveCallback&& payload_from_controller_fn,
    OptionalPayloadReceiveCallback&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoAcquireBasicL2capChannel(rx_multibuf_allocator,
                                      connection_handle,
                                      local_cid,
                                      remote_cid,
                                      transport,
                                      std::move(payload_from_controller_fn),
                                      std::move(payload_from_host_fn),
                                      std::move(event_fn));
  }
  ChannelRequest request{
      .params =
          ChannelRequest::BasicL2capParams{
              .rx_multibuf_allocator = &rx_multibuf_allocator,
              .connection_handle = connection_handle,
              .local_cid = local_cid,
              .remote_cid = remote_cid,
              .transport = transport,
              .payload_from_controller_fn =
                  std::move(payload_from_controller_fn),
              .payload_from_host_fn = std::move(payload_from_host_fn),
              .event_fn = std::move(event_fn),
          },
  };
  PW_TRY_ASSIGN(auto channel, impl_.ChannelSendAndReceive(std::move(request)));
  return Result<BasicL2capChannel>(
      std::move(std::get<BasicL2capChannel>(channel)));
}

Result<UniquePtr<ChannelProxy>> ProxyHost::DoInterceptBasicModeChannel(
    ConnectionHandle connection_handle,
    uint16_t local_channel_id,
    uint16_t remote_channel_id,
    AclTransportType transport,
    BufferReceiveFunction&& payload_from_controller_fn,
    BufferReceiveFunction&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return InternalDoInterceptBasicModeChannel(
        connection_handle,
        local_channel_id,
        remote_channel_id,
        transport,
        std::move(payload_from_controller_fn),
        std::move(payload_from_host_fn),
        std::move(event_fn));
  }
  ChannelRequest request{
      .params =
          ChannelRequest::BasicChannelProxyParams{
              .connection_handle = connection_handle,
              .local_channel_id = local_channel_id,
              .remote_channel_id = remote_channel_id,
              .transport = transport,
              .payload_from_controller_fn =
                  std::move(payload_from_controller_fn),
              .payload_from_host_fn = std::move(payload_from_host_fn),
              .event_fn = std::move(event_fn)},
  };
  PW_TRY_ASSIGN(auto channel, impl_.ChannelSendAndReceive(std::move(request)));
  return Result<UniquePtr<ChannelProxy>>(
      std::move(std::get<UniquePtr<ChannelProxy>>(channel)));
}

Result<GattNotifyChannel> ProxyHost::AcquireGattNotifyChannel(
    int16_t connection_handle,
    uint16_t attribute_handle,
    ChannelEventCallback&& event_fn) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoAcquireGattNotifyChannel(
        connection_handle, attribute_handle, std::move(event_fn));
  }
  ChannelRequest request{
      .params =
          ChannelRequest::GattNotifyParams{
              .connection_handle = connection_handle,
              .attribute_handle = attribute_handle,
              .event_fn = std::move(event_fn),
          },
  };
  PW_TRY_ASSIGN(auto channel, impl_.ChannelSendAndReceive(std::move(request)));
  return Result<GattNotifyChannel>(
      std::move(std::get<GattNotifyChannel>(channel)));
}

bool ProxyHost::HasSendLeAclCapability() const {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoHasSendLeAclCapability();
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kHasSendLeAclCapability;
  return impl_.BasicSendAndReceive(std::move(request)) != 0;
}

bool ProxyHost::HasSendBrEdrAclCapability() const {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoHasSendBrEdrAclCapability();
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kHasSendBrEdrAclCapability;
  return impl_.BasicSendAndReceive(std::move(request)) != 0;
}

uint16_t ProxyHost::GetNumFreeLeAclPackets() const {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoGetNumFreeLeAclPackets();
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kGetNumFreeLeAclPackets;
  return impl_.BasicSendAndReceive(std::move(request));
}

uint16_t ProxyHost::GetNumFreeBrEdrAclPackets() const {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    return DoGetNumFreeBrEdrAclPackets();
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kGetNumFreeBrEdrAclPackets;
  return impl_.BasicSendAndReceive(std::move(request));
}

void ProxyHost::RegisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    DoRegisterL2capStatusDelegate(delegate);
    return;
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kRegisterL2capStatusDelegate;
  request.delegate = &delegate;
  impl_.BasicSendAndReceive(std::move(request));
}

void ProxyHost::UnregisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  if (l2cap_channel_manager_.impl().IsRunningOnDispatcherThread()) {
    DoUnregisterL2capStatusDelegate(delegate);
    return;
  }
  BasicRequest request;
  request.type = BasicRequest::Type::kUnregisterL2capStatusDelegate;
  request.delegate = &delegate;
  impl_.BasicSendAndReceive(std::move(request));
}

namespace internal {

ProxyHostImpl::ProxyHostImpl(ProxyHost& proxy)
    : proxy_(proxy),
      host_packet_task_(PW_ASYNC_TASK_NAME("ProxyHost:FromHost"), *this),
      controller_packet_task_(PW_ASYNC_TASK_NAME("ProxyHost:FromController"),
                              *this),
      basic_task_(PW_ASYNC_TASK_NAME("ProxyHost:Basic"), *this),
      channel_task_(PW_ASYNC_TASK_NAME("ProxyHost:Channel"), *this) {
  [[maybe_unused]] auto [host_packet_handle,
                         host_packet_sender,
                         host_packet_receiver] =
      async2::CreateSpscChannel(host_packet_storage_);
  host_packet_sender_ = std::move(host_packet_sender);
  host_packet_task_.set_receiver(std::move(host_packet_receiver));

  [[maybe_unused]] auto [controller_packet_handle,
                         controller_packet_sender,
                         controller_packet_receiver] =
      async2::CreateSpscChannel(controller_packet_storage_);
  controller_packet_sender_ = std::move(controller_packet_sender);
  controller_packet_task_.set_receiver(std::move(controller_packet_receiver));

  [[maybe_unused]] auto [basic_request_handle,
                         basic_request_sender,
                         basic_request_receiver] =
      async2::CreateSpscChannel(basic_request_storage_);
  [[maybe_unused]] auto [basic_response_handle,
                         basic_response_sender,
                         basic_response_receiver] =
      async2::CreateSpscChannel(basic_response_storage_);
  basic_request_sender_ = std::move(basic_request_sender);
  basic_task_.set_receiver(std::move(basic_request_receiver));
  basic_task_.set_sender(std::move(basic_response_sender));
  basic_response_receiver_ = std::move(basic_response_receiver);

  [[maybe_unused]] auto [channel_request_handle,
                         channel_request_sender,
                         channel_request_receiver] =
      async2::CreateSpscChannel(channel_request_storage_);
  [[maybe_unused]] auto [channel_response_handle,
                         channel_response_sender,
                         channel_response_receiver] =
      async2::CreateSpscChannel(channel_response_storage_);
  channel_request_sender_ = std::move(channel_request_sender);
  channel_task_.set_receiver(std::move(channel_request_receiver));
  channel_task_.set_sender(std::move(channel_response_sender));
  channel_response_receiver_ = std::move(channel_response_receiver);
}

async2::Dispatcher& ProxyHostImpl::dispatcher() const {
  return proxy_.l2cap_channel_manager_.impl().dispatcher();
}

void ProxyHostImpl::Start() {
  dispatcher().Post(host_packet_task_);
  dispatcher().Post(controller_packet_task_);
  dispatcher().Post(basic_task_);
  dispatcher().Post(channel_task_);
}

void ProxyHostImpl::SendH4PacketFromHost(H4PacketWithH4&& h4_packet) const {
  auto reservation = host_packet_sender_.TryReserveSend();
  if (reservation.ok()) {
    reservation->Commit(std::move(h4_packet));
  } else {
    host_packet_sender_.BlockingSend(dispatcher(), std::move(h4_packet))
        .IgnoreError();
  }
}

void ProxyHostImpl::SendH4PacketFromController(
    H4PacketWithHci&& h4_packet) const {
  auto reservation = controller_packet_sender_.TryReserveSend();
  if (reservation.ok()) {
    reservation->Commit(std::move(h4_packet));
  } else {
    controller_packet_sender_.BlockingSend(dispatcher(), std::move(h4_packet))
        .IgnoreError();
  }
}

uint16_t ProxyHostImpl::BasicSendAndReceive(BasicRequest&& request) const {
  // Lock guard is necessary to prevent multiple client threads from racing and
  // getting each other's response on the shared channel.
  std::lock_guard lock(basic_send_receive_mutex_);
  if (!basic_request_sender_.BlockingSend(dispatcher(), std::move(request))
           .ok()) {
    return 0;
  }

  auto response = basic_response_receiver_.BlockingReceive(dispatcher());
  if (!response.ok()) {
    return 0;
  }
  return *response;
}

void ProxyHostImpl::DoHandleRequest(H4PacketWithH4&& h4_packet) {
  proxy_.DoHandleH4HciFromHost(std::move(h4_packet));
}

void ProxyHostImpl::DoHandleRequest(H4PacketWithHci&& h4_packet) {
  proxy_.DoHandleH4HciFromController(std::move(h4_packet));
}

uint16_t ProxyHostImpl::DoHandleRequest(BasicRequest&& request) {
  switch (request.type) {
    case BasicRequest::Type::kReset:
      proxy_.DoReset();
      break;

    case BasicRequest::Type::kRegisterL2capStatusDelegate:
      proxy_.DoRegisterL2capStatusDelegate(*request.delegate);
      break;

    case BasicRequest::Type::kUnregisterL2capStatusDelegate:
      proxy_.DoUnregisterL2capStatusDelegate(*request.delegate);
      break;

    case BasicRequest::Type::kHasSendLeAclCapability:
      return proxy_.DoHasSendLeAclCapability();

    case BasicRequest::Type::kHasSendBrEdrAclCapability:
      return proxy_.DoHasSendBrEdrAclCapability();

    case BasicRequest::Type::kGetNumFreeLeAclPackets:
      return proxy_.DoGetNumFreeLeAclPackets();

    case BasicRequest::Type::kGetNumFreeBrEdrAclPackets:
      return proxy_.DoGetNumFreeBrEdrAclPackets();
  }
  return 0;
}

Result<ProxyHostImpl::ClientChannel> ProxyHostImpl::ChannelSendAndReceive(
    ChannelRequest&& request) const {
  // Lock guard is necessary to prevent multiple client threads from racing and
  // getting each other's response on the shared channel.
  std::lock_guard lock(channel_send_receive_mutex_);
  PW_TRY(
      channel_request_sender_.BlockingSend(dispatcher(), std::move(request)));
  auto response = channel_response_receiver_.BlockingReceive(dispatcher());
  if (!response.ok()) {
    return response.status();
  }
  return Result<ClientChannel>(std::move(*response));
}

namespace {

template <typename... Request>
struct ChannelRequestVisitor : Request... {
  using Request::operator()...;
};

template <typename... Request>
ChannelRequestVisitor(Request...) -> ChannelRequestVisitor<Request...>;

}  // namespace

Result<ProxyHostImpl::ClientChannel> ProxyHostImpl::DoHandleRequest(
    ChannelRequest&& request) {
  return std::visit(
      ChannelRequestVisitor{
          [this](ChannelRequest::BasicL2capParams&& params)
              -> Result<ProxyHostImpl::ClientChannel> {
            PW_TRY_ASSIGN(ClientChannel channel,
                          proxy_.DoAcquireBasicL2capChannel(
                              *params.rx_multibuf_allocator,
                              params.connection_handle,
                              params.local_cid,
                              params.remote_cid,
                              params.transport,
                              std::move(params.payload_from_controller_fn),
                              std::move(params.payload_from_host_fn),
                              std::move(params.event_fn)));
            return Result<ClientChannel>(std::move(channel));
          },
          [this](ChannelRequest::GattNotifyParams&& params)
              -> Result<ProxyHostImpl::ClientChannel> {
            PW_TRY_ASSIGN(
                ClientChannel channel,
                proxy_.DoAcquireGattNotifyChannel(params.connection_handle,
                                                  params.attribute_handle,
                                                  std::move(params.event_fn)));
            return Result<ClientChannel>(std::move(channel));
          },
          [this](ChannelRequest::L2capCocParams&& params)
              -> Result<ProxyHostImpl::ClientChannel> {
            PW_TRY_ASSIGN(
                ClientChannel channel,
                proxy_.DoAcquireL2capCoc(*params.rx_multibuf_allocator,
                                         params.connection_handle,
                                         params.rx_config,
                                         params.tx_config,
                                         std::move(params.receive_fn),
                                         std::move(params.event_fn)));
            return Result<ClientChannel>(std::move(channel));
          },
          [this](ChannelRequest::BasicChannelProxyParams&& params)
              -> Result<ProxyHostImpl::ClientChannel> {
            PW_TRY_ASSIGN(ClientChannel channel,
                          proxy_.InternalDoInterceptBasicModeChannel(
                              params.connection_handle,
                              params.local_channel_id,
                              params.remote_channel_id,
                              params.transport,
                              std::move(params.payload_from_controller_fn),
                              std::move(params.payload_from_host_fn),
                              std::move(params.event_fn)));
            return Result<ClientChannel>(std::move(channel));
          }},
      std::move(request.params));
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC != 0
