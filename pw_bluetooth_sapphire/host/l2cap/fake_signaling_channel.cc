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

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_signaling_channel.h"

#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal::testing {
namespace {

// These classes bind the response that the request handlers are expected to
// send back. These also serve as the actual Responder implementation that the
// request handler under test will see. These roles may need to be decoupled if
// request handlers have to be tested for multiple responses to each request.
class Expecter : public SignalingChannel::Responder {
 public:
  void Send(const ByteBuffer& rsp_payload) override {
    ADD_FAILURE() << "Unexpected local response " << rsp_payload.AsString();
  }

  void RejectNotUnderstood() override {
    ADD_FAILURE() << "Unexpected local rejection, \"Not Understood\"";
  }

  void RejectInvalidChannelId(ChannelId local_cid,
                              ChannelId remote_cid) override {
    ADD_FAILURE() << bt_lib_cpp_string::StringPrintf(
        "Unexpected local rejection, \"Invalid Channel ID\" local: %#.4x "
        "remote: %#.4x",
        local_cid,
        remote_cid);
  }

  bool called() const { return called_; }

 protected:
  void set_called(bool called) { called_ = called; }

 private:
  bool called_ = false;
};

class ResponseExpecter : public Expecter {
 public:
  explicit ResponseExpecter(const ByteBuffer& expected_rsp)
      : expected_rsp_(expected_rsp) {}

  void Send(const ByteBuffer& rsp_payload) override {
    set_called(true);
    EXPECT_TRUE(ContainersEqual(expected_rsp_, rsp_payload));
  }

 private:
  const ByteBuffer& expected_rsp_;
};

class RejectNotUnderstoodExpecter : public Expecter {
 public:
  void RejectNotUnderstood() override { set_called(true); }
};

class RejectInvalidChannelIdExpecter : public Expecter {
 public:
  RejectInvalidChannelIdExpecter(ChannelId local_cid, ChannelId remote_cid)
      : local_cid_(local_cid), remote_cid_(remote_cid) {}

  void RejectInvalidChannelId(ChannelId local_cid,
                              ChannelId remote_cid) override {
    set_called(true);
    EXPECT_EQ(local_cid_, local_cid);
    EXPECT_EQ(remote_cid_, remote_cid);
  }

 private:
  const ChannelId local_cid_;
  const ChannelId remote_cid_;
};

}  // namespace

FakeSignalingChannel::FakeSignalingChannel(pw::async::Dispatcher& pw_dispatcher)
    : heap_dispatcher_(pw_dispatcher) {}

FakeSignalingChannel::~FakeSignalingChannel() {
  // Add a test failure for each expected request that wasn't received
  for (size_t i = expected_transaction_index_; i < transactions_.size(); i++) {
    ADD_FAILURE_AT(transactions_[i].file, transactions_[i].line)
        << "Outbound request [" << i << "] expected "
        << transactions_[i].responses.size() << " responses";
  }
}

bool FakeSignalingChannel::SendRequest(CommandCode req_code,
                                       const ByteBuffer& payload,
                                       SignalingChannel::ResponseHandler cb) {
  if (expected_transaction_index_ >= transactions_.size()) {
    ADD_FAILURE() << "Received unexpected outbound command after handling "
                  << transactions_.size();
    return false;
  }

  size_t transaction_index = expected_transaction_index_++;
  Transaction& transaction = transactions_[transaction_index];
  ::testing::ScopedTrace trace(
      transaction.file, transaction.line, "Outbound request expected here");
  EXPECT_EQ(transaction.request_code, req_code);
  EXPECT_TRUE(ContainersEqual(transaction.req_payload, payload));
  EXPECT_TRUE(cb);
  transaction.response_callback = std::move(cb);

  if (simulate_send_failure_) {
    return false;
  }

  // Simulate the remote's response(s)
  (void)heap_dispatcher_.Post(
      [this, index = transaction_index](pw::async::Context /*ctx*/,
                                        pw::Status status) {
        if (status.ok()) {
          Transaction& pending_tx = transactions_[index];
          pending_tx.responses_handled =
              TriggerResponses(pending_tx, pending_tx.responses);
        }
      });

  return (transaction.request_code == req_code);
}

void FakeSignalingChannel::ReceiveResponses(
    TransactionId id,
    const std::vector<FakeSignalingChannel::Response>& responses) {
  if (id >= transactions_.size()) {
    FAIL() << "Can't trigger response to unknown outbound request " << id;
  }

  const Transaction& transaction = transactions_[id];
  {
    ::testing::ScopedTrace trace(
        transaction.file, transaction.line, "Outbound request expected here");
    ASSERT_TRUE(transaction.response_callback)
        << "Can't trigger responses for outbound request that hasn't been sent";
    EXPECT_EQ(transaction.responses.size(), transaction.responses_handled)
        << "Not all original simulated responses have been handled";
  }
  TriggerResponses(transaction, responses);
}

void FakeSignalingChannel::ServeRequest(CommandCode req_code,
                                        SignalingChannel::RequestDelegate cb) {
  request_handlers_[req_code] = std::move(cb);
}

bool FakeSignalingChannel::SendCommandWithoutResponse(
    CommandCode req_code, const ByteBuffer& payload) {
  if (expected_transaction_index_ >= transactions_.size()) {
    ADD_FAILURE() << "Received unexpected outbound command after handling "
                  << transactions_.size();
    return false;
  }

  Transaction& transaction = transactions_[expected_transaction_index_];
  ::testing::ScopedTrace trace(
      transaction.file, transaction.line, "Outbound request expected here");
  EXPECT_EQ(transaction.request_code, req_code);
  EXPECT_TRUE(ContainersEqual(transaction.req_payload, payload));
  EXPECT_EQ(0u, transaction.responses.size());

  ++expected_transaction_index_;
  return (transaction.request_code == req_code);
}

FakeSignalingChannel::TransactionId FakeSignalingChannel::AddOutbound(
    const char* file,
    int line,
    CommandCode req_code,
    BufferView req_payload,
    std::vector<FakeSignalingChannel::Response> responses) {
  transactions_.push_back(Transaction{file,
                                      line,
                                      req_code,
                                      std::move(req_payload),
                                      std::move(responses),
                                      nullptr});
  return transactions_.size() - 1;
}

void FakeSignalingChannel::Receive(CommandCode req_code,
                                   const ByteBuffer& req_payload) {
  Expecter expecter;
  ReceiveExpectInternal(req_code, req_payload, &expecter);
}

void FakeSignalingChannel::ReceiveExpect(CommandCode req_code,
                                         const ByteBuffer& req_payload,
                                         const ByteBuffer& rsp_payload) {
  ResponseExpecter expecter(rsp_payload);
  ReceiveExpectInternal(req_code, req_payload, &expecter);
  EXPECT_TRUE(expecter.called());
}

void FakeSignalingChannel::ReceiveExpectRejectNotUnderstood(
    CommandCode req_code, const ByteBuffer& req_payload) {
  RejectNotUnderstoodExpecter expecter;
  ReceiveExpectInternal(req_code, req_payload, &expecter);
}

void FakeSignalingChannel::ReceiveExpectRejectInvalidChannelId(
    CommandCode req_code,
    const ByteBuffer& req_payload,
    ChannelId local_cid,
    ChannelId remote_cid) {
  RejectInvalidChannelIdExpecter expecter(local_cid, remote_cid);
  ReceiveExpectInternal(req_code, req_payload, &expecter);
}

size_t FakeSignalingChannel::TriggerResponses(
    const FakeSignalingChannel::Transaction& transaction,
    const std::vector<FakeSignalingChannel::Response>& responses) {
  ::testing::ScopedTrace trace(
      transaction.file, transaction.line, "Outbound request expected here");
  size_t responses_handled = 0;
  for (auto& [status, payload] : responses) {
    responses_handled++;
    if (transaction.response_callback(status, payload) ==
            ResponseHandlerAction::kCompleteOutboundTransaction ||
        ::testing::Test::HasFatalFailure()) {
      break;
    }
  }

  EXPECT_EQ(responses.size(), responses_handled)
      << bt_lib_cpp_string::StringPrintf(
             "Outbound command (code %d, at %zu) handled fewer responses than "
             "expected",
             transaction.request_code,
             transaction.responses_handled);

  return responses_handled;
}

// Test evaluator for inbound requests with type-erased, bound expected requests
void FakeSignalingChannel::ReceiveExpectInternal(CommandCode req_code,
                                                 const ByteBuffer& req_payload,
                                                 Responder* fake_responder) {
  auto iter = request_handlers_.find(req_code);
  ASSERT_NE(request_handlers_.end(), iter);

  // Invoke delegate assigned for this request type
  iter->second(req_payload, fake_responder);
}

}  // namespace bt::l2cap::internal::testing
