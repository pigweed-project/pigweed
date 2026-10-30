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

#include "pw_bluetooth_sapphire/internal/host/att/bearer.h"

#include <cpp-string/string_printf.h>
#include <lib/fit/defer.h>
#include <pw_assert/check.h>
#include <pw_bytes/endian.h>
#include <pw_preprocessor/compiler.h>

#include <type_traits>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/slab_allocator.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::att {

// static

namespace {

// Returns the security level that is required to resolve the given ATT error
// code and the current security properties of the link, according to the table
// in the Core Spec v5.0, Vol 3, Part C, 10.3.2 (table 10.2). A security upgrade
// is not required if the returned value equals sm::SecurityLevel::kNoSecurity.
// TODO(armansito): Supporting requesting Secure Connections in addition to the
// inclusive-language: ignore
// encrypted/MITM dimensions.
sm::SecurityLevel CheckSecurity(ErrorCode ecode,
                                const sm::SecurityProperties& security) {
  bool encrypted = (security.level() != sm::SecurityLevel::kNoSecurity);

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (ecode) {
    // "Insufficient Encryption" error code is specified for cases when the peer
    // is paired (i.e. a LTK or STK exists for it) but the link is not
    // encrypted. We treat this as equivalent to "Insufficient Authentication"
    // sent on an unencrypted link.
    case ErrorCode::kInsufficientEncryption:
      encrypted = false;
      [[fallthrough]];
    // We achieve authorization by pairing which requires a confirmation from
    // the host's pairing delegate.
    // TODO(armansito): Allow for this to be satisfied with a simple user
    // confirmation if we're not paired?
    case ErrorCode::kInsufficientAuthorization:
    case ErrorCode::kInsufficientAuthentication:
      // If the link is already authenticated we cannot request a further
      // upgrade.
      // TODO(armansito): Take into account "secure connections" once it's
      // supported.
      if (security.authenticated()) {
        return sm::SecurityLevel::kNoSecurity;
      }
      return encrypted ? sm::SecurityLevel::kAuthenticated
                       : sm::SecurityLevel::kEncrypted;

    // Our SMP implementation always claims to support the maximum encryption
    // key size. If the key size is too small then the peer must support a
    // smaller size and we cannot upgrade the key.
    case ErrorCode::kInsufficientEncryptionKeySize:
      break;
    default:
      break;
  }
  PW_MODIFY_DIAGNOSTICS_POP();

  return sm::SecurityLevel::kNoSecurity;
}

MethodType GetMethodType(OpCode opcode) {
  // We treat all packets as a command if the command bit was set. An
  // unrecognized command will always be ignored (so it is OK to return kCommand
  // here if, for example, |opcode| is a response with the command-bit set).
  if (opcode & kCommandFlag)
    return MethodType::kCommand;

  switch (opcode) {
    case kInvalidOpCode:
      return MethodType::kInvalid;

    case kExchangeMTURequest:
    case kFindInformationRequest:
    case kFindByTypeValueRequest:
    case kReadByTypeRequest:
    case kReadRequest:
    case kReadBlobRequest:
    case kReadMultipleRequest:
    case kReadByGroupTypeRequest:
    case kWriteRequest:
    case kPrepareWriteRequest:
    case kExecuteWriteRequest:
      return MethodType::kRequest;

    case kErrorResponse:
    case kExchangeMTUResponse:
    case kFindInformationResponse:
    case kFindByTypeValueResponse:
    case kReadByTypeResponse:
    case kReadResponse:
    case kReadBlobResponse:
    case kReadMultipleResponse:
    case kReadByGroupTypeResponse:
    case kWriteResponse:
    case kPrepareWriteResponse:
    case kExecuteWriteResponse:
      return MethodType::kResponse;

    case kNotification:
      return MethodType::kNotification;
    case kIndication:
      return MethodType::kIndication;
    case kConfirmation:
      return MethodType::kConfirmation;

    // These are redundant with the check above but are included for
    // completeness.
    case kWriteCommand:
    case kSignedWriteCommand:
      return MethodType::kCommand;

    default:
      break;
  }

  // Everything else will be treated as an incoming request.
  return MethodType::kRequest;
}

// Returns the corresponding originating transaction opcode for
// |transaction_end_code|, where the latter must correspond to a response or
// confirmation.
OpCode MatchingTransactionCode(OpCode transaction_end_code) {
  switch (transaction_end_code) {
    case kExchangeMTUResponse:
      return kExchangeMTURequest;
    case kFindInformationResponse:
      return kFindInformationRequest;
    case kFindByTypeValueResponse:
      return kFindByTypeValueRequest;
    case kReadByTypeResponse:
      return kReadByTypeRequest;
    case kReadResponse:
      return kReadRequest;
    case kReadBlobResponse:
      return kReadBlobRequest;
    case kReadMultipleResponse:
      return kReadMultipleRequest;
    case kReadByGroupTypeResponse:
      return kReadByGroupTypeRequest;
    case kWriteResponse:
      return kWriteRequest;
    case kPrepareWriteResponse:
      return kPrepareWriteRequest;
    case kExecuteWriteResponse:
      return kExecuteWriteRequest;
    case kConfirmation:
      return kIndication;
    default:
      break;
  }

  return kInvalidOpCode;
}

}  // namespace

// static
std::unique_ptr<Bearer> Bearer::Create(l2cap::Channel::WeakPtr chan,
                                       pw::async::Dispatcher& dispatcher) {
  std::unique_ptr<Bearer> bearer(new Bearer(std::move(chan), dispatcher));
  return bearer->Activate() ? std::move(bearer) : nullptr;
}

Bearer::PendingTransaction::PendingTransaction(OpCode opcode_in,
                                               TransactionCallback callback_in,
                                               ByteBufferPtr pdu_in)
    : opcode(opcode_in),
      callback(std::move(callback_in)),
      pdu(std::move(pdu_in)),
      security_retry_level(sm::SecurityLevel::kNoSecurity) {
  PW_CHECK(this->callback);
  PW_CHECK(this->pdu);
}

Bearer::PendingRemoteTransaction::PendingRemoteTransaction(TransactionId id_in,
                                                           OpCode opcode_in)
    : id(id_in), opcode(opcode_in) {}

Bearer::TransactionQueue::TransactionQueue(TransactionQueue&& other)
    : queue_(std::move(other.queue_)),
      current_(std::move(other.current_)),
      security_upgrade_(std::move(other.security_upgrade_)),
      timeout_task_(other.timeout_task_.dispatcher()) {
  // The move constructor is only used during shut down below. So we simply
  // cancel the task and not worry about moving it.
  other.timeout_task_.Cancel();
  other.security_upgrade_.reset();
}

Bearer::PendingTransactionPtr Bearer::TransactionQueue::ClearCurrent() {
  PW_DCHECK(current_);
  PW_DCHECK(timeout_task_.is_pending());

  timeout_task_.Cancel();

  return std::move(current_);
}

void Bearer::TransactionQueue::Enqueue(PendingTransactionPtr&& transaction) {
  queue_.push_back(std::move(transaction));
}

void Bearer::TransactionQueue::EnqueueFront(
    PendingTransactionPtr&& transaction) {
  queue_.push_front(std::move(transaction));
}

void Bearer::TransactionQueue::SetSecurityUpgrade(
    PendingTransactionPtr&& transaction, std::pair<Error, Handle> error) {
  PW_DCHECK(!security_upgrade_);
  PW_DCHECK(transaction);
  security_upgrade_ = PendingSecurityUpgrade{
      .transaction = std::move(transaction),
      .error = std::move(error),
  };
}

std::optional<Bearer::TransactionQueue::PendingSecurityUpgrade>
Bearer::TransactionQueue::TakeSecurityUpgrade() {
  std::optional<PendingSecurityUpgrade> pending = std::move(security_upgrade_);
  security_upgrade_.reset();
  return pending;
}

void Bearer::TransactionQueue::TrySendNext(
    WeakPtr bearer,
    pw::async::TaskFunction timeout_cb,
    pw::chrono::SystemClock::duration timeout) {
  PW_DCHECK(bearer.is_alive());
  PW_DCHECK(bearer->is_open());

  while (!security_upgrade_ && !current_ && !queue_.empty()) {
    PW_DCHECK(!timeout_task_.is_pending());

    auto transaction = std::move(queue_.front());
    queue_.pop_front();
    PW_DCHECK(transaction);
    PW_DCHECK(transaction->pdu);

    // We copy the PDU payload in case it needs to be retried following a
    // security upgrade.
    auto pdu = NewBuffer(transaction->pdu->size());
    if (pdu) {
      transaction->pdu->Copy(pdu.get());
      current_ = std::move(transaction);
      timeout_task_.set_function(std::move(timeout_cb));
      timeout_task_.PostAfter(timeout);
      bearer->chan_->Send(std::move(pdu));
      break;
    }

    bt_log(TRACE, "att", "Failed to start transaction: out of memory!");
    auto cb = std::move(transaction->callback);
    transaction.reset();
    cb(fit::error(std::pair(Error(HostError::kOutOfMemory), kInvalidHandle)));
    if (!bearer.is_alive() || !bearer->is_open()) {
      return;
    }
  }
}

void Bearer::TransactionQueue::Reset() {
  timeout_task_.Cancel();
  queue_.clear();
  current_ = nullptr;
  security_upgrade_.reset();
}

void Bearer::TransactionQueue::InvokeErrorAll(Error error) {
  if (current_) {
    auto current = std::move(current_);
    timeout_task_.Cancel();
    if (current->callback) {
      current->callback(fit::error(std::pair(error, kInvalidHandle)));
    }
  }

  if (security_upgrade_) {
    auto pending = std::move(*security_upgrade_);
    security_upgrade_.reset();
    if (pending.transaction->callback) {
      pending.transaction->callback(
          fit::error(std::pair(error, kInvalidHandle)));
    }
  }

  while (!queue_.empty()) {
    auto t = std::move(queue_.front());
    queue_.pop_front();
    if (t->callback) {
      t->callback(fit::error(std::pair(error, kInvalidHandle)));
    }
  }
}

Bearer::Bearer(l2cap::Channel::WeakPtr chan, pw::async::Dispatcher& dispatcher)
    : dispatcher_(dispatcher),
      chan_(std::move(chan)),
      request_queue_(dispatcher_),
      indication_queue_(dispatcher_),
      next_remote_transaction_id_(1u),
      next_handler_id_(1u),
      weak_self_(this) {
  PW_DCHECK(chan_);

  if (chan_->link_type() == bt::LinkType::kLE) {
    min_mtu_ = kLEMinMTU;
  } else {
    min_mtu_ = kBREDRMinMTU;
  }

  mtu_ = min_mtu();
  // TODO(fxbug.dev/42087558): Dynamically configure preferred MTU value.
  preferred_mtu_ = kLEMaxMTU;
}

Bearer::~Bearer() {
  weak_self_.InvalidatePtrs();
  chan_ = nullptr;

  request_queue_.Reset();
  indication_queue_.Reset();
}

bool Bearer::Activate() {
  return chan_->Activate(fit::bind_member<&Bearer::OnRxBFrame>(this),
                         fit::bind_member<&Bearer::OnChannelClosed>(this));
}

void Bearer::ShutDown() {
  if (is_open())
    ShutDownInternal(/*due_to_timeout=*/false);
}

void Bearer::ShutDownInternal(bool due_to_timeout) {
  // Prevent this method from being run twice (e.g. by SignalLinkError() below).
  if (shut_down_) {
    return;
  }
  PW_CHECK(is_open());
  shut_down_ = true;

  bt_log(DEBUG, "att", "bearer shutting down");

  // Move the contents to temporaries. This prevents a potential memory
  // corruption in InvokeErrorAll if the Bearer gets deleted by one of the
  // invoked error callbacks.
  TransactionQueue req_queue(std::move(request_queue_));
  TransactionQueue ind_queue(std::move(indication_queue_));
  remote_request_.reset();
  remote_indication_.reset();

  fit::closure closed_cb = std::move(closed_cb_);

  l2cap::ScopedChannel chan = std::move(chan_);
  // SignalLinkError may delete the Bearer! Nothing below this line should
  // access |this|.
  chan->SignalLinkError();
  chan = nullptr;

  if (closed_cb) {
    closed_cb();
  }

  // Terminate all remaining procedures with an error. This is safe even if the
  // bearer got deleted by |closed_cb_|.
  Error error(due_to_timeout ? HostError::kTimedOut : HostError::kFailed);
  req_queue.InvokeErrorAll(error);
  ind_queue.InvokeErrorAll(error);
}

void Bearer::StartTransaction(ByteBufferPtr pdu, TransactionCallback callback) {
  PW_CHECK(pdu);
  PW_CHECK(callback);

  [[maybe_unused]] bool _ = SendInternal(std::move(pdu), std::move(callback));
}

bool Bearer::SendWithoutResponse(ByteBufferPtr pdu) {
  PW_CHECK(pdu);
  return SendInternal(std::move(pdu), {});
}

bool Bearer::SendInternal(ByteBufferPtr pdu, TransactionCallback callback) {
  auto _check_callback_empty = fit::defer([&callback]() {
    // Ensure that callback was either never present or called/moved before
    // SendInternal returns
    PW_CHECK(!callback);
  });

  if (!is_open()) {
    bt_log(TRACE, "att", "bearer closed; cannot send packet");
    if (callback) {
      callback(fit::error(
          std::pair(Error(HostError::kLinkDisconnected), kInvalidHandle)));
    }
    return false;
  }

  PW_CHECK(IsPacketValid(*pdu), "packet has bad length!");

  PacketReader reader(pdu.get());
  MethodType type = GetMethodType(reader.opcode());

  TransactionQueue* tq = nullptr;

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (type) {
    case MethodType::kCommand:
    case MethodType::kNotification:
      PW_CHECK(!callback,
               "opcode %#.2x has no response but callback was provided",
               reader.opcode());

      // Send the command. No flow control is necessary.
      chan_->Send(std::move(pdu));
      return true;

    case MethodType::kRequest:
      tq = &request_queue_;
      break;
    case MethodType::kIndication:
      tq = &indication_queue_;
      break;
    default:
      PW_CRASH("unsupported opcode: %#.2x", reader.opcode());
  }
  PW_MODIFY_DIAGNOSTICS_POP();

  PW_CHECK(callback,
           "transaction with opcode %#.2x has response that requires callback!",
           reader.opcode());

  tq->Enqueue(std::make_unique<PendingTransaction>(
      reader.opcode(), std::move(callback), std::move(pdu)));
  TryStartNextTransaction(tq);

  return true;
}

Bearer::HandlerId Bearer::RegisterHandler(OpCode opcode, Handler handler) {
  PW_DCHECK(handler);

  if (!is_open())
    return kInvalidHandlerId;

  if (handlers_.find(opcode) != handlers_.end()) {
    bt_log(DEBUG,
           "att",
           "can only register one handler per opcode (%#.2x)",
           opcode);
    return kInvalidHandlerId;
  }

  HandlerId id = NextHandlerId();
  if (id == kInvalidHandlerId)
    return kInvalidHandlerId;

  auto res = handler_id_map_.emplace(id, opcode);
  PW_CHECK(res.second, "handler ID got reused (id: %zu)", id);

  handlers_[opcode] = std::move(handler);

  return id;
}

void Bearer::UnregisterHandler(HandlerId id) {
  PW_DCHECK(id != kInvalidHandlerId);

  auto iter = handler_id_map_.find(id);
  if (iter == handler_id_map_.end()) {
    bt_log(DEBUG, "att", "cannot unregister unknown handler id: %zu", id);
    return;
  }

  OpCode opcode = iter->second;
  handler_id_map_.erase(iter);
  handlers_.erase(opcode);
}

bool Bearer::Reply(TransactionId tid, ByteBufferPtr pdu) {
  PW_DCHECK(pdu);

  if (tid == kInvalidTransactionId)
    return false;

  if (!is_open()) {
    bt_log(TRACE, "att", "bearer closed; cannot reply");
    return false;
  }

  if (!IsPacketValid(*pdu)) {
    bt_log(DEBUG, "att", "invalid response PDU");
    return false;
  }

  RemoteTransaction* pending = FindRemoteTransaction(tid);
  if (!pending)
    return false;

  PacketReader reader(pdu.get());

  // Use ReplyWithError() instead.
  if (reader.opcode() == kErrorResponse)
    return false;

  OpCode pending_opcode = (*pending)->opcode;
  if (pending_opcode != MatchingTransactionCode(reader.opcode())) {
    bt_log(DEBUG,
           "att",
           "opcodes do not match (pending: %#.2x, given: %#.2x)",
           pending_opcode,
           reader.opcode());
    return false;
  }

  pending->reset();
  chan_->Send(std::move(pdu));

  return true;
}

bool Bearer::ReplyWithError(TransactionId id,
                            Handle handle,
                            ErrorCode error_code) {
  if (!is_open()) {
    bt_log(TRACE, "att", "bearer closed; cannot reply with error");
    return false;
  }

  RemoteTransaction* pending = FindRemoteTransaction(id);
  if (!pending)
    return false;

  OpCode pending_opcode = (*pending)->opcode;
  if (pending_opcode == kIndication) {
    bt_log(DEBUG, "att", "cannot respond to an indication with error!");
    return false;
  }

  pending->reset();
  SendErrorResponse(pending_opcode, handle, error_code);

  return true;
}

bool Bearer::IsPacketValid(const ByteBuffer& packet) {
  return packet.size() != 0u && packet.size() <= mtu_;
}

// static
void Bearer::FailSecurityUpgrade(WeakPtr self) {
  if (!self.is_alive() || !self->is_open()) {
    return;
  }
  std::optional<TransactionQueue::PendingSecurityUpgrade> pending =
      self->request_queue_.TakeSecurityUpgrade();
  if (!pending.has_value()) {
    return;
  }
  auto cb = std::move(pending->transaction->callback);
  auto error_pair = std::move(pending->error);
  pending.reset();
  cb(fit::error(std::move(error_pair)));
  if (self.is_alive() && self->is_open()) {
    self->TryStartNextTransaction(&self->request_queue_);
  }
}

void Bearer::TryStartNextTransaction(TransactionQueue* tq) {
  PW_DCHECK(tq);

  if (!is_open()) {
    bt_log(TRACE, "att", "Cannot process transactions; bearer is closed");
    return;
  }

  tq->TrySendNext(
      weak_self_.GetWeakPtr(),
      [self = weak_self_.GetWeakPtr()](pw::async::Context /*ctx*/,
                                       pw::Status status) {
        if (self.is_alive() && status.ok()) {
          self->ShutDownInternal(/*due_to_timeout=*/true);
        }
      },
      kTransactionTimeout);
}

void Bearer::SendErrorResponse(OpCode request_opcode,
                               Handle attribute_handle,
                               ErrorCode error_code) {
  auto buffer = NewBuffer(sizeof(Header) + sizeof(ErrorResponseParams));
  PW_CHECK(buffer);

  PacketWriter packet(kErrorResponse, buffer.get());
  auto* payload = packet.mutable_payload<ErrorResponseParams>();
  payload->request_opcode = request_opcode;
  payload->attribute_handle =
      pw::bytes::ConvertOrderTo(cpp20::endian::little, attribute_handle);
  payload->error_code = error_code;

  chan_->Send(std::move(buffer));
}

void Bearer::HandleEndTransaction(bool is_request, const PacketReader& packet) {
  PW_DCHECK(is_open());

  TransactionQueue& tq = is_request ? request_queue_ : indication_queue_;

  if (!tq.current()) {
    bt_log(DEBUG,
           "att",
           "received unexpected transaction PDU (opcode: %#.2x)",
           packet.opcode());
    ShutDown();
    return;
  }

  OpCode target_opcode;
  std::optional<std::pair<Error, Handle>> error;

  if (packet.opcode() == kErrorResponse) {
    // We should never hit this branch for indications.
    PW_DCHECK(tq.current()->opcode != kIndication);

    if (packet.payload_size() == sizeof(ErrorResponseParams)) {
      const auto& payload = packet.payload<ErrorResponseParams>();
      target_opcode = payload.request_opcode;
      const ErrorCode error_code = payload.error_code;
      const Handle attr_in_error = pw::bytes::ConvertOrderFrom(
          cpp20::endian::little, payload.attribute_handle);
      error.emplace(std::pair(Error(error_code), attr_in_error));
    } else {
      bt_log(DEBUG, "att", "received malformed error response");

      // Invalid opcode will fail the opcode comparison below.
      target_opcode = kInvalidOpCode;
    }
  } else {
    target_opcode = MatchingTransactionCode(packet.opcode());
  }

  PW_DCHECK(tq.current()->opcode != kInvalidOpCode);

  if (tq.current()->opcode != target_opcode) {
    bt_log(DEBUG,
           "att",
           "received bad transaction PDU (opcode: %#.2x)",
           packet.opcode());
    ShutDown();
    return;
  }

  // The transaction is complete.
  auto transaction = tq.ClearCurrent();
  PW_DCHECK(transaction);

  const sm::SecurityLevel security_requirement =
      error.has_value()
          ? CheckSecurity(error->first.protocol_error(), chan_->security())
          : sm::SecurityLevel::kNoSecurity;
  if (transaction->security_retry_level >= security_requirement ||
      security_requirement <= chan_->security().level()) {
    // The transaction callback may result in our connection being closed.
    auto self = weak_self_.GetWeakPtr();

    // Resolve the transaction. Move the callback to the stack and reset the
    // transaction first in case invoking the callback destroys the bearer.
    auto cb = std::move(transaction->callback);
    transaction.reset();
    if (error.has_value()) {
      cb(fit::error(error.value()));
    } else {
      cb(fit::ok(packet));
    }

    // Check if the bearer was destroyed in the callback.
    if (self.is_alive()) {
      // Send out the next queued transaction.
      TransactionQueue* queue =
          is_request ? &self->request_queue_ : &self->indication_queue_;
      self->TryStartNextTransaction(queue);
    }
    return;
  }

  PW_CHECK(error.has_value());
  bt_log(TRACE,
         "att",
         "Received security error %s for transaction; requesting upgrade to "
         "level: %s",
         bt_str(error->first),
         sm::LevelToString(security_requirement));
  // UpgradeSecurity can fail synchronously (or drop its callback without
  // invoking it when the channel/link is inactive), which may cascade into
  // LEConnectionManager closing the connection and destroying the Bearer. Keep
  // the pending transaction owned by |request_queue_| so ShutDownInternal()
  // fails it in FIFO order and ~Bearer() drops it without invoking callbacks.
  request_queue_.SetSecurityUpgrade(std::move(transaction), *std::move(error));
  auto self = weak_self_.GetWeakPtr();
  chan_->UpgradeSecurity(
      security_requirement,
      [self, security_requirement, on_drop = fit::defer([self]() {
                                     FailSecurityUpgrade(self);
                                   })](sm::Result<> status) mutable {
        on_drop.cancel();
        auto self_copy = self;
        if (!self_copy.is_alive() || !self_copy->is_open()) {
          return;
        }
        if (status.is_error()) {
          FailSecurityUpgrade(std::move(self_copy));
          return;
        }

        std::optional<TransactionQueue::PendingSecurityUpgrade> pending =
            self_copy->request_queue_.TakeSecurityUpgrade();
        if (!pending.has_value()) {
          return;
        }

        // TODO(armansito): Notify the upper layer to re-initiate service
        // discovery and other necessary procedures (see the Core Spec v5.0, Vol
        // 3, Part C, 10.3.2).

        // Re-send the request as described in the Core Spec v5.0, Vol 3, Part
        // G, 8.1. Since |pending| was originally resolved with an Error
        // Response, it must have come out of |request_queue_|.
        PW_DCHECK(GetMethodType(pending->transaction->opcode) ==
                  MethodType::kRequest);
        pending->transaction->security_retry_level = security_requirement;
        self_copy->request_queue_.EnqueueFront(std::move(pending->transaction));
        self_copy->TryStartNextTransaction(&self_copy->request_queue_);
      });
}

Bearer::HandlerId Bearer::NextHandlerId() {
  auto id = next_handler_id_;

  // This will stop incrementing if this were overflows and always return
  // kInvalidHandlerId.
  if (next_handler_id_ != kInvalidHandlerId)
    next_handler_id_++;
  return id;
}

Bearer::TransactionId Bearer::NextRemoteTransactionId() {
  auto id = next_remote_transaction_id_;

  next_remote_transaction_id_++;

  // Increment extra in the case of overflow.
  if (next_remote_transaction_id_ == kInvalidTransactionId)
    next_remote_transaction_id_++;

  return id;
}

void Bearer::HandleBeginTransaction(RemoteTransaction* currently_pending,
                                    const PacketReader& packet) {
  PW_DCHECK(currently_pending);

  if (currently_pending->has_value()) {
    bt_log(DEBUG,
           "att",
           "A transaction is already pending! (opcode: %#.2x)",
           packet.opcode());
    ShutDown();
    return;
  }

  auto iter = handlers_.find(packet.opcode());
  if (iter == handlers_.end()) {
    bt_log(DEBUG,
           "att",
           "no handler registered for opcode %#.2x",
           packet.opcode());
    SendErrorResponse(packet.opcode(), 0, ErrorCode::kRequestNotSupported);
    return;
  }

  auto id = NextRemoteTransactionId();
  *currently_pending = PendingRemoteTransaction(id, packet.opcode());

  Handler handler = iter->second.share();
  handler(id, packet);
}

Bearer::RemoteTransaction* Bearer::FindRemoteTransaction(TransactionId id) {
  if (remote_request_ && remote_request_->id == id) {
    return &remote_request_;
  }

  if (remote_indication_ && remote_indication_->id == id) {
    return &remote_indication_;
  }

  bt_log(DEBUG, "att", "id %zu does not match any transaction", id);
  return nullptr;
}

void Bearer::HandlePDUWithoutResponse(const PacketReader& packet) {
  auto iter = handlers_.find(packet.opcode());
  if (iter == handlers_.end()) {
    bt_log(DEBUG,
           "att",
           "dropping unhandled packet (opcode: %#.2x)",
           packet.opcode());
    return;
  }

  Handler handler = iter->second.share();
  handler(kInvalidTransactionId, packet);
}

void Bearer::OnChannelClosed() {
  // This will deactivate the channel and notify |closed_cb_|.
  ShutDown();
}

void Bearer::OnRxBFrame(ByteBufferPtr sdu) {
  PW_DCHECK(sdu);
  PW_DCHECK(is_open());

  TRACE_DURATION("bluetooth", "att::Bearer::OnRxBFrame", "length", sdu->size());

  if (sdu->size() > mtu_) {
    bt_log(DEBUG, "att", "PDU exceeds MTU!");
    ShutDown();
    return;
  }

  // This static cast is safe because we have verified that `sdu->size()` fits
  // in a uint16_t with the above check and the below static_assert.
  static_assert(std::is_same_v<uint16_t, decltype(mtu_)>);
  auto length = static_cast<uint16_t>(sdu->size());

  // An ATT PDU should at least contain the opcode.
  if (length < sizeof(OpCode)) {
    bt_log(DEBUG, "att", "PDU too short!");
    ShutDown();
    return;
  }

  PacketReader packet(sdu.get());
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (GetMethodType(packet.opcode())) {
    case MethodType::kResponse:
      HandleEndTransaction(/*is_request=*/true, packet);
      break;
    case MethodType::kConfirmation:
      HandleEndTransaction(/*is_request=*/false, packet);
      break;
    case MethodType::kRequest:
      HandleBeginTransaction(&remote_request_, packet);
      break;
    case MethodType::kIndication:
      HandleBeginTransaction(&remote_indication_, packet);
      break;
    case MethodType::kNotification:
    case MethodType::kCommand:
      HandlePDUWithoutResponse(packet);
      break;
    default:
      bt_log(DEBUG, "att", "Unsupported opcode: %#.2x", packet.opcode());
      SendErrorResponse(packet.opcode(), 0, ErrorCode::kRequestNotSupported);
      break;
  }
  PW_MODIFY_DIAGNOSTICS_POP();
}

}  // namespace bt::att
