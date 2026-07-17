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
#include <lib/fit/function.h>

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/signaling_channel.h"

namespace bt::l2cap::internal {

// Wrapper for a signaling channel that sends and receives command
// transactions. It does not hold state. Rather, it:
// - constructs outbound request payloads and decodes/dispatches the received
//   response payloads
// - constructs request handlers that decode inbound payloads, registers the
//   handlers with SignalingChannel, and creates Responder objects that bind
//   response parameters and can be used to send appropriate response commands
// CommandHandler can be constructed for each command to be sent or each
// kind of request to register, and even ephemerally as a temporary around a
// SignalingChannel.
//
// For outbound requests, use the CommandHandler::Send*Request methods. They
// take parameters to be encoded into the request payload (with endian
// conversion and bounds checking) and a *ResponseCallback callback. When a
// matching response or rejection is received, the callback will be passed a
// *Response object containing the decoded command's parameters. Its |status()|
// shall be checked first to determine whether it's a rejection or response
// command. Return ResponseHandlerAction::kExpectAdditionalResponse if more
// request responses from the peer will follow, or else
// ResponseHandlerAction::kCompleteOutboundTransaction. Returning
// kCompleteOutboundTransaction will destroy the *ResponseCallback object.
//
// If the underlying SignalingChannel times out waiting for a response or a
// malformed/undecodable response or reject is received, the *ResponseCallback
// will not be called. Instead, the |request_fail_callback| that CommandHandler
// was constructed with will be called.
//
// Example:
//   DisconnectionResponseCallback rsp_cb =
//       [](const DisconnectionResponse& rsp) {
//         if (rsp.status() == Status::kReject) {
//           // Do something with rsp.reject_reason()
//         } else {
//           // Do something with rsp.local_cid() and rsp.remote_cid()
//         }
//       };
//   cmd_handler.SendDisonnectionRequest(remote_cid, local_cid,
//   std::move(rsp_cb));
//
// For inbound requests, use the CommandHandler::Serve*Req methods. They
// each take a request-handling delegate that will be called with decoded
// parameters from the received request, as well as a *Responder object. The
// Responder can be used to send a rejection (|RejectNotUnderstood()| or
// |RejectInvalidChannelId()|) or a matching response (|Send*()|). The channel
// IDs to encode into the response will be bound to the Responder. The Responder
// is only valid during the invocation of the request handler. Its sending
// methods can be called multiple times but it does not check that a malformed
// permutation of commands are sent (e.g. multiple rejections, a rejection
// followed by a response, etc.).
//
// Example:
//  DisconnectionRequestCallback req_cb =
//      [](ChannelId local_cid, ChannelId remote_cid,
//         DisconnectionResponder* responder) {
//        // Do something with local_cid and remote_cid
//        responder->Send();  // Request's IDs already bound to responder, omit
//        // OR:
//        responder->RejectInvalidChannelId();  // Idem.
//      };
//  cmd_handler.ServeDisconnectionRequest(std::move(req_cb));
//
// For both inbound requests and responses, if the received payload data is
// insufficient in size or otherwise malformed, it will be replied to with a
// Reject Not Understood and the corresponding callback will not be invoked.
class CommandHandler {
 public:
  using Status = SignalingChannel::Status;
  using ResponseHandlerAction =
      SignalingChannelInterface::ResponseHandlerAction;

  // Base for all responses received, including Command Reject. If |status()|
  // evaluates as |Status::kReject|, then this holds a Command Reject; then
  // |reject_reason()| should be read and the data in the derived Response
  // object should not be accessed.
  class Response {
   public:
    explicit Response(Status status) : status_(status) {}

    Status status() const { return status_; }

    // These are valid for reading if the response format contains them;
    // otherwise, they read as kInvalidChannelId.
    ChannelId local_cid() const { return local_cid_; }
    ChannelId remote_cid() const { return remote_cid_; }

    // This is valid for reading if |status| is kReject. If its value is
    // kInvalidCID, then |local_cid| and |remote_cid| are valid for reading.
    RejectReason reject_reason() const { return reject_reason_; }

   protected:
    friend class CommandHandler;

    // Fills the reject fields of |rsp|. Returns true if successful.
    bool ParseReject(const ByteBuffer& rej_payload_buf);

    Status status_;
    ChannelId local_cid_ = kInvalidChannelId;
    ChannelId remote_cid_ = kInvalidChannelId;
    RejectReason reject_reason_;
  };

  class DisconnectionResponse final : public Response {
   public:
    using PayloadT = DisconnectionResponsePayload;
    static constexpr const char* kName = "Disconnection Response";

    using Response::Response;  // Inherit ctor
    bool Decode(const ByteBuffer& payload_buf);
  };

  // Base of response-sending objects passed to request delegates that they can
  // use to reply with a corresponding response or a rejection. This base
  // includes rejection methods because they can always be sent (but are not
  // always a reasonable reply to a given request). This also binds channel IDs
  // from the request received and uses them for the outbound response payload,
  // so that the delegate can not omit or send incorrect channel IDs.
  class Responder {
   public:
    void RejectNotUnderstood();
    void RejectInvalidChannelId();

   protected:
    explicit Responder(SignalingChannel::Responder* sig_responder,
                       ChannelId local_cid = kInvalidChannelId,
                       ChannelId remote_cid = kInvalidChannelId);
    virtual ~Responder() = default;
    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Responder);

    ChannelId local_cid() const { return local_cid_; }
    ChannelId remote_cid() const { return remote_cid_; }

    SignalingChannel::Responder* const sig_responder_;

   private:
    ChannelId local_cid_;
    ChannelId remote_cid_;
  };

  class DisconnectionResponder final : public Responder {
   public:
    DisconnectionResponder(SignalingChannel::Responder* sig_responder,
                           ChannelId local_cid,
                           ChannelId remote_cid);

    void Send();
  };

  // Disconnection Responses never have additional responses.
  using DisconnectionResponseCallback =
      fit::function<void(const DisconnectionResponse& rsp)>;
  bool SendDisconnectionRequest(ChannelId remote_cid,
                                ChannelId local_cid,
                                DisconnectionResponseCallback cb);

  using DisconnectionRequestCallback =
      fit::function<void(ChannelId local_cid,
                         ChannelId remote_cid,
                         DisconnectionResponder* responder)>;
  void ServeDisconnectionRequest(DisconnectionRequestCallback callback);

  // L2CAP_FLOW_CONTROL_CREDIT_IND commands do not receive a response.
  bool SendCredits(ChannelId local_cid, uint16_t credits);

  using FlowControlCreditIndCallback =
      fit::function<void(ChannelId remote_cid, uint16_t credits)>;
  void ServeFlowControlCreditInd(FlowControlCreditIndCallback callback);

  // |sig| must be valid for the lifetime of this object.
  // |command_failed_callback| is called if an outbound request timed out
  // with RTX or ERTX timers after retransmission (if configured) or if a
  // malformed/undecodable response or reject is received. The call may come
  // after the lifetime of this object.
  explicit CommandHandler(SignalingChannelInterface* sig,
                          fit::closure request_fail_callback = nullptr);
  virtual ~CommandHandler() = default;

 protected:
  // Returns a function that decodes a response status and payload into a
  // |ResponseT| object and invokes |rsp_cb| with it. |ResponseT| needs to have
  //  - |Decode| function that accepts a buffer of at least
  //  |sizeof(ResponseT::PayloadT)| bytes. If
  //    it returns false, then decoding failed, no additional responses are
  //    expected, and the user response handler will not be called.
  //  - |kName| string literal
  //
  // TODO(fxbug.dev/42111549): Name the return type of CallbackT to make parsing
  // code more readable.
  template <class ResponseT, typename CallbackT>
  SignalingChannel::ResponseHandler BuildResponseHandler(
      CallbackT response_cb) {
    return [rsp_cb = std::move(response_cb),
            fail_cb = request_fail_callback_.share()](
               Status status, const ByteBuffer& rsp_payload) {
      if (status == Status::kTimeOut) {
        bt_log(INFO,
               "l2cap",
               "cmd: timed out waiting for \"%s\"",
               ResponseT::kName);
        if (fail_cb) {
          fail_cb();
        }
        return ResponseHandlerAction::kCompleteOutboundTransaction;
      }

      ResponseT rsp(status);
      if (status == Status::kReject) {
        if (!rsp.ParseReject(rsp_payload)) {
          bt_log(DEBUG,
                 "l2cap",
                 "cmd: rejecting malformed Command Reject, size %zu; failing "
                 "request",
                 rsp_payload.size());
          if (fail_cb) {
            fail_cb();
          }
          return ResponseHandlerAction::kCompleteOutboundTransaction;
        }
        return InvokeResponseCallback(&rsp_cb, std::move(rsp));
      }

      if (rsp_payload.size() < sizeof(typename ResponseT::PayloadT)) {
        bt_log(DEBUG,
               "l2cap",
               "cmd: rejecting malformed \"%s\", size %zu (expected %zu); "
               "failing request",
               ResponseT::kName,
               rsp_payload.size(),
               sizeof(typename ResponseT::PayloadT));
        if (fail_cb) {
          fail_cb();
        }
        return ResponseHandlerAction::kCompleteOutboundTransaction;
      }

      if (!rsp.Decode(rsp_payload)) {
        bt_log(DEBUG,
               "l2cap",
               "cmd: rejecting malformed \"%s\", could not decode; failing "
               "request",
               ResponseT::kName);
        if (fail_cb) {
          fail_cb();
        }
        return ResponseHandlerAction::kCompleteOutboundTransaction;
      }

      return InvokeResponseCallback(&rsp_cb, std::move(rsp));
    };
  }

  // Invokes |rsp_cb| with |rsp|. Returns
  // ResponseHandlerAction::kCompleteOutboundTransaction for "no additional
  // responses expected" if |rsp_cb| returns void, otherwise passes along its
  // return result. Used because not all *ResponseCallback types return void
  // (some can request additional continuations in their return value).
  template <typename CallbackT, class ResponseT>
  static CommandHandler::ResponseHandlerAction InvokeResponseCallback(
      CallbackT* const rsp_cb, ResponseT rsp) {
    if constexpr (std::is_void_v<std::invoke_result_t<CallbackT, ResponseT>>) {
      (*rsp_cb)(rsp);
      return ResponseHandlerAction::kCompleteOutboundTransaction;
    } else {
      return (*rsp_cb)(rsp);
    }
  }

  SignalingChannelInterface* sig() const { return sig_; }

 private:
  SignalingChannelInterface* const sig_;  // weak
  fit::closure request_fail_callback_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(CommandHandler);
};

}  // namespace bt::l2cap::internal
