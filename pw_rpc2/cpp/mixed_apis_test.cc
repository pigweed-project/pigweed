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
//
// Verifies that every combination of protobuf encoding (raw / pwpb) and
// implementation style (`<Method>Future` type / factory function / coroutine)
// works when mixed within a single service. Each RPC is made with the generated
// pwpb client and dispatched to the service by a real `Server`.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/mixed_apis.pwpb.rpc2.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::mixed_apis {
namespace {

using MixedRequest = ::pw::rpc2::mixed_apis::pwpb::MixedRequest::Message;
using MixedResponse = ::pw::rpc2::mixed_apis::pwpb::MixedResponse::Message;

class MixedApiService;

// =============================================================================
// Futures
// =============================================================================

class DoubleValueFuture : public test::TestFuture {
 public:
  DoubleValueFuture() = default;
  DoubleValueFuture(MixedRequest request,
                    ::pw::rpc2::UnaryWriter<MixedResponse> responder)
      : test::TestFuture(true),
        request_(request),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    if (!write_fut_.is_pendable()) {
      write_fut_ =
          responder_.Finish(MixedResponse{.value = request_.value * 2});
    }
    PW_AWAIT(::pw::Status status, write_fut_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  MixedRequest request_;
  ::pw::rpc2::UnaryWriter<MixedResponse> responder_;
  ::pw::rpc2::WriteFuture<MixedResponse> write_fut_;
};
static_assert(::pw::async2::Future<DoubleValueFuture>);

class RawEchoFuture : public test::TestFuture {
 public:
  RawEchoFuture() = default;
  RawEchoFuture(::pw::ConstBuf request, ::pw::rpc2::RawUnaryWriter responder)
      : test::TestFuture(true),
        request_(std::move(request)),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    if (!reserve_fut_.is_pendable()) {
      reserve_fut_ = responder_.ReserveFinish(request_.size());
    }
    PW_AWAIT(auto reservation, reserve_fut_, cx);
    PW_TEST_EXPECT_OK(reservation.status());
    if (reservation.ok()) {
      PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, request_));
    }
    return Complete();
  }

 private:
  ::pw::ConstBuf request_;
  ::pw::rpc2::RawUnaryWriter responder_;
  ::pw::rpc2::ReserveWriteFuture reserve_fut_;
};
static_assert(::pw::async2::Future<RawEchoFuture>);

class CountUpFuture : public test::TestFuture {
 public:
  static constexpr uint32_t kCount = 3;

  CountUpFuture() = default;
  CountUpFuture(MixedApiService& service,
                MixedRequest request,
                ::pw::rpc2::Writer<MixedResponse> writer);

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (sent_ < kCount) {
      if (!write_fut_.is_pendable()) {
        write_fut_ =
            writer_.Write(MixedResponse{.value = request_.value + sent_ + 1});
      }
      PW_AWAIT(::pw::Status status, write_fut_, cx);
      PW_TEST_EXPECT_OK(status);
      ++sent_;
    }
    if (!close_fut_.is_pendable()) {
      close_fut_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, close_fut_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  MixedRequest request_;
  ::pw::rpc2::Writer<MixedResponse> writer_;
  ::pw::rpc2::WriteFuture<MixedResponse> write_fut_;
  ::pw::rpc2::WriteFuture<> close_fut_;
  uint32_t sent_ = 0;
};
static_assert(::pw::async2::Future<CountUpFuture>);

class RawCountRequestsFuture : public test::TestFuture {
 public:
  RawCountRequestsFuture() = default;
  RawCountRequestsFuture(::pw::rpc2::RawReader reader,
                         ::pw::rpc2::RawUnaryWriter responder)
      : test::TestFuture(true),
        reader_(std::move(reader)),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      if (!read_fut_.is_pendable()) {
        read_fut_ = reader_.Read();
      }
      PW_AWAIT(auto message, read_fut_, cx);
      if (!message.ok()) {
        EXPECT_EQ(message.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      ++count_;
    }
    if (!reserve_fut_.is_pendable()) {
      reserve_fut_ = responder_.ReserveFinish(kEncodedSize);
    }
    PW_AWAIT(auto reservation, reserve_fut_, cx);
    PW_TEST_EXPECT_OK(reservation.status());
    if (reservation.ok()) {
      // Hand-encode `MixedResponse{.value = count_}`: field 1, varint.
      PW_CHECK_UINT_LT(count_, 128u);
      const std::byte encoded[kEncodedSize] = {std::byte{0x08},
                                               static_cast<std::byte>(count_)};
      PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, encoded));
    }
    return Complete();
  }

 private:
  static constexpr size_t kEncodedSize = 2;

  ::pw::rpc2::RawReader reader_;
  ::pw::rpc2::RawUnaryWriter responder_;
  ::pw::rpc2::RawReadFuture read_fut_;
  ::pw::rpc2::ReserveWriteFuture reserve_fut_;
  uint32_t count_ = 0;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<RawCountRequestsFuture>);

class EchoBidiFuture : public test::TestFuture {
 public:
  EchoBidiFuture() = default;
  EchoBidiFuture(::pw::rpc2::Reader<MixedRequest> reader,
                 ::pw::rpc2::Writer<MixedResponse> writer)
      : test::TestFuture(true),
        reader_(std::move(reader)),
        writer_(std::move(writer)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      if (write_fut_.is_pendable()) {
        PW_AWAIT(::pw::Status status, write_fut_, cx);
        PW_TEST_EXPECT_OK(status);
      }
      if (!read_fut_.is_pendable()) {
        read_fut_ = reader_.Read();
      }
      PW_AWAIT(auto message, read_fut_, cx);
      if (!message.ok()) {
        EXPECT_EQ(message.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      write_fut_ = writer_.Write(MixedResponse{.value = message->value});
    }
    if (!close_fut_.is_pendable()) {
      close_fut_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, close_fut_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::Reader<MixedRequest> reader_;
  ::pw::rpc2::Writer<MixedResponse> writer_;
  ::pw::rpc2::ReadFuture<MixedRequest> read_fut_;
  ::pw::rpc2::WriteFuture<MixedResponse> write_fut_;
  ::pw::rpc2::WriteFuture<> close_fut_;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<EchoBidiFuture>);

class RawEchoBidiFuture : public test::TestFuture {
 public:
  RawEchoBidiFuture() = default;
  RawEchoBidiFuture(MixedApiService& service,
                    ::pw::rpc2::RawReader reader,
                    ::pw::rpc2::RawWriter writer);

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      if (reserve_fut_.is_pendable()) {
        PW_AWAIT(auto reservation, reserve_fut_, cx);
        PW_TEST_EXPECT_OK(reservation.status());
        if (reservation.ok()) {
          PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, pending_));
        }
      }
      if (!read_fut_.is_pendable()) {
        read_fut_ = reader_.Read();
      }
      PW_AWAIT(auto message, read_fut_, cx);
      if (!message.ok()) {
        EXPECT_EQ(message.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      pending_ = std::move(*message);
      reserve_fut_ = writer_.ReserveWrite(pending_.size());
    }
    if (!close_fut_.is_pendable()) {
      close_fut_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, close_fut_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::RawReader reader_;
  ::pw::rpc2::RawWriter writer_;
  ::pw::rpc2::RawReadFuture read_fut_;
  ::pw::rpc2::ReserveWriteFuture reserve_fut_;
  ::pw::rpc2::WriteFuture<> close_fut_;
  ::pw::ConstBuf pending_;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<RawEchoBidiFuture>);

// =============================================================================
// Service
// =============================================================================

class MixedApiService
    : public pw_rpc2::pwpb::MixedApi::Service<MixedApiService> {
 public:
  class PwpbFutureUnaryFuture : public test::TestFuture {
   public:
    PwpbFutureUnaryFuture() = default;
    PwpbFutureUnaryFuture(MixedApiService& service,
                          MixedRequest request,
                          ::pw::rpc2::UnaryWriter<MixedResponse> responder)
        : test::TestFuture(true),
          service_(&service),
          request_(request),
          responder_(std::move(responder)) {
      ++service.future_calls_;
    }

    ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
      if (!write_fut_.is_pendable()) {
        write_fut_ = responder_.Finish(
            MixedResponse{.value = request_.value * service_->multiplier_});
      }
      PW_AWAIT(::pw::Status status, write_fut_, cx);
      PW_TEST_EXPECT_OK(status);
      return Complete();
    }

   private:
    MixedApiService* service_ = nullptr;
    MixedRequest request_;
    ::pw::rpc2::UnaryWriter<MixedResponse> responder_;
    ::pw::rpc2::WriteFuture<MixedResponse> write_fut_;
  };

  using RawFutureUnaryFuture = RawEchoFuture;
  using PwpbFutureServerStreamFuture = CountUpFuture;
  using RawFutureBidiStreamFuture = RawEchoBidiFuture;

  DoubleValueFuture PwpbFactoryUnary(
      MixedRequest request, ::pw::rpc2::UnaryWriter<MixedResponse> responder) {
    ++factory_calls_;
    return DoubleValueFuture(request, std::move(responder));
  }

  static RawEchoFuture RawFactoryUnary(::pw::ConstBuf request,
                                       ::pw::rpc2::RawUnaryWriter responder) {
    return RawEchoFuture(std::move(request), std::move(responder));
  }

  static EchoBidiFuture PwpbFactoryBidiStream(
      ::pw::rpc2::Reader<MixedRequest> reader,
      ::pw::rpc2::Writer<MixedResponse> writer) {
    return EchoBidiFuture(std::move(reader), std::move(writer));
  }

  RawCountRequestsFuture RawFactoryClientStream(
      ::pw::rpc2::RawReader reader, ::pw::rpc2::RawUnaryWriter responder) {
    ++factory_calls_;
    return RawCountRequestsFuture(std::move(reader), std::move(responder));
  }

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  ::pw::async2::Coro<void> PwpbCoroUnary(
      ::pw::async2::CoroContext,
      MixedRequest request,
      ::pw::rpc2::UnaryWriter<MixedResponse> responder) {
    ++coro_calls_;
    ::pw::Status status =
        co_await responder.Finish(MixedResponse{.value = request.value * 2});
    PW_TEST_EXPECT_OK(status);
  }

  static ::pw::async2::Coro<void> RawCoroUnary(
      ::pw::async2::CoroContext,
      ::pw::ConstBuf request,
      ::pw::rpc2::RawUnaryWriter responder) {
    auto reservation = co_await responder.ReserveFinish(request.size());
    PW_TEST_EXPECT_OK(reservation.status());
    if (reservation.ok()) {
      PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, request));
    }
  }

  ::pw::async2::Coro<void> RawCoroServerStream(::pw::async2::CoroContext,
                                               ::pw::ConstBuf request,
                                               ::pw::rpc2::RawWriter writer) {
    ++coro_calls_;
    for (uint32_t i = 0; i < CountUpFuture::kCount; ++i) {
      auto reservation = co_await writer.ReserveWrite(request.size());
      PW_TEST_EXPECT_OK(reservation.status());
      if (reservation.ok()) {
        PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, request));
      }
    }
    ::pw::Status status = co_await writer.Finish();
    PW_TEST_EXPECT_OK(status);
  }

  static ::pw::async2::Coro<void> PwpbCoroClientStream(
      ::pw::async2::CoroContext,
      ::pw::rpc2::Reader<MixedRequest> reader,
      ::pw::rpc2::UnaryWriter<MixedResponse> responder) {
    uint32_t sum = 0;
    while (true) {
      auto message = co_await reader.Read();
      if (!message.ok()) {
        EXPECT_EQ(message.status(), ::pw::Status::OutOfRange());
        break;
      }
      sum += message->value;
    }
    ::pw::Status status =
        co_await responder.Finish(MixedResponse{.value = sum});
    PW_TEST_EXPECT_OK(status);
  }
#else
  using PwpbCoroUnaryFuture = DoubleValueFuture;
  using RawCoroUnaryFuture = RawEchoFuture;
  using RawCoroServerStreamFuture = CountUpFuture;
  using PwpbCoroClientStreamFuture = RawCountRequestsFuture;
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  uint32_t future_calls() const { return future_calls_; }
  uint32_t factory_calls() const { return factory_calls_; }
  uint32_t coro_calls() const { return coro_calls_; }

 private:
  friend class CountUpFuture;
  friend class RawEchoBidiFuture;

  uint32_t future_calls_ = 0;
  uint32_t factory_calls_ = 0;
  uint32_t coro_calls_ = 0;
  uint32_t multiplier_ = 2;
};
static_assert(::pw::async2::Future<MixedApiService::PwpbFutureUnaryFuture>);

CountUpFuture::CountUpFuture(MixedApiService& service,
                             MixedRequest request,
                             ::pw::rpc2::Writer<MixedResponse> writer)
    : test::TestFuture(true), request_(request), writer_(std::move(writer)) {
  ++service.future_calls_;
}

RawEchoBidiFuture::RawEchoBidiFuture(MixedApiService& service,
                                     ::pw::rpc2::RawReader reader,
                                     ::pw::rpc2::RawWriter writer)
    : test::TestFuture(true),
      reader_(std::move(reader)),
      writer_(std::move(writer)) {
  ++service.future_calls_;
}

// =============================================================================
// Test fixture
// =============================================================================

class MixedApisTest : public ::testing::Test {
 protected:
  MixedApisTest() : server_(allocator_, dispatcher_) {}

  void SetUp() override {
    ASSERT_EQ(server_.RegisterService(service_), ::pw::OkStatus());
    ASSERT_EQ(server_.RegisterListenerBlocking(transport_), ::pw::OkStatus());
    server_.Start();
    started_ = true;

    auto connections = ::pw::rpc2::test::MakePairedConnections(allocator_);
    server_connection_ = connections.server_raw();
    transport_.ResolveAccept(connections.server());
    ::pw::Result<::pw::rpc2::Client> client =
        RunToCompletion(::pw::rpc2::Client::Connect(
            dispatcher_, allocator_, connections.client()));
    ASSERT_EQ(client.status(), ::pw::OkStatus());
    client_.emplace(std::move(*client), dispatcher_);
  }

  void TearDown() override {
    client_.reset();
    if (started_) {
      auto closed = server_.Close();
      transport_.ResolveAccept(::pw::Status::Cancelled());
      EXPECT_EQ(RunToCompletion(std::move(closed)), ::pw::OkStatus());
    }
  }

  pw_rpc2::pwpb::MixedApi::Client stub() {
    return pw_rpc2::pwpb::MixedApi::Client(*client_);
  }

  template <typename Fut>
  typename Fut::value_type RunToCompletion(Fut future) {
    return test::RunToCompletion(dispatcher_, std::move(future));
  }

  // Checks that `reader` yields exactly `expected`, after which the server
  // finished the stream with `OK`.
  void ExpectStream(::pw::rpc2::Reader<MixedResponse>& reader,
                    std::initializer_list<uint32_t> expected) {
    for (uint32_t value : expected) {
      ::pw::Result<MixedResponse> response = RunToCompletion(reader.Read());
      PW_TEST_ASSERT_OK(response);
      EXPECT_EQ(response->value, value);
    }
    EXPECT_EQ(RunToCompletion(reader.Read()).status(),
              ::pw::Status::OutOfRange());
  }

  // Streams `values` to a client-streaming RPC and returns its response.
  ::pw::Result<MixedResponse> ClientStream(
      ::pw::rpc2::ClientStreamFuture<MixedRequest, MixedResponse> future,
      std::initializer_list<uint32_t> values) {
    auto call = RunToCompletion(std::move(future));
    PW_TRY(call.status());
    for (uint32_t value : values) {
      PW_TRY(
          RunToCompletion(call->writer().Write(MixedRequest{.value = value})));
    }
    PW_TRY(RunToCompletion(call->writer().Finish()));
    return RunToCompletion(std::move(*call).response());
  }

  // Checks that a bidirectional echo RPC returns every request, including
  // requests that arrive while the server is waiting to write a response.
  void ExpectBidiEcho(
      ::pw::rpc2::BidiStreamFuture<MixedRequest, MixedResponse> future) {
    auto call = RunToCompletion(std::move(future));
    PW_TEST_ASSERT_OK(call);

    server_connection_->SetBlockReserveWrite(true);
    for (uint32_t value : {4u, 9u}) {
      PW_TEST_EXPECT_OK(
          RunToCompletion(call->writer().Write(MixedRequest{.value = value})));
    }
    server_connection_->SetBlockReserveWrite(false);

    PW_TEST_EXPECT_OK(RunToCompletion(call->writer().Finish()));
    ExpectStream(call->reader(), {4u, 9u});
  }

  // Checks how many times each implementation style was invoked.
  void ExpectCalls(uint32_t future, uint32_t factory, uint32_t coro) {
    EXPECT_EQ(service_.future_calls(), future);
    EXPECT_EQ(service_.factory_calls(), factory);
    EXPECT_EQ(service_.coro_calls(), coro);
  }

  ::pw::allocator::test::AllocatorForTest<8192> allocator_;
  ::pw::async2::DispatcherForTest dispatcher_;
  MixedApiService service_;
  ::pw::rpc2::Server server_;
  ::pw::rpc2::test::MockTransport transport_;
  ::pw::rpc2::test::PairedConnection* server_connection_ = nullptr;
  std::optional<::pw::rpc2::test::ScopedClient> client_;
  bool started_ = false;
};

// =============================================================================
// Unary: three styles x two encodings
// =============================================================================

TEST_F(MixedApisTest, PwpbFutureUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().PwpbFutureUnary(MixedRequest{.value = 21}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 42u);
  ExpectCalls(/*future=*/1, /*factory=*/0, /*coro=*/0);
}

TEST_F(MixedApisTest, PwpbFactoryUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().PwpbFactoryUnary(MixedRequest{.value = 21}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 42u);
  ExpectCalls(/*future=*/0, /*factory=*/1, /*coro=*/0);
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
TEST_F(MixedApisTest, PwpbCoroUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().PwpbCoroUnary(MixedRequest{.value = 21}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 42u);
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/1);
}
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// The `<Method>Future` for this RPC is written against the raw API even
// though the service is generated by the pwpb codegen.
TEST_F(MixedApisTest, RawFutureUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().RawFutureUnary(MixedRequest{.value = 7}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 7u);
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/0);
}

TEST_F(MixedApisTest, RawFactoryUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().RawFactoryUnary(MixedRequest{.value = 7}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 7u);
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/0);
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
TEST_F(MixedApisTest, RawCoroUnary) {
  ::pw::Result<MixedResponse> response =
      RunToCompletion(stub().RawCoroUnary(MixedRequest{.value = 7}));
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 7u);
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/0);
}
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// =============================================================================
// Server streaming
// =============================================================================

TEST_F(MixedApisTest, PwpbFutureServerStream) {
  auto reader =
      RunToCompletion(stub().PwpbFutureServerStream(MixedRequest{.value = 10}));
  PW_TEST_ASSERT_OK(reader);
  ExpectStream(*reader, {11u, 12u, 13u});
  ExpectCalls(/*future=*/1, /*factory=*/0, /*coro=*/0);
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
TEST_F(MixedApisTest, RawCoroServerStream) {
  auto reader =
      RunToCompletion(stub().RawCoroServerStream(MixedRequest{.value = 5}));
  PW_TEST_ASSERT_OK(reader);
  ExpectStream(*reader, {5u, 5u, 5u});
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/1);
}
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// =============================================================================
// Client streaming
// =============================================================================

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
TEST_F(MixedApisTest, PwpbCoroClientStream) {
  ::pw::Result<MixedResponse> response =
      ClientStream(stub().PwpbCoroClientStream(), {1u, 2u, 3u});
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 6u);
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/0);
}
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

TEST_F(MixedApisTest, RawFactoryClientStream) {
  ::pw::Result<MixedResponse> response =
      ClientStream(stub().RawFactoryClientStream(), {1u, 2u});
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->value, 2u);
  ExpectCalls(/*future=*/0, /*factory=*/1, /*coro=*/0);
}

// =============================================================================
// Bidirectional streaming
// =============================================================================

TEST_F(MixedApisTest, PwpbFactoryBidiStream) {
  ExpectBidiEcho(stub().PwpbFactoryBidiStream());
  ExpectCalls(/*future=*/0, /*factory=*/0, /*coro=*/0);
}

TEST_F(MixedApisTest, RawFutureBidiStream) {
  ExpectBidiEcho(stub().RawFutureBidiStream());
  ExpectCalls(/*future=*/1, /*factory=*/0, /*coro=*/0);
}

}  // namespace
}  // namespace pw::rpc2::mixed_apis
