// Copyright 2026 The Pigweed Authors
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

#include "pw_rpc2/test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_buf/buf.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_rpc2/pw_rpc2_test.pwpb.rpc2.h"
#include "pw_rpc2/pw_rpc2_test.raw.rpc2.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/status.h"
#include "pw_rpc2/writer.h"
#include "pw_unit_test/framework.h"

namespace {

namespace async2 = ::pw::async2;

using ::pw::rpc2::BidiStreamCall;
using ::pw::rpc2::BidiStreamFuture;
using ::pw::rpc2::ClientStreamCall;
using ::pw::rpc2::ClientStreamFuture;
using ::pw::rpc2::Reader;
using ::pw::rpc2::ReadFuture;
using ::pw::rpc2::RpcResult;
using ::pw::rpc2::RpcStatus;
using ::pw::rpc2::UnaryFuture;
using ::pw::rpc2::UnaryWriter;
using ::pw::rpc2::WriteFuture;
using ::pw::rpc2::Writer;
using ::pw::rpc2::test::TestFuture;
using ::pw::rpc2::test::TestHarness;

using EchoRequest = ::pw::rpc2::test::pwpb::EchoRequest::Message;
using EchoResponse = ::pw::rpc2::test::pwpb::EchoResponse::Message;
namespace PwpbTestEcho = ::pw::rpc2::test::pw_rpc2::pwpb::TestEcho;
namespace RawTestEcho = ::pw::rpc2::test::pw_rpc2::raw::TestEcho;

// Service that implements every RPC by declaring `<Method>Future` types.
class EchoFutureService : public PwpbTestEcho::Service<EchoFutureService> {
 public:
  uint32_t calls() const { return calls_; }

  class EchoUnaryFuture : public TestFuture {
   public:
    EchoUnaryFuture() = default;
    EchoUnaryFuture(EchoFutureService& service,
                    EchoRequest request,
                    UnaryWriter<EchoResponse> responder)
        : TestFuture(true),
          request_(request),
          responder_(std::move(responder)) {
      ++service.calls_;
    }

    async2::Poll<> Pend(async2::Context& cx) {
      if (!finish_.is_pendable()) {
        finish_ = responder_.Finish(EchoResponse{.val = request_.val * 2});
      }
      PW_AWAIT(RpcStatus status, finish_, cx);
      PW_TEST_EXPECT_OK(status);
      return Complete();
    }

   private:
    EchoRequest request_;
    UnaryWriter<EchoResponse> responder_;
    WriteFuture<EchoResponse> finish_;
  };

  class CountUpServerStreamFuture : public TestFuture {
   public:
    CountUpServerStreamFuture() = default;
    CountUpServerStreamFuture(EchoRequest request, Writer<EchoResponse> writer)
        : TestFuture(true), request_(request), writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      if (std::holds_alternative<std::monostate>(curr_fut_)) {
        curr_fut_ =
            writer_.Write(EchoResponse{.val = request_.val + current_step_});
      }
      while (auto* write = std::get_if<WriteFuture<EchoResponse>>(&curr_fut_)) {
        PW_AWAIT(RpcStatus status, *write, cx);
        PW_TEST_EXPECT_OK(status);
        if (++current_step_ <= 3) {
          curr_fut_ =
              writer_.Write(EchoResponse{.val = request_.val + current_step_});
        } else {
          curr_fut_ = writer_.Finish();
        }
      }
      PW_AWAIT(RpcStatus status, std::get<WriteFuture<>>(curr_fut_), cx);
      PW_TEST_EXPECT_OK(status);
      return Complete();
    }

   private:
    EchoRequest request_;
    Writer<EchoResponse> writer_;
    uint32_t current_step_ = 1;
    std::variant<std::monostate, WriteFuture<EchoResponse>, WriteFuture<>>
        curr_fut_;
  };

  class AccumulateClientStreamFuture : public TestFuture {
   public:
    AccumulateClientStreamFuture() = default;
    AccumulateClientStreamFuture(Reader<EchoRequest> reader,
                                 UnaryWriter<EchoResponse> responder)
        : TestFuture(true),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      if (std::holds_alternative<std::monostate>(curr_fut_)) {
        curr_fut_ = reader_.Read();
      }
      while (auto* read = std::get_if<ReadFuture<EchoRequest>>(&curr_fut_)) {
        PW_AWAIT(auto res, *read, cx);
        if (res.ok()) {
          sum_ += res->val;
          curr_fut_ = reader_.Read();
        } else {
          curr_fut_ = responder_.Finish(EchoResponse{.val = sum_});
        }
      }
      PW_AWAIT(
          RpcStatus status, std::get<WriteFuture<EchoResponse>>(curr_fut_), cx);
      PW_TEST_EXPECT_OK(status);
      return Complete();
    }

   private:
    Reader<EchoRequest> reader_;
    UnaryWriter<EchoResponse> responder_;
    std::variant<std::monostate,
                 ReadFuture<EchoRequest>,
                 WriteFuture<EchoResponse>>
        curr_fut_;
    uint32_t sum_ = 0;
  };

  class EchoBidiStreamFuture : public TestFuture {
   public:
    EchoBidiStreamFuture() = default;
    EchoBidiStreamFuture(Reader<EchoRequest> reader,
                         Writer<EchoResponse> writer)
        : TestFuture(true),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      if (std::holds_alternative<std::monostate>(curr_fut_)) {
        curr_fut_ = reader_.Read();
      }
      while (!std::holds_alternative<WriteFuture<>>(curr_fut_)) {
        if (auto* read = std::get_if<ReadFuture<EchoRequest>>(&curr_fut_)) {
          PW_AWAIT(auto res, *read, cx);
          if (res.ok()) {
            curr_fut_ = writer_.Write(EchoResponse{.val = res->val * 2});
          } else {
            curr_fut_ = writer_.Finish();
          }
        } else if (auto* write =
                       std::get_if<WriteFuture<EchoResponse>>(&curr_fut_)) {
          PW_AWAIT(RpcStatus status, *write, cx);
          PW_TEST_EXPECT_OK(status);
          curr_fut_ = reader_.Read();
        }
      }
      PW_AWAIT(RpcStatus status, std::get<WriteFuture<>>(curr_fut_), cx);
      PW_TEST_EXPECT_OK(status);
      return Complete();
    }

   private:
    Reader<EchoRequest> reader_;
    Writer<EchoResponse> writer_;
    std::variant<std::monostate,
                 ReadFuture<EchoRequest>,
                 WriteFuture<EchoResponse>,
                 WriteFuture<>>
        curr_fut_;
  };

 private:
  uint32_t calls_ = 0;
};
static_assert(async2::Future<EchoFutureService::EchoUnaryFuture>);
static_assert(async2::Future<EchoFutureService::CountUpServerStreamFuture>);
static_assert(async2::Future<EchoFutureService::AccumulateClientStreamFuture>);
static_assert(async2::Future<EchoFutureService::EchoBidiStreamFuture>);

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;

// DOCSTAG: [pw_rpc2-testing-coro]
TEST(TestHarnessTest, UnaryCoroClient) {
  TestHarness<EchoFutureService> tester;

  tester.RunToCompletion([&](CoroContext) -> Coro<void> {
    RpcResult<EchoResponse> resp =
        co_await tester.service_client().EchoUnary(EchoRequest{.val = 21});
    PW_TEST_EXPECT_OK(resp);
    if (resp.ok()) {
      EXPECT_EQ(resp->val, 42u);
    }
  });
}

TEST(TestHarnessTest, ServerStreamingCoroClient) {
  TestHarness<EchoFutureService> tester;

  tester.RunToCompletion([&](CoroContext) -> Coro<void> {
    RpcResult<Reader<EchoResponse>> reader =
        co_await tester.service_client().CountUpServerStream(
            EchoRequest{.val = 100});
    PW_TEST_EXPECT_OK(reader);
    if (!reader.ok()) {
      co_return;
    }

    for (uint32_t expected = 101; expected <= 103; ++expected) {
      RpcResult<EchoResponse> msg = co_await reader->Read();
      PW_TEST_EXPECT_OK(msg);
      if (msg.ok()) {
        EXPECT_EQ(msg->val, expected);
      }
    }
    EXPECT_EQ((co_await reader->Read()).status(), RpcStatus::EndOfStream());
  });
}

TEST(TestHarnessTest, ClientStreamingCoroClient) {
  TestHarness<EchoFutureService> tester;

  tester.RunToCompletion([&](CoroContext) -> Coro<void> {
    auto call = co_await tester.service_client().AccumulateClientStream();
    PW_TEST_EXPECT_OK(call);
    if (!call.ok()) {
      co_return;
    }

    PW_TEST_EXPECT_OK(co_await call->writer().Write(EchoRequest{.val = 10}));
    PW_TEST_EXPECT_OK(co_await call->writer().Write(EchoRequest{.val = 20}));
    PW_TEST_EXPECT_OK(co_await call->writer().Write(EchoRequest{.val = 30}));
    PW_TEST_EXPECT_OK(co_await call->writer().Finish());

    RpcResult<EchoResponse> resp = co_await call->response();
    PW_TEST_EXPECT_OK(resp);
    if (resp.ok()) {
      EXPECT_EQ(resp->val, 60u);
    }
  });
}

TEST(TestHarnessTest, BidiStreamingCoroClient) {
  TestHarness<EchoFutureService> tester;

  tester.RunToCompletion([&](CoroContext) -> Coro<void> {
    auto call = co_await tester.service_client().EchoBidiStream();
    PW_TEST_EXPECT_OK(call);
    if (!call.ok()) {
      co_return;
    }

    PW_TEST_EXPECT_OK(co_await call->writer().Write(EchoRequest{.val = 5}));
    RpcResult<EchoResponse> first = co_await call->reader().Read();
    PW_TEST_EXPECT_OK(first);
    if (first.ok()) {
      EXPECT_EQ(first->val, 10u);
    }

    PW_TEST_EXPECT_OK(co_await call->writer().Write(EchoRequest{.val = 15}));
    RpcResult<EchoResponse> second = co_await call->reader().Read();
    PW_TEST_EXPECT_OK(second);
    if (second.ok()) {
      EXPECT_EQ(second->val, 30u);
    }

    PW_TEST_EXPECT_OK(co_await call->writer().Finish());
    EXPECT_EQ((co_await call->reader().Read()).status(),
              RpcStatus::EndOfStream());
  });
}
// DOCSTAG: [pw_rpc2-testing-coro]

TEST(TestHarnessTest, CoroCallableReturningValue) {
  TestHarness<EchoFutureService> tester;

  uint32_t result = tester.RunToCompletion([&](CoroContext) -> Coro<uint32_t> {
    RpcResult<EchoResponse> resp =
        co_await tester.service_client().EchoUnary(EchoRequest{.val = 11});
    PW_TEST_EXPECT_OK(resp);
    co_return resp.ok() ? resp->val : 0u;
  });
  EXPECT_EQ(result, 22u);
}

#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// DOCSTAG: [pw_rpc2-testing-futures]
TEST(TestHarnessTest, UnaryDirectFuture) {
  TestHarness<EchoFutureService> tester;

  RpcResult<EchoResponse> resp = tester.RunToCompletion(
      tester.service_client().EchoUnary(EchoRequest{.val = 21}));
  PW_TEST_ASSERT_OK(resp);
  EXPECT_EQ(resp->val, 42u);
  EXPECT_EQ(tester.service().calls(), 1u);
}
// DOCSTAG: [pw_rpc2-testing-futures]

TEST(TestHarnessTest, ServerStreamingNonCoro) {
  TestHarness<EchoFutureService> tester;

  RpcResult<Reader<EchoResponse>> reader = tester.RunToCompletion(
      tester.service_client().CountUpServerStream(EchoRequest{.val = 100}));
  PW_TEST_ASSERT_OK(reader);

  for (uint32_t expected = 101; expected <= 103; ++expected) {
    RpcResult<EchoResponse> msg = tester.RunToCompletion(reader->Read());
    PW_TEST_ASSERT_OK(msg);
    EXPECT_EQ(msg->val, expected);
  }
  EXPECT_EQ(tester.RunToCompletion(reader->Read()).status(),
            RpcStatus::EndOfStream());
}

// Client task that makes one `EchoUnary` call and stores the response.
class EchoUnaryClientTask : public async2::Task {
 public:
  EchoUnaryClientTask(PwpbTestEcho::Client client, uint32_t val)
      : client_(std::move(client)), val_(val) {}

  const RpcResult<EchoResponse>& result() const { return result_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (!future_.is_pendable()) {
      future_ = client_.EchoUnary(EchoRequest{.val = val_});
    }
    PW_AWAIT(result_, future_, cx);
    return async2::Ready();
  }

  PwpbTestEcho::Client client_;
  uint32_t val_;
  UnaryFuture<EchoRequest, EchoResponse> future_;
  RpcResult<EchoResponse> result_ = RpcStatus::Aborted();
};

// DOCSTAG: [pw_rpc2-testing-task]
TEST(TestHarnessTest, UnaryTask) {
  TestHarness<EchoFutureService> tester;

  EchoUnaryClientTask task(tester.service_client(), 21);
  tester.RunToCompletion(task);

  PW_TEST_ASSERT_OK(task.result());
  EXPECT_EQ(task.result()->val, 42u);
}
// DOCSTAG: [pw_rpc2-testing-task]

// Client task that streams a list of values to `AccumulateClientStream` and
// stores the final response.
class AccumulateClientTask : public async2::Task {
 public:
  explicit AccumulateClientTask(PwpbTestEcho::Client client,
                                std::array<uint32_t, 3> values)
      : client_(std::move(client)), values_(values) {}

  const RpcResult<EchoResponse>& result() const { return result_; }

 private:
  struct Receiving {};

  async2::Poll<> DoPend(async2::Context& cx) override {
    if (std::holds_alternative<std::monostate>(curr_fut_)) {
      curr_fut_ = client_.AccumulateClientStream();
    }
    if (auto* start =
            std::get_if<ClientStreamFuture<EchoRequest, EchoResponse>>(
                &curr_fut_)) {
      PW_AWAIT(auto call, *start, cx);
      if (!call.ok()) {
        result_ = call.status();
        return async2::Ready();
      }
      call_ = std::move(*call);
      curr_fut_ = call_.writer().Write(EchoRequest{.val = values_[index_]});
    }
    while (auto* write = std::get_if<WriteFuture<EchoRequest>>(&curr_fut_)) {
      PW_AWAIT(RpcStatus status, *write, cx);
      if (!status.ok()) {
        result_ = status;
        return async2::Ready();
      }
      if (++index_ < values_.size()) {
        curr_fut_ = call_.writer().Write(EchoRequest{.val = values_[index_]});
      } else {
        curr_fut_ = call_.writer().Finish();
      }
    }
    if (auto* finish = std::get_if<WriteFuture<>>(&curr_fut_)) {
      PW_AWAIT(RpcStatus status, *finish, cx);
      if (!status.ok()) {
        result_ = status;
        return async2::Ready();
      }
      curr_fut_ = Receiving{};
    }
    PW_AWAIT(result_, call_.response(), cx);
    return async2::Ready();
  }

  PwpbTestEcho::Client client_;
  std::array<uint32_t, 3> values_;
  size_t index_ = 0;
  ClientStreamCall<EchoRequest, EchoResponse> call_;
  std::variant<std::monostate,
               ClientStreamFuture<EchoRequest, EchoResponse>,
               WriteFuture<EchoRequest>,
               WriteFuture<>,
               Receiving>
      curr_fut_;
  RpcResult<EchoResponse> result_ = RpcStatus::Aborted();
};

// Client task that streams requests and reads responses on `EchoBidiStream`.
class EchoBidiClientTask : public async2::Task {
 public:
  explicit EchoBidiClientTask(PwpbTestEcho::Client client,
                              std::array<uint32_t, 2> requests)
      : client_(std::move(client)), requests_(requests) {}

  RpcStatus status() const { return status_; }
  const std::array<uint32_t, 2>& responses() const { return responses_; }
  size_t response_count() const { return response_count_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (std::holds_alternative<std::monostate>(curr_fut_)) {
      curr_fut_ = client_.EchoBidiStream();
    }
    if (auto* start = std::get_if<BidiStreamFuture<EchoRequest, EchoResponse>>(
            &curr_fut_)) {
      PW_AWAIT(auto call, *start, cx);
      if (!call.ok()) {
        status_ = call.status();
        return async2::Ready();
      }
      call_ = std::move(*call);
      curr_fut_ = call_.writer().Write(EchoRequest{.val = requests_[index_++]});
    }
    while (response_count_ < requests_.size()) {
      if (auto* write = std::get_if<WriteFuture<EchoRequest>>(&curr_fut_)) {
        PW_AWAIT(RpcStatus status, *write, cx);
        if (!status.ok()) {
          status_ = status;
          return async2::Ready();
        }
        curr_fut_ = call_.reader().Read();
      } else if (auto* read =
                     std::get_if<ReadFuture<EchoResponse>>(&curr_fut_)) {
        PW_AWAIT(RpcResult<EchoResponse> resp, *read, cx);
        if (!resp.ok()) {
          status_ = resp.status();
          return async2::Ready();
        }
        responses_[response_count_++] = resp->val;
        if (index_ < requests_.size()) {
          curr_fut_ =
              call_.writer().Write(EchoRequest{.val = requests_[index_++]});
        } else {
          curr_fut_ = call_.writer().Finish();
        }
      }
    }
    if (auto* finish = std::get_if<WriteFuture<>>(&curr_fut_)) {
      PW_AWAIT(RpcStatus status, *finish, cx);
      if (!status.ok()) {
        status_ = status;
        return async2::Ready();
      }
      curr_fut_ = call_.reader().Read();
    }
    PW_AWAIT(RpcResult<EchoResponse> end,
             std::get<ReadFuture<EchoResponse>>(curr_fut_),
             cx);
    status_ = end.status().IsEndOfStream() ? RpcStatus() : end.status();
    return async2::Ready();
  }

  PwpbTestEcho::Client client_;
  std::array<uint32_t, 2> requests_;
  std::array<uint32_t, 2> responses_{};
  size_t response_count_ = 0;
  size_t index_ = 0;
  BidiStreamCall<EchoRequest, EchoResponse> call_;
  std::variant<std::monostate,
               BidiStreamFuture<EchoRequest, EchoResponse>,
               WriteFuture<EchoRequest>,
               ReadFuture<EchoResponse>,
               WriteFuture<>>
      curr_fut_;
  RpcStatus status_ = RpcStatus::Aborted();
};

TEST(TestHarnessTest, ClientStreamingNonCoro) {
  TestHarness<EchoFutureService> tester;

  AccumulateClientTask task(tester.service_client(), {10, 20, 30});
  tester.RunToCompletion(task);

  PW_TEST_ASSERT_OK(task.result());
  EXPECT_EQ(task.result()->val, 60u);
}

TEST(TestHarnessTest, BidiStreamingNonCoro) {
  TestHarness<EchoFutureService> tester;

  EchoBidiClientTask task(tester.service_client(), {5, 15});
  tester.RunToCompletion(task);

  PW_TEST_ASSERT_OK(task.status());
  ASSERT_EQ(task.response_count(), 2u);
  EXPECT_EQ(task.responses()[0], 10u);
  EXPECT_EQ(task.responses()[1], 30u);
}

// DOCSTAG: [pw_rpc2-testing-stalled]
TEST(TestHarnessTest, StalledCallResumedWhenStreamFinishes) {
  TestHarness<EchoFutureService> tester;

  auto call =
      tester.RunToCompletion(tester.service_client().AccumulateClientStream());
  PW_TEST_ASSERT_OK(call);

  auto& response_fut = call->response();
  // First run stalls because the client has not finished the request stream.
  EXPECT_TRUE(tester.RunInTaskUntilStalled(response_fut).IsPending());

  PW_TEST_EXPECT_OK(
      tester.RunToCompletion(call->writer().Write(EchoRequest{.val = 42})));
  PW_TEST_EXPECT_OK(tester.RunToCompletion(call->writer().Finish()));

  auto poll = tester.RunInTaskUntilStalled(response_fut);
  ASSERT_TRUE(poll.IsReady());
  PW_TEST_ASSERT_OK(*poll);
  EXPECT_EQ((*poll)->val, 42u);
}
// DOCSTAG: [pw_rpc2-testing-stalled]

// The harness exposes both generated client flavors, regardless of which
// flavor `ServiceImpl` implements.
static_assert(std::is_same_v<TestHarness<EchoFutureService>::ServiceClient,
                             PwpbTestEcho::Client>);
static_assert(std::is_same_v<TestHarness<EchoFutureService>::PwpbClient,
                             PwpbTestEcho::Client>);
static_assert(std::is_same_v<TestHarness<EchoFutureService>::RawClient,
                             RawTestEcho::Client>);

TEST(TestHarnessTest, PwpbClient) {
  TestHarness<EchoFutureService> tester;

  RpcResult<EchoResponse> resp = tester.RunToCompletion(
      tester.pwpb_client().EchoUnary(EchoRequest{.val = 7}));
  PW_TEST_ASSERT_OK(resp);
  EXPECT_EQ(resp->val, 14u);
}

TEST(TestHarnessTest, RawClientCallsPwpbService) {
  TestHarness<EchoFutureService> tester;

  // Wire encoding of `EchoRequest{.val = 21}`.
  constexpr std::array<std::byte, 2> kRequest = {std::byte{0x08},
                                                 std::byte{21}};
  RpcResult<pw::ConstBuf> resp = tester.RunToCompletion(
      tester.raw_client().EchoUnary::Copy(pw::ConstBuf::Unowned(kRequest)));
  PW_TEST_ASSERT_OK(resp);
  // Wire encoding of `EchoResponse{.val = 42}`.
  ASSERT_EQ(resp->size(), 2u);
  EXPECT_EQ((*resp)[0], std::byte{0x08});
  EXPECT_EQ((*resp)[1], std::byte{42});
  EXPECT_EQ(tester.service().calls(), 1u);
}

TEST(TestHarnessTest, AbandonedCallTearsDownCleanly) {
  TestHarness<EchoFutureService> tester;

  auto call = tester.RunToCompletion(tester.service_client().EchoBidiStream());
  PW_TEST_ASSERT_OK(call);
  PW_TEST_EXPECT_OK(
      tester.RunToCompletion(call->writer().Write(EchoRequest{.val = 5})));
  // Leave the call open when `tester` and `call` go out of scope.
}

#if PW_NC_TEST(NotAServiceImpl)
PW_NC_EXPECT("ServiceImpl must derive from a generated pw_rpc2 Service");

struct NotAService {};

[[maybe_unused]] void BadServiceImpl() { TestHarness<NotAService> tester; }

#elif PW_NC_TEST(InvalidRunToCompletionArgument)
PW_NC_EXPECT("RunToCompletion.*requires a pw::async2::Future");

[[maybe_unused]] void BadRunToCompletion() {
  TestHarness<EchoFutureService> tester;
  tester.RunToCompletion(42);
}

#endif  // PW_NC_TEST

}  // namespace
