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
// Tests the generated `Service<Impl>` base classes. Each service implements
// every RPC in `TestEcho` either by declaring `<Method>Future` types or by
// declaring `<Method>()` member functions, using both the raw and the pwpb
// codegen. Every RPC is made with a generated `Client` and dispatched by a real
// `Server`.
//
// The negative compilation tests at the end check the errors reported for
// service implementations that do not match the generated service.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/pw_rpc2_test.pwpb.rpc2.h"
#include "pw_rpc2/pw_rpc2_test.raw.rpc2.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::test {
namespace {

using EchoRequest = ::pw::rpc2::test::pwpb::EchoRequest::Message;
using EchoResponse = ::pw::rpc2::test::pwpb::EchoResponse::Message;

// =============================================================================
// Futures that implement the RPCs
// =============================================================================

// Unary: responds with twice the request value.
class DoubleUnaryFuture : public TestFuture {
 public:
  DoubleUnaryFuture() = default;
  DoubleUnaryFuture(EchoRequest request,
                    ::pw::rpc2::UnaryWriter<EchoResponse> responder)
      : TestFuture(true), request_(request), responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    if (!finish_.is_pendable()) {
      finish_ = responder_.Finish(EchoResponse{.val = request_.val * 2});
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  EchoRequest request_;
  ::pw::rpc2::UnaryWriter<EchoResponse> responder_;
  ::pw::rpc2::WriteFuture<EchoResponse> finish_;
};
static_assert(::pw::async2::Future<DoubleUnaryFuture>);

// Server streaming: responds with the request value plus 1, 2 and 3.
class CountUpFuture : public TestFuture {
 public:
  static constexpr uint32_t kCount = 3;

  CountUpFuture() = default;
  CountUpFuture(EchoRequest request, ::pw::rpc2::Writer<EchoResponse> writer)
      : TestFuture(true), request_(request), writer_(std::move(writer)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (sent_ < kCount) {
      if (!write_.is_pendable()) {
        write_ = writer_.Write(EchoResponse{.val = request_.val + sent_ + 1});
      }
      PW_AWAIT(::pw::Status status, write_, cx);
      PW_TEST_EXPECT_OK(status);
      ++sent_;
    }
    if (!finish_.is_pendable()) {
      finish_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  EchoRequest request_;
  ::pw::rpc2::Writer<EchoResponse> writer_;
  ::pw::rpc2::WriteFuture<EchoResponse> write_;
  ::pw::rpc2::WriteFuture<> finish_;
  uint32_t sent_ = 0;
};
static_assert(::pw::async2::Future<CountUpFuture>);

// Client streaming: responds with the sum of the request values.
class SumFuture : public TestFuture {
 public:
  SumFuture() = default;
  SumFuture(::pw::rpc2::Reader<EchoRequest> reader,
            ::pw::rpc2::UnaryWriter<EchoResponse> responder)
      : TestFuture(true),
        reader_(std::move(reader)),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      if (!read_.is_pendable()) {
        read_ = reader_.Read();
      }
      PW_AWAIT(::pw::Result<EchoRequest> request, read_, cx);
      if (!request.ok()) {
        EXPECT_EQ(request.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      sum_ += request->val;
    }
    if (!finish_.is_pendable()) {
      finish_ = responder_.Finish(EchoResponse{.val = sum_});
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::Reader<EchoRequest> reader_;
  ::pw::rpc2::UnaryWriter<EchoResponse> responder_;
  ::pw::rpc2::ReadFuture<EchoRequest> read_;
  ::pw::rpc2::WriteFuture<EchoResponse> finish_;
  uint32_t sum_ = 0;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<SumFuture>);

// Bidirectional streaming: responds to each request with twice its value.
class DoubleBidiFuture : public TestFuture {
 public:
  DoubleBidiFuture() = default;
  DoubleBidiFuture(::pw::rpc2::Reader<EchoRequest> reader,
                   ::pw::rpc2::Writer<EchoResponse> writer)
      : TestFuture(true),
        reader_(std::move(reader)),
        writer_(std::move(writer)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      // Finish writing the previous response before reading the next request.
      if (write_.is_pendable()) {
        PW_AWAIT(::pw::Status status, write_, cx);
        PW_TEST_EXPECT_OK(status);
      }
      if (!read_.is_pendable()) {
        read_ = reader_.Read();
      }
      PW_AWAIT(::pw::Result<EchoRequest> request, read_, cx);
      if (!request.ok()) {
        EXPECT_EQ(request.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      write_ = writer_.Write(EchoResponse{.val = request->val * 2});
    }
    if (!finish_.is_pendable()) {
      finish_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::Reader<EchoRequest> reader_;
  ::pw::rpc2::Writer<EchoResponse> writer_;
  ::pw::rpc2::ReadFuture<EchoRequest> read_;
  ::pw::rpc2::WriteFuture<EchoResponse> write_;
  ::pw::rpc2::WriteFuture<> finish_;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<DoubleBidiFuture>);

// Raw unary: echoes the request bytes.
class RawEchoUnaryFuture : public TestFuture {
 public:
  RawEchoUnaryFuture() = default;
  RawEchoUnaryFuture(::pw::ConstBuf request,
                     ::pw::rpc2::RawUnaryWriter responder)
      : TestFuture(true),
        request_(std::move(request)),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    if (!finish_.is_pendable()) {
      finish_ = responder_.FinishCopy(request_);
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::ConstBuf request_;
  ::pw::rpc2::RawUnaryWriter responder_;
  ::pw::rpc2::WriteFuture<::pw::ConstByteSpan> finish_;
};
static_assert(::pw::async2::Future<RawEchoUnaryFuture>);

// Raw server streaming: echoes the request bytes `kCount` times.
class RawRepeatFuture : public TestFuture {
 public:
  static constexpr uint32_t kCount = 3;

  RawRepeatFuture() = default;
  RawRepeatFuture(::pw::ConstBuf request, ::pw::rpc2::RawWriter writer)
      : TestFuture(true),
        request_(std::move(request)),
        writer_(std::move(writer)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (sent_ < kCount) {
      if (!write_.is_pendable()) {
        write_ = writer_.WriteCopy(request_);
      }
      PW_AWAIT(::pw::Status status, write_, cx);
      PW_TEST_EXPECT_OK(status);
      ++sent_;
    }
    if (!finish_.is_pendable()) {
      finish_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::ConstBuf request_;
  ::pw::rpc2::RawWriter writer_;
  ::pw::rpc2::WriteFuture<::pw::ConstByteSpan> write_;
  ::pw::rpc2::WriteFuture<> finish_;
  uint32_t sent_ = 0;
};
static_assert(::pw::async2::Future<RawRepeatFuture>);

// Raw client streaming: responds with a single byte holding the number of
// requests received.
class RawCountFuture : public TestFuture {
 public:
  RawCountFuture() = default;
  RawCountFuture(::pw::rpc2::RawReader reader,
                 ::pw::rpc2::RawUnaryWriter responder)
      : TestFuture(true),
        reader_(std::move(reader)),
        responder_(std::move(responder)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      if (!read_.is_pendable()) {
        read_ = reader_.Read();
      }
      PW_AWAIT(::pw::Result<::pw::ConstBuf> request, read_, cx);
      if (!request.ok()) {
        EXPECT_EQ(request.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      ++count_;
    }
    if (!finish_.is_pendable()) {
      response_[0] = static_cast<std::byte>(count_);
      finish_ = responder_.FinishCopy(response_);
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::RawReader reader_;
  ::pw::rpc2::RawUnaryWriter responder_;
  ::pw::rpc2::RawReadFuture read_;
  ::pw::rpc2::WriteFuture<::pw::ConstByteSpan> finish_;
  uint8_t count_ = 0;
  std::byte response_[1] = {};
  bool reading_ = true;
};
static_assert(::pw::async2::Future<RawCountFuture>);

// Raw bidirectional streaming: echoes each request's bytes.
class RawEchoBidiFuture : public TestFuture {
 public:
  RawEchoBidiFuture() = default;
  RawEchoBidiFuture(::pw::rpc2::RawReader reader, ::pw::rpc2::RawWriter writer)
      : TestFuture(true),
        reader_(std::move(reader)),
        writer_(std::move(writer)) {}

  ::pw::async2::Poll<> Pend(::pw::async2::Context& cx) {
    while (reading_) {
      // `write_` refers to `message_`, so finish writing it before reading the
      // next request.
      if (write_.is_pendable()) {
        PW_AWAIT(::pw::Status status, write_, cx);
        PW_TEST_EXPECT_OK(status);
      }
      if (!read_.is_pendable()) {
        read_ = reader_.Read();
      }
      PW_AWAIT(::pw::Result<::pw::ConstBuf> request, read_, cx);
      if (!request.ok()) {
        EXPECT_EQ(request.status(), ::pw::Status::OutOfRange());
        reading_ = false;
        break;
      }
      message_ = std::move(*request);
      write_ = writer_.WriteCopy(message_);
    }
    if (!finish_.is_pendable()) {
      finish_ = writer_.Finish();
    }
    PW_AWAIT(::pw::Status status, finish_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  ::pw::rpc2::RawReader reader_;
  ::pw::rpc2::RawWriter writer_;
  ::pw::rpc2::RawReadFuture read_;
  ::pw::ConstBuf message_;
  ::pw::rpc2::WriteFuture<::pw::ConstByteSpan> write_;
  ::pw::rpc2::WriteFuture<> finish_;
  bool reading_ = true;
};
static_assert(::pw::async2::Future<RawEchoBidiFuture>);

// =============================================================================
// Services
// =============================================================================

// Implements every RPC by declaring a `<Method>Future` type, which the
// generated base class constructs from the method's arguments.
class PwpbFutureService
    : public pw_rpc2::pwpb::TestEcho::Service<PwpbFutureService> {
 public:
  using EchoUnaryFuture = DoubleUnaryFuture;
  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

// Implements every RPC with a member function (non-static or static) that
// returns a future.
class PwpbMemberService
    : public pw_rpc2::pwpb::TestEcho::Service<PwpbMemberService> {
 public:
  DoubleUnaryFuture EchoUnary(EchoRequest request,
                              ::pw::rpc2::UnaryWriter<EchoResponse> responder) {
    return DoubleUnaryFuture(request, std::move(responder));
  }

  CountUpFuture CountUpServerStream(EchoRequest request,
                                    ::pw::rpc2::Writer<EchoResponse> writer) {
    return CountUpFuture(request, std::move(writer));
  }

  static SumFuture AccumulateClientStream(
      ::pw::rpc2::Reader<EchoRequest> reader,
      ::pw::rpc2::UnaryWriter<EchoResponse> responder) {
    return SumFuture(std::move(reader), std::move(responder));
  }

  static DoubleBidiFuture EchoBidiStream(
      ::pw::rpc2::Reader<EchoRequest> reader,
      ::pw::rpc2::Writer<EchoResponse> writer) {
    return DoubleBidiFuture(std::move(reader), std::move(writer));
  }
};

// The raw counterpart of `PwpbFutureService`.
class RawFutureService
    : public pw_rpc2::raw::TestEcho::Service<RawFutureService> {
 public:
  using EchoUnaryFuture = RawEchoUnaryFuture;
  using CountUpServerStreamFuture = RawRepeatFuture;
  using AccumulateClientStreamFuture = RawCountFuture;
  using EchoBidiStreamFuture = RawEchoBidiFuture;
};

// The raw counterpart of `PwpbMemberService`.
class RawMemberService
    : public pw_rpc2::raw::TestEcho::Service<RawMemberService> {
 public:
  RawEchoUnaryFuture EchoUnary(::pw::ConstBuf request,
                               ::pw::rpc2::RawUnaryWriter responder) {
    return RawEchoUnaryFuture(std::move(request), std::move(responder));
  }

  RawRepeatFuture CountUpServerStream(::pw::ConstBuf request,
                                      ::pw::rpc2::RawWriter writer) {
    return RawRepeatFuture(std::move(request), std::move(writer));
  }

  static RawCountFuture AccumulateClientStream(
      ::pw::rpc2::RawReader reader, ::pw::rpc2::RawUnaryWriter responder) {
    return RawCountFuture(std::move(reader), std::move(responder));
  }

  static RawEchoBidiFuture EchoBidiStream(::pw::rpc2::RawReader reader,
                                          ::pw::rpc2::RawWriter writer) {
    return RawEchoBidiFuture(std::move(reader), std::move(writer));
  }
};

// Only implementations can construct the generated base classes.
static_assert(!std::is_default_constructible_v<
              pw_rpc2::pwpb::TestEcho::Service<PwpbFutureService>>);
static_assert(!std::is_default_constructible_v<
              pw_rpc2::raw::TestEcho::Service<RawFutureService>>);
static_assert(std::is_default_constructible_v<PwpbFutureService>);
static_assert(std::is_default_constructible_v<RawFutureService>);

// The generated constructor is `constexpr`, so services can be
// constant-initialized.
TEST(GeneratedService, ConstantInitialization) {
  PW_CONSTINIT static PwpbFutureService pwpb_service;
  PW_CONSTINIT static RawFutureService raw_service;
  static_cast<void>(pwpb_service);
  static_cast<void>(raw_service);
}

// =============================================================================
// Test fixture
// =============================================================================

// Serves `ServiceImpl` from a real `Server` and connects a `Client` to it over
// an in-memory connection.
template <typename ServiceImpl>
class ServiceTest : public ::testing::Test {
 protected:
  ServiceTest() : server_(allocator_, dispatcher_) {}

  void SetUp() override {
    ASSERT_EQ(server_.RegisterService(service_), ::pw::OkStatus());
    ASSERT_EQ(server_.RegisterListenerBlocking(transport_), ::pw::OkStatus());
    server_.Start();
    started_ = true;

    auto connections = MakePairedConnections(allocator_);
    transport_.ResolveAccept(connections.server());
    ::pw::Result<::pw::rpc2::Client> client =
        RunToCompletion(::pw::rpc2::Client::Connect(
            dispatcher_, allocator_, connections.client()));
    ASSERT_EQ(client.status(), ::pw::OkStatus());
    client_.emplace(std::move(*client), dispatcher_);
  }

  // A started server must be closed before it, and the service registered
  // with it, are destroyed.
  void TearDown() override {
    client_.reset();
    if (started_) {
      auto closed = server_.Close();
      transport_.ResolveAccept(::pw::Status::Cancelled());
      EXPECT_EQ(RunToCompletion(std::move(closed)), ::pw::OkStatus());
    }
  }

  const ::pw::rpc2::Client& client() { return *client_; }

  template <typename Fut>
  typename Fut::value_type RunToCompletion(Fut future) {
    return test::RunToCompletion(dispatcher_, std::move(future));
  }

  // Sends `payload` as the request of a zero-copy raw call and returns the
  // result of committing it: a response future or reader.
  template <typename ReserveFuture,
            typename Future = std::remove_reference_t<ReserveFuture>>
  auto SendRawRequest(ReserveFuture&& reservation, ::pw::ConstByteSpan payload)
      -> decltype(std::declval<typename Future::Reservation&>().Commit(0)) {
    typename Future::value_type buffer =
        RunToCompletion(std::forward<ReserveFuture>(reservation));
    if (!buffer.ok()) {
      return buffer.status();
    }
    return CommitCopy(*buffer, payload);
  }

  ::pw::allocator::test::AllocatorForTest<8192> allocator_;
  ::pw::async2::DispatcherForTest dispatcher_;
  ServiceImpl service_;
  ::pw::rpc2::Server server_;
  MockTransport transport_;
  std::optional<ScopedClient> client_;
  bool started_ = false;
};

// Checks every RPC of a pwpb service implemented with the futures above.
template <typename ServiceImpl>
class PwpbServiceTest : public ServiceTest<ServiceImpl> {
 protected:
  void TestUnary() {
    pw_rpc2::pwpb::TestEcho::Client stub(this->client());
    ::pw::Result<EchoResponse> response =
        this->RunToCompletion(stub.EchoUnary(EchoRequest{.val = 21}));
    PW_TEST_ASSERT_OK(response);
    EXPECT_EQ(response->val, 42u);
  }

  void TestServerStreaming() {
    pw_rpc2::pwpb::TestEcho::Client stub(this->client());
    ::pw::Result<::pw::rpc2::Reader<EchoResponse>> reader =
        this->RunToCompletion(stub.CountUpServerStream(EchoRequest{.val = 10}));
    PW_TEST_ASSERT_OK(reader);

    for (uint32_t expected : {11u, 12u, 13u}) {
      ::pw::Result<EchoResponse> response =
          this->RunToCompletion(reader->Read());
      PW_TEST_ASSERT_OK(response);
      EXPECT_EQ(response->val, expected);
    }
    // The server finished the stream with `OK`.
    EXPECT_EQ(this->RunToCompletion(reader->Read()).status(),
              ::pw::Status::OutOfRange());
  }

  void TestClientStreaming() {
    pw_rpc2::pwpb::TestEcho::Client stub(this->client());
    auto call = this->RunToCompletion(stub.AccumulateClientStream());
    PW_TEST_ASSERT_OK(call);

    for (uint32_t value : {10u, 20u, 30u}) {
      PW_TEST_EXPECT_OK(this->RunToCompletion(
          call->writer().Write(EchoRequest{.val = value})));
    }
    PW_TEST_EXPECT_OK(this->RunToCompletion(call->writer().Finish()));

    ::pw::Result<EchoResponse> response =
        this->RunToCompletion(std::move(*call).response());
    PW_TEST_ASSERT_OK(response);
    EXPECT_EQ(response->val, 60u);
  }

  void TestBidiStreaming() {
    pw_rpc2::pwpb::TestEcho::Client stub(this->client());
    auto call = this->RunToCompletion(stub.EchoBidiStream());
    PW_TEST_ASSERT_OK(call);

    for (uint32_t value : {5u, 15u}) {
      PW_TEST_EXPECT_OK(this->RunToCompletion(
          call->writer().Write(EchoRequest{.val = value})));
      ::pw::Result<EchoResponse> response =
          this->RunToCompletion(call->reader().Read());
      PW_TEST_ASSERT_OK(response);
      EXPECT_EQ(response->val, value * 2);
    }
    PW_TEST_EXPECT_OK(this->RunToCompletion(call->writer().Finish()));
    EXPECT_EQ(this->RunToCompletion(call->reader().Read()).status(),
              ::pw::Status::OutOfRange());
  }
};

constexpr std::byte kPayload[] = {std::byte{0xDE}, std::byte{0xAD}};
constexpr std::byte kOtherPayload[] = {std::byte{0xBE}, std::byte{0xEF}};

// Checks every RPC of a raw service implemented with the futures above.
template <typename ServiceImpl>
class RawServiceTest : public ServiceTest<ServiceImpl> {
 protected:
  void TestUnary() {
    pw_rpc2::raw::TestEcho::Client stub(this->client());
    ::pw::Result<::pw::rpc2::RawResponseFuture> response_future =
        this->SendRawRequest(stub.EchoUnary(sizeof(kPayload)), kPayload);
    PW_TEST_ASSERT_OK(response_future);

    ::pw::Result<::pw::ConstBuf> response =
        this->RunToCompletion(std::move(*response_future));
    PW_TEST_ASSERT_OK(response);
    ExpectBytes(*response, kPayload);
  }

  void TestServerStreaming() {
    pw_rpc2::raw::TestEcho::Client stub(this->client());
    ::pw::Result<::pw::rpc2::RawReader> reader = this->SendRawRequest(
        stub.CountUpServerStream(sizeof(kPayload)), kPayload);
    PW_TEST_ASSERT_OK(reader);

    for (uint32_t i = 0; i < RawRepeatFuture::kCount; ++i) {
      ::pw::Result<::pw::ConstBuf> response =
          this->RunToCompletion(reader->Read());
      PW_TEST_ASSERT_OK(response);
      ExpectBytes(*response, kPayload);
    }
    EXPECT_EQ(this->RunToCompletion(reader->Read()).status(),
              ::pw::Status::OutOfRange());
  }

  void TestClientStreaming() {
    pw_rpc2::raw::TestEcho::Client stub(this->client());
    auto call = this->RunToCompletion(stub.AccumulateClientStream());
    PW_TEST_ASSERT_OK(call);

    for (int i = 0; i < 3; ++i) {
      PW_TEST_EXPECT_OK(
          this->RunToCompletion(call->writer().WriteCopy(kPayload)));
    }
    PW_TEST_EXPECT_OK(this->RunToCompletion(call->writer().Finish()));

    ::pw::Result<::pw::ConstBuf> response =
        this->RunToCompletion(std::move(*call).response());
    PW_TEST_ASSERT_OK(response);
    constexpr std::byte kExpectedCount[] = {std::byte{3}};
    ExpectBytes(*response, kExpectedCount);
  }

  void TestBidiStreaming() {
    pw_rpc2::raw::TestEcho::Client stub(this->client());
    auto call = this->RunToCompletion(stub.EchoBidiStream());
    PW_TEST_ASSERT_OK(call);

    for (::pw::ConstByteSpan payload :
         {::pw::ConstByteSpan(kPayload), ::pw::ConstByteSpan(kOtherPayload)}) {
      PW_TEST_EXPECT_OK(
          this->RunToCompletion(call->writer().WriteCopy(payload)));
      ::pw::Result<::pw::ConstBuf> response =
          this->RunToCompletion(call->reader().Read());
      PW_TEST_ASSERT_OK(response);
      ExpectBytes(*response, payload);
    }
    PW_TEST_EXPECT_OK(this->RunToCompletion(call->writer().Finish()));
    EXPECT_EQ(this->RunToCompletion(call->reader().Read()).status(),
              ::pw::Status::OutOfRange());
  }
};

// =============================================================================
// Tests
// =============================================================================

class PwpbFutureServiceTest : public PwpbServiceTest<PwpbFutureService> {};

TEST_F(PwpbFutureServiceTest, Unary) { TestUnary(); }
TEST_F(PwpbFutureServiceTest, ServerStreaming) { TestServerStreaming(); }
TEST_F(PwpbFutureServiceTest, ClientStreaming) { TestClientStreaming(); }
TEST_F(PwpbFutureServiceTest, BidiStreaming) { TestBidiStreaming(); }

class PwpbMemberServiceTest : public PwpbServiceTest<PwpbMemberService> {};

TEST_F(PwpbMemberServiceTest, Unary) { TestUnary(); }
TEST_F(PwpbMemberServiceTest, ServerStreaming) { TestServerStreaming(); }
TEST_F(PwpbMemberServiceTest, ClientStreaming) { TestClientStreaming(); }
TEST_F(PwpbMemberServiceTest, BidiStreaming) { TestBidiStreaming(); }

class RawFutureServiceTest : public RawServiceTest<RawFutureService> {};

TEST_F(RawFutureServiceTest, Unary) { TestUnary(); }
TEST_F(RawFutureServiceTest, ServerStreaming) { TestServerStreaming(); }
TEST_F(RawFutureServiceTest, ClientStreaming) { TestClientStreaming(); }
TEST_F(RawFutureServiceTest, BidiStreaming) { TestBidiStreaming(); }

class RawMemberServiceTest : public RawServiceTest<RawMemberService> {};

TEST_F(RawMemberServiceTest, Unary) { TestUnary(); }
TEST_F(RawMemberServiceTest, ServerStreaming) { TestServerStreaming(); }
TEST_F(RawMemberServiceTest, ClientStreaming) { TestClientStreaming(); }
TEST_F(RawMemberServiceTest, BidiStreaming) { TestBidiStreaming(); }

// =============================================================================
// Negative compilation tests
// =============================================================================
//
// Each service below gets one RPC wrong and implements the others correctly.
// Each test constructs its service, since constructing it is what
// instantiates the generated base class's method table.

#if PW_NC_TEST(ServiceDeclaresNeitherMethodNorFuture)
PW_NC_EXPECT(
    "Service implementation must define either a member function named "
    "'EchoUnary' or a future type named 'EchoUnaryFuture'");

class NoEchoUnary : public pw_rpc2::pwpb::TestEcho::Service<NoEchoUnary> {
 public:
  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { NoEchoUnary service; }

#elif PW_NC_TEST(ServiceDeclaresBothMethodAndFuture)
PW_NC_EXPECT(
    "Service implementation must not define both a member function named "
    "'EchoUnary' and a future type named 'EchoUnaryFuture'");

class BothEchoUnary : public pw_rpc2::pwpb::TestEcho::Service<BothEchoUnary> {
 public:
  using EchoUnaryFuture = DoubleUnaryFuture;

  DoubleUnaryFuture EchoUnary(EchoRequest request,
                              ::pw::rpc2::UnaryWriter<EchoResponse> responder) {
    return DoubleUnaryFuture(request, std::move(responder));
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { BothEchoUnary service; }

#elif PW_NC_TEST(FutureTypeIsNotAFuture)
PW_NC_EXPECT("RPC method return type must satisfy pw::async2::Future");

struct NotAFuture {
  using value_type = void;
};

class BadFutureType : public pw_rpc2::pwpb::TestEcho::Service<BadFutureType> {
 public:
  using EchoUnaryFuture = NotAFuture;
  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { BadFutureType service; }

#elif PW_NC_TEST(FutureTypeIsNotConstructibleFromRpcArguments)
PW_NC_EXPECT("cannot be constructed from the method's arguments");

class BadFutureConstructor
    : public pw_rpc2::pwpb::TestEcho::Service<BadFutureConstructor> {
 public:
  // Takes an int rather than the request and responder.
  class EchoUnaryFuture : public TestFuture {
   public:
    EchoUnaryFuture() = default;
    explicit EchoUnaryFuture(int) {}

    ::pw::async2::Poll<> Pend(::pw::async2::Context&) { return Complete(); }
  };

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { BadFutureConstructor service; }

#elif PW_NC_TEST(MethodDoesNotReturnAFuture)
PW_NC_EXPECT("RPC method return type must satisfy pw::async2::Future");

class BadMethodReturnType
    : public pw_rpc2::pwpb::TestEcho::Service<BadMethodReturnType> {
 public:
  void EchoUnary(EchoRequest, ::pw::rpc2::UnaryWriter<EchoResponse>) {}

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { BadMethodReturnType service; }

#elif PW_NC_TEST(MethodReturnsFutureOfNonVoid)
PW_NC_EXPECT("value_type must be void");

// RPC methods produce their result by writing it, so the future they return
// must not produce a value.
class NonVoidMethod : public pw_rpc2::pwpb::TestEcho::Service<NonVoidMethod> {
 public:
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  ::pw::async2::Coro<int> EchoUnary(::pw::async2::CoroContext,
                                    EchoRequest,
                                    ::pw::rpc2::UnaryWriter<EchoResponse>);
#else
  struct IntFuture {
    using value_type = int;
    bool is_pendable() const { return false; }
    bool is_complete() const { return true; }
    ::pw::async2::Poll<int> Pend(::pw::async2::Context&) {
      return ::pw::async2::Ready(0);
    }
  };

  IntFuture EchoUnary(EchoRequest, ::pw::rpc2::UnaryWriter<EchoResponse>) {
    return IntFuture();
  }
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { NonVoidMethod service; }

#elif PW_NC_TEST(MethodHasUnrecognizedSignature)
PW_NC_EXPECT("Invalid RPC method signature");

class BadMethodSignature
    : public pw_rpc2::pwpb::TestEcho::Service<BadMethodSignature> {
 public:
  // Missing the responder.
  DoubleUnaryFuture EchoUnary(EchoRequest) { return DoubleUnaryFuture(); }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { BadMethodSignature service; }

#elif PW_NC_TEST(CoroContextIsNotTheFirstParameter)
PW_NC_EXPECT("Invalid RPC method signature");

// A coroutine method's `CoroContext` must come before the RPC's arguments.
// Declaring the coroutine is enough to classify its signature.
class MisplacedCoroContext
    : public pw_rpc2::pwpb::TestEcho::Service<MisplacedCoroContext> {
 public:
  ::pw::async2::Coro<void> EchoUnary(EchoRequest,
                                     ::pw::async2::CoroContext,
                                     ::pw::rpc2::UnaryWriter<EchoResponse>);

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { MisplacedCoroContext service; }

#elif PW_NC_TEST(MethodImplementsTheWrongRpcType)
PW_NC_EXPECT("Method signature does not match the expected RPC method type");

class WrongMethodType
    : public pw_rpc2::pwpb::TestEcho::Service<WrongMethodType> {
 public:
  // EchoUnary is a unary RPC, but this takes a Writer.
  CountUpFuture EchoUnary(EchoRequest request,
                          ::pw::rpc2::Writer<EchoResponse> writer) {
    return CountUpFuture(request, std::move(writer));
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { WrongMethodType service; }

#elif PW_NC_TEST(RawMethodImplementsTheWrongRpcType)
PW_NC_EXPECT("Method signature does not match the expected RPC method type");

class WrongRawMethodType
    : public pw_rpc2::raw::TestEcho::Service<WrongRawMethodType> {
 public:
  // EchoUnary is a unary RPC, but this takes a RawWriter.
  RawRepeatFuture EchoUnary(::pw::ConstBuf request,
                            ::pw::rpc2::RawWriter writer) {
    return RawRepeatFuture(std::move(request), std::move(writer));
  }

  using CountUpServerStreamFuture = RawRepeatFuture;
  using AccumulateClientStreamFuture = RawCountFuture;
  using EchoBidiStreamFuture = RawEchoBidiFuture;
};

[[maybe_unused]] void ConstructService() { WrongRawMethodType service; }

#elif PW_NC_TEST(TypedMethodInRawService)
PW_NC_EXPECT("Method request type does not match the generated service");

// A `raw::` service base class only accepts raw methods, since the raw
// generated header does not know the protobuf message types. Services that
// mix raw and typed methods must inherit from the typed (e.g. `pwpb::`) base
// class, which accepts both.
class TypedMethodInRawService
    : public pw_rpc2::raw::TestEcho::Service<TypedMethodInRawService> {
 public:
  DoubleUnaryFuture EchoUnary(EchoRequest request,
                              ::pw::rpc2::UnaryWriter<EchoResponse> responder) {
    return DoubleUnaryFuture(request, std::move(responder));
  }

  using CountUpServerStreamFuture = RawRepeatFuture;
  using AccumulateClientStreamFuture = RawCountFuture;
  using EchoBidiStreamFuture = RawEchoBidiFuture;
};

[[maybe_unused]] void ConstructService() { TypedMethodInRawService service; }

#elif PW_NC_TEST(MethodHasWrongRequestType)
PW_NC_EXPECT("Method request type does not match the generated service");

class WrongRequestType
    : public pw_rpc2::pwpb::TestEcho::Service<WrongRequestType> {
 public:
  // Takes an EchoResponse where an EchoRequest is expected.
  DoubleUnaryFuture EchoUnary(EchoResponse,
                              ::pw::rpc2::UnaryWriter<EchoResponse>) {
    return DoubleUnaryFuture();
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { WrongRequestType service; }

#elif PW_NC_TEST(MethodHasWrongResponseType)
PW_NC_EXPECT("Method response type does not match the generated service");

class WrongResponseType
    : public pw_rpc2::pwpb::TestEcho::Service<WrongResponseType> {
 public:
  // Responds with an EchoRequest where an EchoResponse is expected.
  DoubleUnaryFuture EchoUnary(EchoRequest,
                              ::pw::rpc2::UnaryWriter<EchoRequest>) {
    return DoubleUnaryFuture();
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { WrongResponseType service; }

#elif PW_NC_TEST(CoroutineHasWrongMessageTypes)
PW_NC_EXPECT("Method request type does not match the generated service");

// Declaring the coroutine is enough to classify its signature. Without
// coroutine support, an equivalent future-returning method stands in.
class CoroWrongTypes : public pw_rpc2::pwpb::TestEcho::Service<CoroWrongTypes> {
 public:
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  ::pw::async2::Coro<void> EchoUnary(::pw::async2::CoroContext,
                                     EchoResponse,
                                     ::pw::rpc2::UnaryWriter<EchoRequest>);
#else
  DoubleUnaryFuture EchoUnary(EchoResponse,
                              ::pw::rpc2::UnaryWriter<EchoRequest>);
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { CoroWrongTypes service; }

#elif PW_NC_TEST(MethodMixesRawRequestWithTypedResponder)
PW_NC_EXPECT("Method request type does not match the generated service");

// A method is only treated as raw when *both* its request and response are
// `pw::ConstBuf`. Mixing a raw request with a typed responder is an error
// rather than a partially-raw method.
class HalfRawRequest : public pw_rpc2::pwpb::TestEcho::Service<HalfRawRequest> {
 public:
  DoubleUnaryFuture EchoUnary(::pw::ConstBuf,
                              ::pw::rpc2::UnaryWriter<EchoResponse>) {
    return DoubleUnaryFuture();
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { HalfRawRequest service; }

#elif PW_NC_TEST(MethodMixesTypedRequestWithRawResponder)
PW_NC_EXPECT("Method response type does not match the generated service");

// The mirror image of `MethodMixesRawRequestWithTypedResponder`.
class HalfRawResponse
    : public pw_rpc2::pwpb::TestEcho::Service<HalfRawResponse> {
 public:
  RawEchoUnaryFuture EchoUnary(EchoRequest, ::pw::rpc2::RawUnaryWriter) {
    return RawEchoUnaryFuture();
  }

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { HalfRawResponse service; }

#elif PW_NC_TEST(FutureTypeHasWrongMessageTypes)
PW_NC_EXPECT("cannot be constructed from the method's arguments");

// Unlike `FutureTypeIsNotConstructibleFromRpcArguments`, this future has a
// plausible-looking RPC constructor; only the message types are wrong. It
// matches neither the typed nor the raw API, so it is rejected.
class FutureWithWrongTypes
    : public pw_rpc2::pwpb::TestEcho::Service<FutureWithWrongTypes> {
 public:
  class EchoUnaryFuture : public TestFuture {
   public:
    EchoUnaryFuture() = default;
    EchoUnaryFuture(FutureWithWrongTypes&,
                    EchoResponse,
                    ::pw::rpc2::UnaryWriter<EchoRequest>) {}

    ::pw::async2::Poll<> Pend(::pw::async2::Context&) { return Complete(); }
  };

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { FutureWithWrongTypes service; }

#elif PW_NC_TEST(FutureTypeIsForTheWrongMethodType)
PW_NC_EXPECT("cannot be constructed from the method's arguments");

// CountUpServerStream is server streaming, so its future must accept a
// `Writer`, not a `UnaryWriter`.
class FutureWithWrongMethodShape
    : public pw_rpc2::pwpb::TestEcho::Service<FutureWithWrongMethodShape> {
 public:
  using EchoUnaryFuture = DoubleUnaryFuture;
  using CountUpServerStreamFuture = DoubleUnaryFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { FutureWithWrongMethodShape service; }

#elif PW_NC_TEST(FutureTypeAcceptsBothTypedAndRawArguments)
PW_NC_EXPECT(
    "constructible from both the typed protobuf arguments and the raw");

// A `<Method>Future` on a typed service must not be constructible from both
// the typed protobuf arguments and the raw (`pw::ConstBuf`) arguments, since
// that makes the intended encoding ambiguous.
class AmbiguousFutureService
    : public pw_rpc2::pwpb::TestEcho::Service<AmbiguousFutureService> {
 public:
  class EchoUnaryFuture : public TestFuture {
   public:
    EchoUnaryFuture() = default;
    template <typename Request, typename Responder>
    EchoUnaryFuture(Request&&, Responder&&) : TestFuture(true) {}

    ::pw::async2::Poll<> Pend(::pw::async2::Context&) { return Complete(); }
  };

  using CountUpServerStreamFuture = CountUpFuture;
  using AccumulateClientStreamFuture = SumFuture;
  using EchoBidiStreamFuture = DoubleBidiFuture;
};

[[maybe_unused]] void ConstructService() { AmbiguousFutureService service; }

#endif  // PW_NC_TEST

}  // namespace
}  // namespace pw::rpc2::test
