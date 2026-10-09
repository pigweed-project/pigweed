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

// End-to-end tests: generated clients talk to generated services through a
// real `Server` over in-memory `PairedConnection` links.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/future_task.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/pw_rpc2_test.pwpb.rpc2.h"
#include "pw_rpc2/pw_rpc2_test.raw.rpc2.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_rpc2/writer.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::rpc2 {
namespace {

using RawClient = test::pw_rpc2::raw::TestEcho::Client;
using PwpbClient = test::pw_rpc2::pwpb::TestEcho::Client;
using EchoRequest = test::pwpb::EchoRequest::Message;
using EchoResponse = test::pwpb::EchoResponse::Message;

constexpr size_t kMaxPayloadSize = 64;
using Payload = Vector<std::byte, kMaxPayloadSize>;

// Returns `size` bytes of a pattern that differs for each `seed`.
Payload MakePayload(size_t size, uint32_t seed) {
  Payload payload;
  for (size_t i = 0; i < size; ++i) {
    payload.push_back(static_cast<std::byte>((seed * 31 + i * 7) ^ 0x5A));
  }
  return payload;
}

// Makes a raw `EchoUnary` call with the zero-copy API: awaits the request
// reservation, copies `request` into it, commits it, and awaits the response.
// `request` must outlive the future.
class RawEchoFuture {
 public:
  using value_type = Result<ConstBuf>;

  RawEchoFuture() = default;
  RawEchoFuture(RawClient client, ConstByteSpan request)
      : client_(std::move(client)), request_(request), state_(kNotStarted) {}

  bool is_pendable() const { return state_ != kEmpty && state_ != kComplete; }
  bool is_complete() const { return state_ == kComplete; }

  async2::Poll<value_type> Pend(async2::Context& cx) {
    if (state_ == kNotStarted) {
      req_fut_ = client_.EchoUnary(request_.size());
      state_ = kSendingRequest;
    }
    if (state_ == kSendingRequest) {
      PW_AWAIT(Result<RawUnaryReservation> reservation, req_fut_, cx);
      Result<RawResponseFuture> response_future =
          test::CommitCopy(std::move(reservation), request_);
      if (!response_future.ok()) {
        state_ = kComplete;
        return async2::Ready(value_type(response_future.status()));
      }
      resp_fut_ = std::move(*response_future);
      state_ = kAwaitingResponse;
    }
    PW_AWAIT(Result<ConstBuf> response, resp_fut_, cx);
    state_ = kComplete;
    return async2::Ready(std::move(response));
  }

 private:
  enum State {
    kEmpty,
    kNotStarted,
    kSendingRequest,
    kAwaitingResponse,
    kComplete
  };

  RawClient client_;
  ConstByteSpan request_;
  RawUnaryReserveFuture req_fut_;
  RawResponseFuture resp_fut_;
  State state_ = kEmpty;
};

static_assert(async2::Future<RawEchoFuture>);

// Remembers the first error a test service hit, so that tests can check
// whether (and why) a method failed rather than having it swallowed.
class ErrorRecorder {
 public:
  void Record(Status status) {
    if (first_error_.ok()) {
      first_error_ = status;
    }
  }

  // Returns the first recorded error, or `OK`, and clears it.
  Status TakeError() { return std::exchange(first_error_, OkStatus()); }

 private:
  Status first_error_;
};

// Base for the test services' method futures. A default-constructed future is
// neither pendable nor complete.
class TestMethodFuture : public test::TestFuture {
 protected:
  constexpr TestMethodFuture() = default;
  explicit TestMethodFuture(ErrorRecorder& recorder)
      : test::TestFuture(true), recorder_(&recorder) {}

  // Completes the method, recording `status` with the service if it failed.
  async2::Poll<> Complete(Status status) {
    if (!status.ok()) {
      recorder_->Record(status);
    }
    return test::TestFuture::Complete();
  }

 private:
  ErrorRecorder* recorder_ = nullptr;
};

// Raw `CountUpServerStream` streams this many bytes per message.
constexpr size_t kStreamMessageSize = 16;

// The `index`th message of a raw `CountUpServerStream`.
Payload StreamMessage(size_t index) {
  return Payload(kStreamMessageSize, static_cast<std::byte>(index));
}

// Raw `AccumulateClientStream` response.
struct SumStreamResponse {
  uint32_t messages;
  uint32_t byte_sum;
};

class RawTestService
    : public test::pw_rpc2::raw::TestEcho::Service<RawTestService>,
      public ErrorRecorder {
 public:
  // How many `EchoUnary` calls the service has received.
  size_t unary_echo_calls() const { return unary_echo_calls_; }

  // Responds with the request.
  class EchoUnaryFuture : public TestMethodFuture {
   public:
    EchoUnaryFuture() = default;
    EchoUnaryFuture(RawTestService& service,
                    ConstBuf request,
                    RawUnaryWriter responder)
        : TestMethodFuture(service),
          request_(std::move(request)),
          responder_(std::move(responder)) {
      service.unary_echo_calls_ += 1;
    }

    async2::Poll<> Pend(async2::Context& cx) {
      if (!finish_.is_pendable()) {
        finish_ = responder_.FinishCopy(std::move(request_));
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    ConstBuf request_;
    RawUnaryWriter responder_;
    WriteFuture<ConstBuf> finish_;
  };

  // Streams `StreamMessage(0)` through `StreamMessage(n - 1)`, where `n` is
  // the first byte of the request.
  class CountUpServerStreamFuture : public TestMethodFuture {
   public:
    CountUpServerStreamFuture() = default;
    CountUpServerStreamFuture(RawTestService& service,
                              ConstBuf request,
                              RawWriter writer)
        : TestMethodFuture(service),
          count_(request.empty() ? 0u : static_cast<uint8_t>(request[0])),
          writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (sent_ < count_) {
        if (!write_.is_pendable()) {
          message_ = StreamMessage(sent_);
          write_ = writer_.WriteCopy(ConstBuf::Unowned(message_));
        }
        PW_AWAIT(Status status, write_, cx);
        if (!status.ok()) {
          return Complete(status);
        }
        sent_ += 1;
      }
      if (!finish_.is_pendable()) {
        finish_ = writer_.Finish();
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    size_t count_ = 0;
    size_t sent_ = 0;
    RawWriter writer_;
    Payload message_;
    WriteFuture<ConstBuf> write_;
    WriteFuture<> finish_;
  };

  // Reads messages until the client finishes, then responds with a
  // `SumStreamResponse`.
  class AccumulateClientStreamFuture : public TestMethodFuture {
   public:
    AccumulateClientStreamFuture() = default;
    AccumulateClientStreamFuture(RawTestService& service,
                                 RawReader reader,
                                 RawUnaryWriter responder)
        : TestMethodFuture(service),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (!input_done_) {
        if (!read_.is_pendable()) {
          read_ = reader_.Read();
        }
        PW_AWAIT(Result<ConstBuf> message, read_, cx);
        if (message.status().IsOutOfRange()) {
          input_done_ = true;
          break;
        }
        if (!message.ok()) {
          return Complete(message.status());
        }
        response_.messages += 1;
        for (std::byte b : *message) {
          response_.byte_sum += static_cast<uint8_t>(b);
        }
      }
      if (!finish_.is_pendable()) {
        finish_ = responder_.FinishCopy(
            ConstBuf::Unowned(as_bytes(span(&response_, 1))));
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    RawReader reader_;
    RawUnaryWriter responder_;
    RawReadFuture read_;
    WriteFuture<ConstBuf> finish_;
    SumStreamResponse response_{};
    bool input_done_ = false;
  };

  // Echoes each message until the client finishes, then finishes.
  class EchoBidiStreamFuture : public TestMethodFuture {
   public:
    EchoBidiStreamFuture() = default;
    EchoBidiStreamFuture(RawTestService& service,
                         RawReader reader,
                         RawWriter writer)
        : TestMethodFuture(service),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (!input_done_) {
        if (!write_.is_pendable()) {
          if (!read_.is_pendable()) {
            read_ = reader_.Read();
          }
          PW_AWAIT(Result<ConstBuf> message, read_, cx);
          if (message.status().IsOutOfRange()) {
            input_done_ = true;
            break;
          }
          if (!message.ok()) {
            return Complete(message.status());
          }
          write_ = writer_.WriteCopy(std::move(*message));
        }
        PW_AWAIT(Status status, write_, cx);
        if (!status.ok()) {
          return Complete(status);
        }
      }
      if (!finish_.is_pendable()) {
        finish_ = writer_.Finish();
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    RawReader reader_;
    RawWriter writer_;
    RawReadFuture read_;
    WriteFuture<ConstBuf> write_;
    WriteFuture<> finish_;
    bool input_done_ = false;
  };

 private:
  size_t unary_echo_calls_ = 0;
};

static_assert(async2::Future<RawTestService::EchoUnaryFuture>);
static_assert(async2::Future<RawTestService::CountUpServerStreamFuture>);
static_assert(async2::Future<RawTestService::AccumulateClientStreamFuture>);
static_assert(async2::Future<RawTestService::EchoBidiStreamFuture>);

class PwpbEchoService
    : public test::pw_rpc2::pwpb::TestEcho::Service<PwpbEchoService>,
      public ErrorRecorder {
 public:
  // Responds with the request's value plus 100.
  class EchoUnaryFuture : public TestMethodFuture {
   public:
    EchoUnaryFuture() = default;
    EchoUnaryFuture(PwpbEchoService& service,
                    EchoRequest request,
                    UnaryWriter<EchoResponse> responder)
        : TestMethodFuture(service),
          value_(request.val),
          responder_(std::move(responder)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      if (!finish_.is_pendable()) {
        finish_ = responder_.Finish(EchoResponse{.val = value_ + 100});
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    uint32_t value_ = 0;
    UnaryWriter<EchoResponse> responder_;
    WriteFuture<EchoResponse> finish_;
  };

  // Streams the request's value plus 1, 2, and 3.
  class CountUpServerStreamFuture : public TestMethodFuture {
   public:
    CountUpServerStreamFuture() = default;
    CountUpServerStreamFuture(PwpbEchoService& service,
                              EchoRequest request,
                              Writer<EchoResponse> writer)
        : TestMethodFuture(service),
          base_(request.val),
          writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (sent_ < 3) {
        if (!write_.is_pendable()) {
          write_ = writer_.Write(EchoResponse{.val = base_ + sent_ + 1});
        }
        PW_AWAIT(Status status, write_, cx);
        if (!status.ok()) {
          return Complete(status);
        }
        sent_ += 1;
      }
      if (!finish_.is_pendable()) {
        finish_ = writer_.Finish();
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    uint32_t base_ = 0;
    uint32_t sent_ = 0;
    Writer<EchoResponse> writer_;
    WriteFuture<EchoResponse> write_;
    WriteFuture<> finish_;
  };

  // Responds with the sum of the streamed values.
  class AccumulateClientStreamFuture : public TestMethodFuture {
   public:
    AccumulateClientStreamFuture() = default;
    AccumulateClientStreamFuture(PwpbEchoService& service,
                                 Reader<EchoRequest> reader,
                                 UnaryWriter<EchoResponse> responder)
        : TestMethodFuture(service),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (!input_done_) {
        if (!read_.is_pendable()) {
          read_ = reader_.Read();
        }
        PW_AWAIT(Result<EchoRequest> request, read_, cx);
        if (request.status().IsOutOfRange()) {
          input_done_ = true;
          break;
        }
        if (!request.ok()) {
          return Complete(request.status());
        }
        sum_ += request->val;
      }
      if (!finish_.is_pendable()) {
        finish_ = responder_.Finish(EchoResponse{.val = sum_});
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    Reader<EchoRequest> reader_;
    UnaryWriter<EchoResponse> responder_;
    ReadFuture<EchoRequest> read_;
    WriteFuture<EchoResponse> finish_;
    uint32_t sum_ = 0;
    bool input_done_ = false;
  };

  // Responds to each streamed value with three times the value.
  class EchoBidiStreamFuture : public TestMethodFuture {
   public:
    EchoBidiStreamFuture() = default;
    EchoBidiStreamFuture(PwpbEchoService& service,
                         Reader<EchoRequest> reader,
                         Writer<EchoResponse> writer)
        : TestMethodFuture(service),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      while (!input_done_) {
        if (!write_.is_pendable()) {
          if (!read_.is_pendable()) {
            read_ = reader_.Read();
          }
          PW_AWAIT(Result<EchoRequest> request, read_, cx);
          if (request.status().IsOutOfRange()) {
            input_done_ = true;
            break;
          }
          if (!request.ok()) {
            return Complete(request.status());
          }
          write_ = writer_.Write(EchoResponse{.val = request->val * 3});
        }
        PW_AWAIT(Status status, write_, cx);
        if (!status.ok()) {
          return Complete(status);
        }
      }
      if (!finish_.is_pendable()) {
        finish_ = writer_.Finish();
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    Reader<EchoRequest> reader_;
    Writer<EchoResponse> writer_;
    ReadFuture<EchoRequest> read_;
    WriteFuture<EchoResponse> write_;
    WriteFuture<> finish_;
    bool input_done_ = false;
  };
};

static_assert(async2::Future<PwpbEchoService::EchoUnaryFuture>);
static_assert(async2::Future<PwpbEchoService::CountUpServerStreamFuture>);
static_assert(async2::Future<PwpbEchoService::AccumulateClientStreamFuture>);
static_assert(async2::Future<PwpbEchoService::EchoBidiStreamFuture>);

// For methods a test service never expects to be called. It drops the call's
// handles when constructed, which finishes a response stream or cancels a call
// that is owed a single response, and completes on its first poll.
class UnusedMethodFuture : public test::TestFuture {
 public:
  UnusedMethodFuture() = default;
  template <typename Request, typename Responder>
  UnusedMethodFuture(Request&&, Responder&&) : test::TestFuture(true) {}

  async2::Poll<> Pend(async2::Context&) { return Complete(); }
};

static_assert(async2::Future<UnusedMethodFuture>);

// A service that handles `EchoUnary` by calling `EchoUnary` on a backend
// server from inside the method, on the same dispatcher.
class ProxyService : public test::pw_rpc2::raw::TestEcho::Service<ProxyService>,
                     public ErrorRecorder {
 public:
  void set_backend(const Client& client) { backend_ = RawClient(client); }

  // Forwards the request to the backend and relays its response. A server
  // cannot fail a call with an arbitrary status, so if the backend call fails,
  // this records the backend's error and cancels the frontend call.
  class EchoUnaryFuture : public TestMethodFuture {
   public:
    EchoUnaryFuture() = default;
    EchoUnaryFuture(ProxyService& proxy,
                    ConstBuf request,
                    RawUnaryWriter responder)
        : TestMethodFuture(proxy),
          backend_(proxy.backend_),
          request_(std::move(request)),
          responder_(std::move(responder)) {}

    async2::Poll<> Pend(async2::Context& cx) {
      if (!finish_.is_pendable()) {
        if (!backend_call_.is_pendable()) {
          backend_call_ = RawEchoFuture(backend_, request_);
        }
        PW_AWAIT(Result<ConstBuf> response, backend_call_, cx);
        if (!response.ok()) {
          responder_.Cancel();
          return Complete(response.status());
        }
        finish_ = responder_.FinishCopy(std::move(*response));
      }
      PW_AWAIT(Status status, finish_, cx);
      return Complete(status);
    }

   private:
    RawClient backend_;
    ConstBuf request_;
    RawUnaryWriter responder_;
    RawEchoFuture backend_call_;
    WriteFuture<ConstBuf> finish_;
  };

  using CountUpServerStreamFuture = UnusedMethodFuture;
  using AccumulateClientStreamFuture = UnusedMethodFuture;
  using EchoBidiStreamFuture = UnusedMethodFuture;

 private:
  RawClient backend_;
};

static_assert(async2::Future<ProxyService::EchoUnaryFuture>);

using TestAllocator = allocator::test::AllocatorForTest<65536>;

// Owns a fresh `TestAllocator` for as long as it lives. The allocator is too
// large for the unit test framework's fixture pool, so it is kept in static
// storage instead of in the fixture.
class ScopedTestAllocator {
 public:
  ScopedTestAllocator() { storage_.emplace(); }
  ~ScopedTestAllocator() { storage_.reset(); }

  TestAllocator& get() { return *storage_; }

 private:
  static inline std::optional<TestAllocator> storage_;
};

// Runs a `Server` hosting `RawTestService` or `PwpbEchoService`, with three
// listeners ("interfaces") that clients can connect through. Teardown closes
// every client and the server, then checks that the services recorded no
// unexpected errors and that nothing leaked.
class E2ETest : public ::testing::Test {
 protected:
  static constexpr size_t kNumInterfaces = 3;

  enum class ServiceKind { kNone, kRaw, kPwpb };

  // A connected client and both ends of the link it runs over.
  struct Connection {
    Client client;
    test::PairedConnection* client_end = nullptr;
    test::PairedConnection* server_end = nullptr;
  };

  explicit E2ETest(ServiceKind service_kind = ServiceKind::kRaw)
      : service_kind_(service_kind) {}

  void SetUp() override {
    if (service_kind_ == ServiceKind::kRaw) {
      EXPECT_EQ(server_.RegisterService(raw_service_), OkStatus());
    } else if (service_kind_ == ServiceKind::kPwpb) {
      EXPECT_EQ(server_.RegisterService(pwpb_service_), OkStatus());
    }
    for (test::MockTransport& transport : interfaces_) {
      EXPECT_EQ(server_.RegisterListenerBlocking(transport), OkStatus());
    }
    server_.Start();
  }

  void TearDown() override {
    CloseClients();
    EXPECT_EQ(RunToCompletion(server_.Close()), OkStatus());
    EXPECT_EQ(raw_service_.TakeError(), OkStatus());
    EXPECT_EQ(pwpb_service_.TakeError(), OkStatus());
    EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
  }

  template <typename Fut>
  typename Fut::value_type RunToCompletion(Fut future) {
    return test::RunToCompletion(dispatcher_, std::move(future));
  }

  // Connects a new client to the server through `listener`.
  Connection& ConnectClient(test::MockTransport& listener) {
    test::PairedConnections link = test::MakePairedConnections(allocator_);
    connections_.emplace_back();
    Connection& connection = connections_.back();
    connection.client_end = link.client_raw();
    connection.server_end = link.server_raw();
    listener.ResolveAccept(std::move(link.server()));
    Result<Client> client = RunToCompletion(
        Client::Connect(dispatcher_, allocator_, std::move(link.client())));
    EXPECT_EQ(client.status(), OkStatus());
    if (client.ok()) {
      connection.client = std::move(*client);
    }
    return connection;
  }

  // Connects a new client through interface `index`.
  Connection& ConnectClient(size_t index = 0) {
    return ConnectClient(interfaces_[index]);
  }

  // Closes every connected client and releases their handles, so that their
  // connections are freed.
  void CloseClients() {
    for (Connection& connection : connections_) {
      EXPECT_EQ(RunToCompletion(connection.client.Close()), OkStatus());
    }
    connections_.clear();
  }

  // Severs both ends of `connection`'s link at once. Neither end is told why:
  // each just sees its socket close. The server frees its end once it notices,
  // so `connection`'s raw pointers must not be used afterwards.
  static void SeverLink(Connection& connection) {
    connection.client_end->SimulateDisconnect(Status::Unavailable(),
                                              /*disconnect_peer=*/true);
  }

  // Makes a raw `EchoUnary` call and checks that it echoes `request`.
  bool ExpectEcho(const RawClient& client, ConstByteSpan request) {
    Result<ConstBuf> response = RunToCompletion(RawEchoFuture(client, request));
    EXPECT_EQ(response.status(), OkStatus());
    return response.ok() && test::ExpectBytes(*response, request);
  }

  // Starts a raw `CountUpServerStream` of `count` messages.
  Result<RawReader> StartCountStream(const RawClient& client, uint8_t count) {
    const std::byte request[] = {static_cast<std::byte>(count)};
    return test::CommitCopy(
        RunToCompletion(client.CountUpServerStream(sizeof(request))), request);
  }

  // Reads the next message and checks that it is `expected`.
  bool ExpectNextMessage(RawReader& reader, ConstByteSpan expected) {
    Result<ConstBuf> message = RunToCompletion(reader.Read());
    EXPECT_EQ(message.status(), OkStatus());
    return message.ok() && test::ExpectBytes(*message, expected);
  }

  // Checks that the peer has finished the stream.
  template <typename T>
  bool ExpectEndOfStream(Reader<T>& reader) {
    const Status status = RunToCompletion(reader.Read()).status();
    EXPECT_EQ(status, Status::OutOfRange());
    return status.IsOutOfRange();
  }

  ScopedTestAllocator scoped_allocator_;
  TestAllocator& allocator_ = scoped_allocator_.get();
  async2::DispatcherForTest dispatcher_;
  RawTestService raw_service_;
  PwpbEchoService pwpb_service_;
  std::array<test::MockTransport, kNumInterfaces> interfaces_;
  Server server_{allocator_, dispatcher_};
  Vector<Connection, 4> connections_;

 private:
  const ServiceKind service_kind_;
};

class PwpbE2ETest : public E2ETest {
 protected:
  PwpbE2ETest() : E2ETest(ServiceKind::kPwpb) {}
};

TEST_F(E2ETest, RawUnaryEcho) {
  RawClient client(ConnectClient().client);
  EXPECT_TRUE(ExpectEcho(client, MakePayload(kMaxPayloadSize, 1)));
  EXPECT_TRUE(ExpectEcho(client, MakePayload(0, 2)));
  EXPECT_EQ(raw_service_.unary_echo_calls(), 2u);
}

TEST_F(E2ETest, RawServerStream) {
  RawClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(RawReader reader, StartCountStream(client, 5));
  for (size_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(ExpectNextMessage(reader, StreamMessage(i)));
  }
  EXPECT_TRUE(ExpectEndOfStream(reader));
}

TEST_F(E2ETest, RawClientStream) {
  RawClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(client.AccumulateClientStream()));

  const Payload messages[] = {
      MakePayload(10, 1), MakePayload(0, 2), MakePayload(kMaxPayloadSize, 3)};
  SumStreamResponse expected{};
  for (const Payload& message : messages) {
    // Write through a reservation to cover the zero-copy client writer.
    PW_TEST_ASSERT_OK(test::CommitCopy(
        RunToCompletion(call.writer().ReserveWrite(message.size())), message));
    expected.messages += 1;
    for (std::byte b : message) {
      expected.byte_sum += static_cast<uint8_t>(b);
    }
  }
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Finish()));

  PW_TEST_ASSERT_OK_AND_ASSIGN(ConstBuf response,
                               RunToCompletion(std::move(call).response()));
  ASSERT_EQ(response.size(), sizeof(SumStreamResponse));
  SumStreamResponse actual;
  std::memcpy(&actual, response.data(), sizeof(actual));
  EXPECT_EQ(actual.messages, expected.messages);
  EXPECT_EQ(actual.byte_sum, expected.byte_sum);
}

TEST_F(E2ETest, RawBidiStreamInterleaved) {
  RawClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client.EchoBidiStream()));

  std::array<Payload, 8> messages;
  for (size_t i = 0; i < messages.size(); ++i) {
    messages[i] = MakePayload(16 + i * 4, static_cast<uint32_t>(i));
  }
  auto send = [&](size_t i) {
    return RunToCompletion(
        stream.writer().WriteCopy(ConstBuf::Unowned(messages[i])));
  };

  // Keep up to two echoes outstanding while writing.
  PW_TEST_ASSERT_OK(send(0));
  PW_TEST_ASSERT_OK(send(1));
  for (size_t i = 2; i < messages.size(); ++i) {
    ASSERT_TRUE(ExpectNextMessage(stream.reader(), messages[i - 2]));
    PW_TEST_ASSERT_OK(send(i));
  }

  // Echoes still arrive after the client half-closes.
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));
  ASSERT_TRUE(ExpectNextMessage(stream.reader(), messages[6]));
  ASSERT_TRUE(ExpectNextMessage(stream.reader(), messages[7]));
  EXPECT_TRUE(ExpectEndOfStream(stream.reader()));
}

TEST_F(E2ETest, RawBidiStreamEchoesEveryPayloadSize) {
  RawClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client.EchoBidiStream()));
  for (size_t size = 0; size <= kMaxPayloadSize; ++size) {
    const Payload message = MakePayload(size, static_cast<uint32_t>(size));
    PW_TEST_ASSERT_OK(
        RunToCompletion(stream.writer().WriteCopy(ConstBuf::Unowned(message))));
    ASSERT_TRUE(ExpectNextMessage(stream.reader(), message));
  }
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));
  EXPECT_TRUE(ExpectEndOfStream(stream.reader()));
}

TEST_F(PwpbE2ETest, PwpbUnary) {
  PwpbClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      EchoResponse response,
      RunToCompletion(client.EchoUnary(EchoRequest{.val = 23})));
  EXPECT_EQ(response.val, 123u);
}

TEST_F(PwpbE2ETest, PwpbServerStream) {
  PwpbClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      Reader<EchoResponse> reader,
      RunToCompletion(client.CountUpServerStream(EchoRequest{.val = 10})));
  for (uint32_t expected : {11u, 12u, 13u}) {
    PW_TEST_ASSERT_OK_AND_ASSIGN(EchoResponse response,
                                 RunToCompletion(reader.Read()));
    EXPECT_EQ(response.val, expected);
  }
  EXPECT_TRUE(ExpectEndOfStream(reader));
}

TEST_F(PwpbE2ETest, PwpbClientStream) {
  PwpbClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(client.AccumulateClientStream()));
  for (uint32_t value : {5u, 15u, 25u}) {
    PW_TEST_ASSERT_OK(
        RunToCompletion(call.writer().Write(EchoRequest{.val = value})));
  }
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Finish()));
  PW_TEST_ASSERT_OK_AND_ASSIGN(EchoResponse response,
                               RunToCompletion(std::move(call).response()));
  EXPECT_EQ(response.val, 45u);
}

TEST_F(PwpbE2ETest, PwpbBidiStream) {
  PwpbClient client(ConnectClient().client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client.EchoBidiStream()));
  for (uint32_t value : {4u, 9u}) {
    PW_TEST_ASSERT_OK(
        RunToCompletion(stream.writer().Write(EchoRequest{.val = value})));
    PW_TEST_ASSERT_OK_AND_ASSIGN(EchoResponse response,
                                 RunToCompletion(stream.reader().Read()));
    EXPECT_EQ(response.val, value * 3);
  }
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));
  EXPECT_TRUE(ExpectEndOfStream(stream.reader()));
}

TEST_F(E2ETest, ConcurrentCallsOnOneConnectionGetTheirOwnResponses) {
  Connection& connection = ConnectClient();
  RawClient client(connection.client);
  const std::array<Payload, 3> requests = {
      MakePayload(16, 1), MakePayload(32, 2), MakePayload(48, 3)};

  // Hold the responses at the server until all three calls are in flight.
  connection.server_end->SetBlockReserveWrite(true);
  std::array<async2::FutureTask<RawEchoFuture>, 3> calls;
  for (size_t i = 0; i < calls.size(); ++i) {
    calls[i].emplace_future(client, requests[i]);
    dispatcher_.Post(calls[i]);
  }
  dispatcher_.RunUntilStalled();
  EXPECT_EQ(raw_service_.unary_echo_calls(), calls.size());
  for (const auto& call : calls) {
    EXPECT_FALSE(call.has_value());
  }

  connection.server_end->SetBlockReserveWrite(false);
  dispatcher_.RunUntilStalled();
  for (size_t i = 0; i < calls.size(); ++i) {
    ASSERT_TRUE(calls[i].has_value());
    PW_TEST_ASSERT_OK(calls[i].value());
    EXPECT_TRUE(test::ExpectBytes(*calls[i].value(), requests[i]));
  }
}

TEST_F(E2ETest, MultiInterfaceBackpressureOnOneConnectionDoesNotStallOthers) {
  Connection& blocked = ConnectClient(0);
  RawClient client1(blocked.client);
  RawClient client2(ConnectClient(1).client);
  RawClient client3(ConnectClient(2).client);

  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client1.EchoBidiStream()));
  std::array<Payload, 4> messages;
  for (size_t i = 0; i < messages.size(); ++i) {
    messages[i] = MakePayload(16, static_cast<uint32_t>(i));
  }

  // Block the server's writes to client 1 before it sends anything, so every
  // echo is held at the server.
  blocked.server_end->SetBlockReserveWrite(true);
  for (const Payload& message : messages) {
    PW_TEST_ASSERT_OK(
        RunToCompletion(stream.writer().WriteCopy(ConstBuf::Unowned(message))));
  }
  async2::FutureTask first_echo(stream.reader().Read());
  dispatcher_.Post(first_echo);
  dispatcher_.RunUntilStalled();
  ASSERT_FALSE(first_echo.has_value());

  EXPECT_TRUE(ExpectEcho(client2, MakePayload(kMaxPayloadSize, 20)));
  EXPECT_TRUE(ExpectEcho(client3, MakePayload(kMaxPayloadSize, 30)));
  EXPECT_FALSE(first_echo.has_value());

  blocked.server_end->SetBlockReserveWrite(false);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(first_echo.has_value());
  PW_TEST_ASSERT_OK(first_echo.value());
  EXPECT_TRUE(test::ExpectBytes(*first_echo.value(), messages[0]));
  for (size_t i = 1; i < messages.size(); ++i) {
    ASSERT_TRUE(ExpectNextMessage(stream.reader(), messages[i]));
  }
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));
  EXPECT_TRUE(ExpectEndOfStream(stream.reader()));
}

TEST_F(E2ETest, MultiInterfaceDisconnectedClientsFailWhileOthersContinue) {
  Connection& connection1 = ConnectClient(0);
  Connection& connection2 = ConnectClient(1);
  RawClient client1(connection1.client);
  RawClient client2(connection2.client);
  RawClient client3(ConnectClient(2).client);
  const Payload request = MakePayload(kMaxPayloadSize, 1);

  SeverLink(connection1);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(client1.is_open());
  EXPECT_EQ(RunToCompletion(RawEchoFuture(client1, request)).status(),
            Status::Unavailable());
  EXPECT_TRUE(ExpectEcho(client2, request));
  EXPECT_TRUE(ExpectEcho(client3, request));

  SeverLink(connection2);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(client2.is_open());
  EXPECT_EQ(RunToCompletion(RawEchoFuture(client2, request)).status(),
            Status::Unavailable());
  EXPECT_TRUE(ExpectEcho(client3, request));
  EXPECT_TRUE(client3.is_open());
}

TEST_F(E2ETest, MultiInterfaceDisconnectWithEchoesInFlight) {
  Connection& severed = ConnectClient(0);
  RawClient client1(severed.client);
  RawClient client2(ConnectClient(1).client);

  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client1.EchoBidiStream()));
  severed.server_end->SetBlockReserveWrite(true);
  for (uint32_t i = 0; i < 3; ++i) {
    PW_TEST_ASSERT_OK(RunToCompletion(
        stream.writer().WriteCopy(ConstBuf::Unowned(MakePayload(16, i)))));
  }
  async2::FutureTask echo(stream.reader().Read());
  dispatcher_.Post(echo);
  dispatcher_.RunUntilStalled();
  ASSERT_FALSE(echo.has_value());

  SeverLink(severed);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo.value().status(), Status::Cancelled());
  EXPECT_EQ(RunToCompletion(stream.writer().WriteCopy(
                ConstBuf::Unowned(MakePayload(16, 3)))),
            Status::Cancelled());
  // The server's echo was waiting for a reservation when the link went down.
  EXPECT_EQ(raw_service_.TakeError(), Status::Unavailable());

  EXPECT_TRUE(ExpectEcho(client2, MakePayload(kMaxPayloadSize, 2)));
}

TEST_F(E2ETest, MultiInterfaceServerStreamsAreIndependent) {
  RawClient client1(ConnectClient(0).client);
  RawClient client2(ConnectClient(1).client);
  RawClient client3(ConnectClient(2).client);

  PW_TEST_ASSERT_OK_AND_ASSIGN(RawReader reader1, StartCountStream(client1, 8));
  PW_TEST_ASSERT_OK_AND_ASSIGN(RawReader reader2, StartCountStream(client2, 4));
  EXPECT_TRUE(ExpectEcho(client3, MakePayload(kMaxPayloadSize, 3)));

  // Alternate between the streams; each sees only its own messages.
  for (size_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(ExpectNextMessage(reader2, StreamMessage(i)));
    ASSERT_TRUE(ExpectNextMessage(reader1, StreamMessage(i)));
  }
  EXPECT_TRUE(ExpectEndOfStream(reader2));
  for (size_t i = 4; i < 8; ++i) {
    ASSERT_TRUE(ExpectNextMessage(reader1, StreamMessage(i)));
  }
  EXPECT_TRUE(ExpectEndOfStream(reader1));
}

TEST_F(E2ETest, DisconnectFailsPendingAndLaterStreamOperations) {
  Connection& connection = ConnectClient();
  RawClient client(connection.client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client.EchoBidiStream()));
  const Payload message = MakePayload(32, 1);
  PW_TEST_ASSERT_OK(
      RunToCompletion(stream.writer().WriteCopy(ConstBuf::Unowned(message))));
  ASSERT_TRUE(ExpectNextMessage(stream.reader(), message));

  async2::FutureTask pending_read(stream.reader().Read());
  dispatcher_.Post(pending_read);
  dispatcher_.RunUntilStalled();
  ASSERT_FALSE(pending_read.has_value());

  SeverLink(connection);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(pending_read.has_value());
  EXPECT_EQ(pending_read.value().status(), Status::Cancelled());
  EXPECT_EQ(RunToCompletion(stream.reader().Read()).status(),
            Status::Cancelled());
  EXPECT_EQ(RunToCompletion(stream.writer().ReserveWrite(16)).status(),
            Status::Cancelled());
  EXPECT_FALSE(client.is_open());
  // The server's echo loop, which was waiting for the next message, is dropped
  // with the connection without being resumed, so it records no error.
}

TEST_F(E2ETest, RepeatedBackpressureReturnsMemoryToBaseline) {
  Connection& connection = ConnectClient();
  RawClient client(connection.client);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto stream,
                               RunToCompletion(client.EchoBidiStream()));

  // One round trip first, so that any state allocated on first use exists.
  const Payload warm_up = MakePayload(32, 0);
  PW_TEST_ASSERT_OK(
      RunToCompletion(stream.writer().WriteCopy(ConstBuf::Unowned(warm_up))));
  ASSERT_TRUE(ExpectNextMessage(stream.reader(), warm_up));
  const size_t baseline = allocator_.metrics().allocated_bytes.value();

  constexpr size_t kRounds = 20;
  constexpr size_t kMessagesPerRound = 4;
  size_t first_round_in_flight = 0;
  for (size_t round = 0; round < kRounds; ++round) {
    std::array<Payload, kMessagesPerRound> messages;
    connection.server_end->SetBlockReserveWrite(true);
    for (size_t i = 0; i < messages.size(); ++i) {
      messages[i] =
          MakePayload(32, static_cast<uint32_t>(round * kMessagesPerRound + i));
      PW_TEST_ASSERT_OK(RunToCompletion(
          stream.writer().WriteCopy(ConstBuf::Unowned(messages[i]))));
    }
    async2::FutureTask first_echo(stream.reader().Read());
    dispatcher_.Post(first_echo);
    dispatcher_.RunUntilStalled();
    ASSERT_FALSE(first_echo.has_value());

    // Sample while every echo is held at the server. Holding the same traffic
    // must cost the same in every round.
    const size_t in_flight =
        allocator_.metrics().allocated_bytes.value() - baseline;
    if (round == 0) {
      ASSERT_GT(in_flight, 0u);
      first_round_in_flight = in_flight;
    } else {
      ASSERT_EQ(in_flight, first_round_in_flight);
    }

    connection.server_end->SetBlockReserveWrite(false);
    dispatcher_.RunUntilStalled();
    ASSERT_TRUE(first_echo.has_value());
    PW_TEST_ASSERT_OK(first_echo.value());
    ASSERT_TRUE(test::ExpectBytes(*first_echo.value(), messages[0]));
    for (size_t i = 1; i < messages.size(); ++i) {
      ASSERT_TRUE(ExpectNextMessage(stream.reader(), messages[i]));
    }
    first_echo.reset();

    // Once drained, nothing from the round is still allocated.
    ASSERT_EQ(allocator_.metrics().allocated_bytes.value(), baseline);
  }

  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));
  EXPECT_TRUE(ExpectEndOfStream(stream.reader()));
}

// Adds a proxy server whose `ProxyService` forwards `EchoUnary` to the
// fixture's server (the backend). `frontend_` calls the proxy.
class ProxyTest : public E2ETest {
 protected:
  explicit ProxyTest(ServiceKind backend_service = ServiceKind::kRaw)
      : E2ETest(backend_service) {}

  void SetUp() override {
    E2ETest::SetUp();
    EXPECT_EQ(proxy_server_.RegisterService(proxy_service_), OkStatus());
    EXPECT_EQ(proxy_server_.RegisterListenerBlocking(proxy_listener_),
              OkStatus());
    proxy_server_.Start();

    backend_ = &ConnectClient();
    proxy_service_.set_backend(backend_->client);
    frontend_ = RawClient(ConnectClient(proxy_listener_).client);
  }

  void TearDown() override {
    // Release the extra client handles so the connections can be freed.
    frontend_ = RawClient();
    proxy_service_.set_backend(Client());
    CloseClients();
    EXPECT_EQ(RunToCompletion(proxy_server_.Close()), OkStatus());
    EXPECT_EQ(proxy_service_.TakeError(), OkStatus());
    E2ETest::TearDown();
  }

  ProxyService proxy_service_;
  test::MockTransport proxy_listener_;
  Server proxy_server_{allocator_, dispatcher_};
  Connection* backend_ = nullptr;
  RawClient frontend_;
};

TEST_F(ProxyTest, ForwardsUnaryCallsToBackend) {
  for (uint32_t i = 0; i < 5; ++i) {
    EXPECT_TRUE(ExpectEcho(frontend_, MakePayload(kMaxPayloadSize, i)));
  }
  EXPECT_EQ(raw_service_.unary_echo_calls(), 5u);

  // Hold the backend's responses until all three forwarded calls reach it.
  const std::array<Payload, 3> requests = {
      MakePayload(16, 10), MakePayload(32, 11), MakePayload(48, 12)};
  backend_->server_end->SetBlockReserveWrite(true);
  std::array<async2::FutureTask<RawEchoFuture>, 3> calls;
  for (size_t i = 0; i < calls.size(); ++i) {
    calls[i].emplace_future(frontend_, requests[i]);
    dispatcher_.Post(calls[i]);
  }
  dispatcher_.RunUntilStalled();
  EXPECT_EQ(raw_service_.unary_echo_calls(), 5u + calls.size());
  for (const auto& call : calls) {
    EXPECT_FALSE(call.has_value());
  }

  backend_->server_end->SetBlockReserveWrite(false);
  dispatcher_.RunUntilStalled();
  for (size_t i = 0; i < calls.size(); ++i) {
    ASSERT_TRUE(calls[i].has_value());
    PW_TEST_ASSERT_OK(calls[i].value());
    EXPECT_TRUE(test::ExpectBytes(*calls[i].value(), requests[i]));
  }
}

TEST_F(ProxyTest, BackendDisconnectCancelsFrontendCall) {
  // Hold the proxy's request to the backend, then sever the backend link.
  backend_->client_end->SetBlockReserveWrite(true);
  const Payload request = MakePayload(kMaxPayloadSize, 1);
  async2::FutureTask call(RawEchoFuture(frontend_, request));
  dispatcher_.Post(call);
  dispatcher_.RunUntilStalled();
  ASSERT_FALSE(call.has_value());

  SeverLink(*backend_);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ(call.value().status(), Status::Cancelled());
  EXPECT_EQ(proxy_service_.TakeError(), Status::Unavailable());
  EXPECT_EQ(raw_service_.unary_echo_calls(), 0u);
}

// A `ProxyTest` whose backend server has no services.
class ProxyToEmptyBackendTest : public ProxyTest {
 protected:
  ProxyToEmptyBackendTest() : ProxyTest(ServiceKind::kNone) {}
};

TEST_F(ProxyToEmptyBackendTest, BackendErrorCancelsFrontendCall) {
  const Payload request = MakePayload(kMaxPayloadSize, 1);
  EXPECT_EQ(RunToCompletion(RawEchoFuture(frontend_, request)).status(),
            Status::Cancelled());
  EXPECT_EQ(proxy_service_.TakeError(), Status::NotFound());
}

}  // namespace
}  // namespace pw::rpc2
