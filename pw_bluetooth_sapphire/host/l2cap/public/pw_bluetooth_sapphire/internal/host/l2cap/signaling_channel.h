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
#include <memory>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/packet_view.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/scoped_channel.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bt::l2cap {

class Channel;

namespace internal {

using SignalingPacket = PacketView<CommandHeader>;
using MutableSignalingPacket = MutablePacketView<CommandHeader>;

using DataCallback = fit::function<void(const ByteBuffer& data)>;
using SignalingPacketHandler =
    fit::function<void(const SignalingPacket& packet)>;

// SignalingChannelInterface contains the procedures that command flows use to
// send and receive signaling channel transactions.
class SignalingChannelInterface {
 public:
  // Action in response to a request-type packet.
  enum class Status {
    kSuccess,  // Remote response received
    kReject,   // Remote rejection received
    kTimeOut,  // Timed out waiting for matching remote command
  };

  // ResponseHandler return value. Indicates whether additional responses are
  // expected in this transaction (e.g. in the case of receiving a response with
  // a pending status or continuation flag).
  enum class ResponseHandlerAction {
    kExpectAdditionalResponse,
    // No additional responses expected in this transaction.
    kCompleteOutboundTransaction,
  };

  // Callback invoked to handle a response received from the remote. If |status|
  // is kSuccess or kReject, then |rsp_payload| will contain any payload
  // received. This callback is allowed to destroy the SignalingChannel, but
  // must return kCompleteOutboundTransaction if it does.
  using ResponseHandler = fit::function<ResponseHandlerAction(
      Status, const ByteBuffer& rsp_payload)>;

  // Initiate an outbound transaction. The signaling channel will send a request
  // then expect reception of one or more responses with a code one greater than
  // the request. Each response or rejection received invokes |cb|. When |cb|
  // returns false, it will be removed. Returns false if the request failed to
  // send.
  virtual bool SendRequest(CommandCode req_code,
                           const ByteBuffer& payload,
                           ResponseHandler cb) = 0;

  // Send a command packet in response to an incoming request.
  class Responder {
   public:
    // Send a response that corresponds to the request received
    virtual void Send(const ByteBuffer& rsp_payload) = 0;

    // Reject invalid, malformed, or unhandled request
    virtual void RejectNotUnderstood() = 0;

    // Reject request non-existent or otherwise invalid channel ID(s)
    virtual void RejectInvalidChannelId(ChannelId local_cid,
                                        ChannelId remote_cid) = 0;

   protected:
    virtual ~Responder() = default;
  };

  // Callback invoked to handle a request received from the remote.
  // |req_payload| contains any payload received, without the command header.
  // The callee can use |responder| to respond or reject. Parameters passed to
  // this handler are only guaranteed to be valid while the handler is running.
  using RequestDelegate =
      fit::function<void(const ByteBuffer& req_payload, Responder* responder)>;

  // Register a handler for all inbound transactions matching |req_code|, which
  // should be the code of a request. |cb| will be called with request payloads
  // received, and is expected to respond to, reject, or ignore the requests.
  // Calls to this function with a previously registered |req_code| will replace
  // the current delegate.
  virtual void ServeRequest(CommandCode req_code, RequestDelegate cb) = 0;

  // Send a command that expects no response.
  virtual bool SendCommandWithoutResponse(CommandCode req_code,
                                          const ByteBuffer& payload) = 0;

 protected:
  virtual ~SignalingChannelInterface() = default;
};

// SignalingChannel is an abstract class that handles the common operations
// involved in LE and BR/EDR signaling channels.
//
// TODO(armansito): Implement flow control (RTX/ERTX timers).
class SignalingChannel : public SignalingChannelInterface {
 public:
  SignalingChannel(Channel::WeakPtr chan,
                   pw::bluetooth::emboss::ConnectionRole role,
                   pw::async::Dispatcher& dispatcher,
                   pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);
  ~SignalingChannel() override = default;

  // SignalingChannelInterface overrides
  bool SendRequest(CommandCode req_code,
                   const ByteBuffer& payload,
                   ResponseHandler cb) override;
  void ServeRequest(CommandCode req_code, RequestDelegate cb) override;
  bool SendCommandWithoutResponse(CommandCode req_code,
                                  const ByteBuffer& payload) override;

  bool is_open() const { return is_open_; }

  // Local signaling MTU (i.e. MTU_sig, per spec)
  uint16_t mtu() const { return mtu_; }
  void set_mtu(uint16_t mtu) { mtu_ = mtu; }

 protected:
  // Implementation for responding to a request that binds the request's
  // identifier and the response's code so that the client's |Send| invocation
  // does not need to supply them nor even know them.
  class ResponderImpl : public Responder {
   public:
    ResponderImpl(SignalingChannel* sig, CommandCode code, CommandId id);
    void Send(const ByteBuffer& rsp_payload) override;
    void RejectNotUnderstood() override;
    void RejectInvalidChannelId(ChannelId local_cid,
                                ChannelId remote_cid) override;

   private:
    SignalingChannel* sig() const { return sig_; }

    SignalingChannel* const sig_;
    const CommandCode code_;
    const CommandId id_;
  };

  // Sends out a single signaling packet using the given parameters.
  bool SendPacket(CommandCode code, uint8_t identifier, const ByteBuffer& data);

  // True if the code is for a supported response-type signaling command.
  virtual bool IsSupportedResponse(CommandCode code) const = 0;

  // Called when a frame is received to decode into L2CAP signaling command
  // packets. The derived implementation should invoke |cb| for each packet with
  // a valid payload length, send a Command Reject packet for each packet with
  // an intact ID in its header but invalid payload length, and drop any other
  // incoming data.
  virtual void DecodeRxUnit(ByteBufferPtr sdu,
                            const SignalingPacketHandler& cb) = 0;

  // Called when a new signaling packet has been received. Returns false if
  // |packet| is rejected. Otherwise returns true and sends a response packet.
  //
  // This method is thread-safe in that a SignalingChannel cannot be deleted
  // while this is running. SendPacket() can be called safely from this method.
  // TODO(fxbug.dev/42056068): make non-virtual & private after removing le
  // signaling channel override
  virtual bool HandlePacket(const SignalingPacket& packet);

  // Sends out a command reject packet with the given parameters.
  bool SendCommandReject(uint8_t identifier,
                         RejectReason reason,
                         const ByteBuffer& data);

  // Returns true if called on this SignalingChannel's creation thread. Mainly
  // intended for debug assertions.

  // Returns the logical link that signaling channel is operating on.
  pw::bluetooth::emboss::ConnectionRole role() const { return role_; }

  // Generates a command identifier in sequential order that is never
  // kInvalidId. The caller is responsible for bookkeeping when reusing command
  // IDs to prevent collisions with pending commands.
  CommandId GetNextCommandId();

  // Generate a command identifier in sequential order that is never
  // kInvalidId, and ensures that it does not correspond to a pending command.
  // If all possible command IDs are pending, returns std::nullopt.
  std::optional<CommandId> GetNextAvailableCommandId();

 private:
  // Enqueue a response to a request with command id |id| and payload
  // |request_packet|. Register a callback |cb| that will be invoked when a
  // response-type command packet (specified by |response_command_code|) is
  // received. Starts the RTX timer and handles retransmission of
  // |request_packet| and eventual timeout failure if a response isn't received.
  // If the signaling channel receives a Command Reject that matches the same
  // |id|, the rejection packet will be forwarded to the callback instead.
  void EnqueueResponse(const ByteBuffer& request_packet,
                       CommandId id,
                       CommandCode response_command_code,
                       ResponseHandler cb);

  // Called when a response-type command packet is received. Sends a Command
  // Reject if no ResponseHandler was registered for inbound packet's command
  // code and identifier.
  void OnRxResponse(const SignalingPacket& packet);

  // Called after Response Timeout eXpired (RTX) or Extended Response Timeout
  // eXpired (ERTX) timer expires. |id| must be in |pending_commands_|. If
  // |retransmit| is true, requests will be retransmitted up to the
  // retransmission limit before timing out the response. The ResponseHandler
  // will be invoked with Status::kTimeOut and an empty ByteBuffer.
  void OnResponseTimeout(CommandId id, bool retransmit);

  // True if an outbound request-type command has registered a callback for its
  // response matching a particular |id|.
  bool IsCommandPending(CommandId id) const;

  // Sends out the given signaling packet directly via |chan_| after running
  // debug-mode assertions for validity. Packet must correspond to exactly one
  // C-frame payload.
  //
  // This method is not thread-safe (i.e. requires external locking).
  //
  // TODO(armansito): This should be generalized for ACL-U to allow multiple
  // signaling commands in a single C-frame.
  bool Send(ByteBufferPtr packet);

  // Builds a signaling packet with the given parameters and payload. The
  // backing buffer is slab allocated.
  ByteBufferPtr BuildPacket(CommandCode code,
                            uint8_t identifier,
                            const ByteBuffer& data);
  // Channel callbacks:
  void OnChannelClosed();
  void OnRxBFrame(ByteBufferPtr sdu);

  // Invoke the abstract packet handler |HandlePacket| for well-formed command
  // packets and send responses for command packets that exceed this host's MTU
  // or can't be handled by this host.
  void CheckAndDispatchPacket(const SignalingPacket& packet);

  // Stores copy of request, response handlers, and timeout state for requests
  // that have been sent.
  struct PendingCommand {
    PendingCommand(const ByteBuffer& request_packet,
                   CommandCode response_command_code,
                   ResponseHandler response_handler_cb,
                   pw::async::Dispatcher& dispatcher,
                   pw::bluetooth_sapphire::Lease wake_lease_in)
        : response_code(response_command_code),
          response_handler(std::move(response_handler_cb)),
          command_packet(std::make_unique<DynamicByteBuffer>(request_packet)),
          transmit_count(1u),
          timer_duration(0u),
          response_timeout_task(dispatcher),
          wake_lease(std::move(wake_lease_in)),
          enqueue_time(dispatcher.now()) {}
    CommandCode response_code;
    ResponseHandler response_handler;

    // Copy of request command packet. Used for retransmissions.
    ByteBufferPtr command_packet;

    // Number of times this request has been transmitted.
    size_t transmit_count;

    // The current timer duration. Used to perform exponential backoff with the
    // RTX timer.
    pw::chrono::SystemClock::duration timer_duration;

    // Automatically canceled by destruction if the response is received.
    SmartTask response_timeout_task;

    // Keep the system awake while this command is pending.
    pw::bluetooth_sapphire::Lease wake_lease;

    // The time when the command was enqueued.
    pw::chrono::SystemClock::time_point enqueue_time;
  };

  // Retransmit the request corresponding to |pending_command| and reset the RTX
  // timer.
  void RetransmitPendingCommand(PendingCommand& pending_command);

  pw::async::Dispatcher& pw_dispatcher_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  bool is_open_;
  l2cap::ScopedChannel chan_;
  pw::bluetooth::emboss::ConnectionRole role_;
  uint16_t mtu_;
  uint8_t next_cmd_id_;

  // Stores response handlers for outbound request packets with the
  // corresponding CommandId.
  std::unordered_map<CommandId, PendingCommand> pending_commands_;

  // Stores handlers for incoming request packets.
  std::unordered_map<CommandCode, RequestDelegate> inbound_handlers_;

  WeakSelf<SignalingChannel> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SignalingChannel);
};

}  // namespace internal
}  // namespace bt::l2cap
