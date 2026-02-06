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
#pragma once

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include <cstdint>
#include <variant>

#include "pw_async2/channel.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_bluetooth_proxy/basic_l2cap_channel.h"
#include "pw_bluetooth_proxy/gatt_notify_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/l2cap_coc.h"
#include "pw_bluetooth_proxy/l2cap_coc_config.h"
#include "pw_bluetooth_proxy/l2cap_status_delegate.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

class ProxyHost;

using BufferReceiveFunction =
    L2capChannelManagerInterface::BufferReceiveFunction;

namespace internal {

/// Implementation detail class for ProxyHost.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is true; that is, when
/// the proxy is using an asynchronous execution model with a task dispatcher
/// running one exactly one thread.
class ProxyHostImpl {
 public:
  /// Callback for forwarding payloads to an L2capCoc client.
  using PayloadReceiveCallback = Function<void(FlatConstMultiBuf&& payload)>;

  /// Callback for forwarding payloads to a generic L2CAP client.
  using ReceiveFn = std::variant<std::monostate,
                                 OptionalPayloadReceiveCallback,
                                 PayloadReceiveCallback>;

  /// Request to perform a basic API methods.
  ///
  /// When an API method is called from a thread other than the dispatcher
  /// thread, a request is created and sent over a channel to a task on the
  /// dispatcher that can handle it and respond. This allows all processing to
  /// happen on the dispatcher thread without needing synchronization.
  ///
  /// "Basic" methods are those that do not involve acquiring a client channel.
  struct BasicRequest {
    enum class Type : uint16_t {
      kReset,
      kRegisterL2capStatusDelegate,
      kUnregisterL2capStatusDelegate,
      kHasSendLeAclCapability,
      kHasSendBrEdrAclCapability,
      kGetNumFreeLeAclPackets,
      kGetNumFreeBrEdrAclPackets,
    } type = Type::kReset;

    L2capStatusDelegate* delegate = nullptr;
  };

  /// Request to perform a channel API methods.
  ///
  /// When an API method is called from a thread other than the dispatcher
  /// thread, a request is created and sent over a channel to a task on the
  /// dispatcher that can handle it and respond. This allows all processing to
  /// happen on the dispatcher thread without needing synchronization.
  ///
  /// "Channel" methods are those that involve acquiring a client channel.
  struct ChannelRequest {
    struct BasicL2capParams {
      MultiBufAllocator* rx_multibuf_allocator = nullptr;
      uint16_t connection_handle = 0;
      uint16_t local_cid = 0;
      uint16_t remote_cid = 0;
      AclTransportType transport = AclTransportType::kBrEdr;
      OptionalPayloadReceiveCallback payload_from_controller_fn;
      OptionalPayloadReceiveCallback payload_from_host_fn;
      ChannelEventCallback event_fn;
    };

    struct BasicChannelProxyParams {
      ConnectionHandle connection_handle = ConnectionHandle{0};
      uint16_t local_channel_id = 0;
      uint16_t remote_channel_id = 0;
      AclTransportType transport = AclTransportType::kBrEdr;
      BufferReceiveFunction payload_from_controller_fn;
      BufferReceiveFunction payload_from_host_fn;
      ChannelEventCallback event_fn;
    };

    struct GattNotifyParams {
      int16_t connection_handle = 0;
      uint16_t attribute_handle = 0;
      ChannelEventCallback event_fn;
    };

    struct L2capCocParams {
      MultiBufAllocator* rx_multibuf_allocator = nullptr;
      uint16_t connection_handle = 0;
      ConnectionOrientedChannelConfig rx_config = {0, 0, 0, 0};
      ConnectionOrientedChannelConfig tx_config = {0, 0, 0, 0};
      PayloadReceiveCallback receive_fn;
      ChannelEventCallback event_fn;
    };

    std::variant<BasicL2capParams,
                 BasicChannelProxyParams,
                 GattNotifyParams,
                 L2capCocParams>
        params;
  };

  /// Response to a channel request.
  using ClientChannel = std::variant<BasicL2capChannel,
                                     GattNotifyChannel,
                                     L2capCoc,
                                     UniquePtr<ChannelProxy>>;

  explicit ProxyHostImpl(ProxyHost& proxy);

  async2::Dispatcher& dispatcher() const;

  /// Registers the proxy host's tasks with the dispatcher.
  void Start();

 private:
  friend class pw::bluetooth::proxy::ProxyHost;

  /// Generic task that can receive messages from the API methods.
  template <typename Request>
  class ProxyTask : public async2::Task {
   public:
    constexpr ProxyTask(log::Token name, ProxyHostImpl& impl)
        : async2::Task(name), impl_(impl) {}

    void set_receiver(async2::Receiver<Request>&& receiver) {
      receiver_ = std::move(receiver);
    }

   protected:
    ProxyHostImpl& impl() { return impl_; }

    async2::Poll<std::optional<Request>> Receive(async2::Context& context);

   private:
    async2::Receiver<Request> receiver_;
    async2::ReceiveFuture<Request> request_;

    ProxyHostImpl& impl_;
  };

  /// Task that handles incoming H4 packets on the dispatcher thread.
  template <typename Request>
  class H4Handler final : public ProxyTask<Request> {
   private:
    using Base = ProxyTask<Request>;

   public:
    constexpr H4Handler(log::Token name, ProxyHostImpl& impl)
        : Base(name, impl) {}

    ~H4Handler() override { async2::Task::Deregister(); }

   private:
    /// @copydoc pw::async2::Task::Pend
    async2::Poll<> DoPend(async2::Context& context) override;
  };

  /// Task that handles and responds to basic requests and channel requests.
  template <typename Request, typename Response>
  class Responder final : public ProxyTask<Request> {
   private:
    using Base = ProxyTask<Request>;

   public:
    constexpr Responder(log::Token name, ProxyHostImpl& impl)
        : Base(name, impl) {}

    ~Responder() override { async2::Task::Deregister(); }

    void set_sender(async2::Sender<Response>&& sender) {
      sender_ = std::move(sender);
    }

   private:
    /// @copydoc pw::async2::Task::Pend
    async2::Poll<> DoPend(async2::Context& context) override;

    async2::Sender<Response> sender_;
    async2::SendFuture<Response> response_;
  };

  /// Sends an H4 packet from the host to be handled.
  void SendH4PacketFromHost(H4PacketWithH4&& h4_packet) const;

  /// Sends an H4 packet from the controller to be handled.
  void SendH4PacketFromController(H4PacketWithHci&& h4_packet) const;

  /// Sends a basic request and blocks until it receives a response.
  uint16_t BasicSendAndReceive(BasicRequest&& request) const;

  /// Sends a channel request and blocks until it receives a response.
  Result<ClientChannel> ChannelSendAndReceive(ChannelRequest&& request) const;

  /// Called by `host_packet_task_` to handle an H4 packet.
  void DoHandleRequest(H4PacketWithH4&& h4_packet);

  /// Called by `controller_packet_task_` to handle an H4 packet.
  void DoHandleRequest(H4PacketWithHci&& h4_packet);

  /// Called by `basic_task_` to handle a basic request.
  uint16_t DoHandleRequest(BasicRequest&& request);

  /// Called by `channel_task_` to handle a channel request.
  Result<ClientChannel> DoHandleRequest(ChannelRequest&& request);

  ProxyHost& proxy_;

  // Order matters: storage should be declared before, and therefore destructed
  // after, the channels that use it.

  // Channel to handle H4 packets from host.
  async2::ChannelStorage<H4PacketWithH4, 1> host_packet_storage_;
  mutable async2::Sender<H4PacketWithH4> host_packet_sender_;
  H4Handler<H4PacketWithH4> host_packet_task_;

  // Channel to handle H4 packets from controller.
  async2::ChannelStorage<H4PacketWithHci, 1> controller_packet_storage_;
  mutable async2::Sender<H4PacketWithHci> controller_packet_sender_;
  H4Handler<H4PacketWithHci> controller_packet_task_;

  // Channels to request proxy details.
  async2::ChannelStorage<BasicRequest, 1> basic_request_storage_;
  async2::ChannelStorage<uint16_t, 1> basic_response_storage_;

  mutable async2::Sender<BasicRequest> basic_request_sender_;
  Responder<BasicRequest, uint16_t> basic_task_;
  mutable async2::Receiver<uint16_t> basic_response_receiver_;

  // Channels to request L2CAP channels.
  async2::ChannelStorage<ChannelRequest, 1> channel_request_storage_;
  async2::ChannelStorage<Result<ClientChannel>, 1> channel_response_storage_;

  mutable async2::Sender<ChannelRequest> channel_request_sender_;
  Responder<ChannelRequest, Result<ClientChannel>> channel_task_;
  mutable async2::Receiver<Result<ClientChannel>> channel_response_receiver_;
};

////////////////////////////////////////////////////////////////////////////////
// Template method implementations.

template <typename Request>
async2::Poll<std::optional<Request>> ProxyHostImpl::ProxyTask<Request>::Receive(
    async2::Context& context) {
  if (!request_.is_pendable()) {
    request_ = receiver_.Receive();
  }
  std::optional<Request> request;
  PW_TRY_READY_ASSIGN(request, request_.Pend(context));
  return request;
}

template <typename Request>
async2::Poll<> ProxyHostImpl::H4Handler<Request>::DoPend(
    async2::Context& context) {
  while (true) {
    std::optional<Request> request;
    PW_TRY_READY_ASSIGN(request, Base::Receive(context));
    if (!request.has_value()) {
      return async2::Ready();
    }
    Base::impl().DoHandleRequest(std::move(*request));
  }
}

template <typename Request, typename Response>
async2::Poll<> ProxyHostImpl::Responder<Request, Response>::DoPend(
    async2::Context& context) {
  // Handle requests until the async2::Channel closes.
  while (true) {
    // First, send any outstanding response.
    bool can_send = true;
    if (response_.is_pendable()) {
      // Try to send the response. This may return Pending and cause the loop to
      // be rerun from the top when the task is later awoken.
      PW_TRY_READY_ASSIGN(can_send, response_.Pend(context));
    }
    if (!can_send) {
      // async2::Channel closed; exit the loop.
      return async2::Ready();
    }

    // Next, read the next request.
    std::optional<Request> request;
    // Try to receive a request. This may return Pending and cause the loop to
    // be rerun from the top when the task is later awoken.
    PW_TRY_READY_ASSIGN(request, Base::Receive(context));
    if (!request.has_value()) {
      // async2::Channel closed; exit the loop.
      return async2::Ready();
    }

    // Handle the request and prepare the response.
    Response response = Base::impl().DoHandleRequest(std::move(*request));
    response_ = sender_.Send(std::move(response));
  }
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
