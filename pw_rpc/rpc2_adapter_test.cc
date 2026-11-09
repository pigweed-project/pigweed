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

#include "pw_rpc/rpc2_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/method_lookup.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/pwpb/server_reader_writer.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/generated_service_client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/status.h"
#include "pw_rpc_test_protos/no_package.raw_rpc.pb.h"
#include "pw_rpc_test_protos/test.pwpb.rpc2.h"
#include "pw_rpc_test_protos/test.raw_rpc.pb.h"
#include "pw_rpc_test_protos/test.rpc.pwpb.h"
#include "pw_status/status.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/options.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_transport/forwarding_socket.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace {

using TestRequest = pw::rpc::test::pwpb::TestRequest::Message;
using TestResponse = pw::rpc::test::pwpb::TestResponse::Message;
using TestStreamResponse = pw::rpc::test::pwpb::TestStreamResponse::Message;

// pw_rpc2 client for pw.rpc.test.TestService.
using Rpc2Client = pw::rpc::test::pw_rpc2::pwpb::TestService::Client;

// Legacy handlers run on the adapter's thread, where a failed test expectation
// would race with the test thread. Handlers count failures here instead, and
// the harness checks the count on the test thread.
std::atomic<int> handler_failures = 0;

void ExpectOkInHandler(pw::Status status) {
  if (!status.ok()) {
    handler_failures.fetch_add(1);
  }
}

// A legacy pw_rpc service, implemented with the pw_protobuf API.
class LegacyService final
    : public pw::rpc::test::pw_rpc::pwpb::TestService::Service<LegacyService> {
 public:
  // Responds with `integer + 1` and ends the call with `status_code`. If
  // `integer` is -1, first blocks until `unblock` is released.
  pw::Status TestUnaryRpc(const TestRequest& request, TestResponse& response) {
    unary_calls += 1;
    if (request.integer == -1) {
      blocked.release();
      unblock.acquire();
    }
    response.value = static_cast<int32_t>(request.integer + 1);
    return static_cast<pw::Status::Code>(request.status_code);
  }

  // Keeps the responder so that the test can respond later. If `integer` is -1,
  // calls `TryFinish()` inside the handler before storing the responder.
  void TestAnotherUnaryRpc(
      const TestRequest& request,
      pw::rpc::PwpbUnaryResponder<TestResponse>& responder) {
    last_request = static_cast<int32_t>(request.integer);
    if (request.integer == -1) {
      last_try_finish_status =
          responder.TryFinish({.value = 0, .repeated_field = {}}).code();
    }
    unary_responder = std::move(responder);
  }

  // Streams responses numbered `0` to `integer - 1`, then ends the call with
  // `status_code`. If `integer` is negative, keeps the writer instead (and if
  // -2, blocks until `unblock` is released). Records the call's channel ID.
  void TestServerStreamRpc(const TestRequest& request,
                           ServerWriter<TestStreamResponse>& new_writer) {
    last_channel_id = new_writer.channel_id();
    if (request.integer < 0) {
      writer = std::move(new_writer);
      writer.set_on_error([this](pw::Status status) {
        last_stream_error = status.code();
        stream_errors += 1;
      });
      server_stream_calls += 1;
      if (request.integer == -2) {
        blocked.release();
        unblock.acquire();
      }
      return;
    }
    for (int64_t i = 0; i < request.integer; ++i) {
      ExpectOkInHandler(
          new_writer.Write({.chunk = {}, .number = static_cast<uint32_t>(i)}));
    }
    ExpectOkInHandler(
        new_writer.Finish(static_cast<pw::Status::Code>(request.status_code)));
    server_stream_calls += 1;
  }

  // Sums the requests' integers until one is 0, then responds with the sum and
  // ends the call with that request's `status_code`.
  void TestClientStreamRpc(
      ServerReader<TestRequest, TestStreamResponse>& new_reader) {
    sum = 0;
    reader = std::move(new_reader);
    reader.set_on_next([this](const TestRequest& request) {
      if (request.integer != 0) {
        sum += static_cast<uint32_t>(request.integer);
        return;
      }
      ExpectOkInHandler(
          reader.Finish({.chunk = {}, .number = sum.load()},
                        static_cast<pw::Status::Code>(request.status_code)));
    });
    reader.set_on_completion_requested_if_enabled([this]() {
      ExpectOkInHandler(reader.Finish({.chunk = {}, .number = sum.load()}));
    });
  }

  // Responds to each request with its integer, until one is 0, which ends the
  // call with its `status_code`.
  void TestBidirectionalStreamRpc(
      ServerReaderWriter<TestRequest, TestStreamResponse>& new_reader_writer) {
    reader_writer = std::move(new_reader_writer);
    reader_writer.set_on_next([this](const TestRequest& request) {
      if (request.integer == 0) {
        ExpectOkInHandler(reader_writer.Finish(
            static_cast<pw::Status::Code>(request.status_code)));
        return;
      }
      ExpectOkInHandler(reader_writer.Write(
          {.chunk = {}, .number = static_cast<uint32_t>(request.integer)}));
    });
  }

  std::atomic<int> unary_calls = 0;
  std::atomic<int> server_stream_calls = 0;
  std::atomic<int> stream_errors = 0;
  std::atomic<pw::Status::Code> last_stream_error = pw::OkStatus().code();
  std::atomic<pw::Status::Code> last_try_finish_status = pw::OkStatus().code();
  pw::sync::ThreadNotification blocked;
  pw::sync::ThreadNotification unblock;
  std::atomic<int32_t> last_request = 0;
  std::atomic<uint32_t> last_channel_id = 0;
  std::atomic<uint32_t> sum = 0;
  pw::rpc::PwpbUnaryResponder<TestResponse> unary_responder;
  ServerWriter<TestStreamResponse> writer;
  ServerReader<TestRequest, TestStreamResponse> reader;
  ServerReaderWriter<TestRequest, TestStreamResponse> reader_writer;
};

// A legacy pw_rpc service, implemented with the raw API, for the generated
// service `Base`.
template <template <typename> class Base>
class RawEchoService final : public Base<RawEchoService<Base>> {
 public:
  // Responds with the request's bytes.
  static void TestUnaryRpc(pw::ConstByteSpan request,
                           pw::rpc::RawUnaryResponder& responder) {
    ExpectOkInHandler(responder.Finish(request));
  }

  static void TestAnotherUnaryRpc(pw::ConstByteSpan request,
                                  pw::rpc::RawUnaryResponder& responder) {
    ExpectOkInHandler(responder.Finish(request, pw::Status::Unimplemented()));
  }

  static void TestServerStreamRpc(pw::ConstByteSpan,
                                  pw::rpc::RawServerWriter& writer) {
    ExpectOkInHandler(writer.Finish(pw::Status::Unimplemented()));
  }

  static void TestClientStreamRpc(pw::rpc::RawServerReader& reader) {
    ExpectOkInHandler(reader.Finish({}, pw::Status::Unimplemented()));
  }

  static void TestBidirectionalStreamRpc(
      pw::rpc::RawServerReaderWriter& reader_writer) {
    ExpectOkInHandler(reader_writer.Finish(pw::Status::Unimplemented()));
  }
};

// pw.rpc.test.TestService, implemented with the raw API.
using RawLegacyService =
    RawEchoService<pw::rpc::test::pw_rpc::raw::TestService::Service>;

// PwRpcTestService, which has the same methods as pw.rpc.test.TestService but
// a different service ID.
using OtherRawLegacyService =
    RawEchoService<pw_rpc::raw::PwRpcTestService::Service>;

// A raw pw_rpc2 client for PwRpcTestService's unary method.
class OtherServiceClient final
    : public pw::rpc2::internal::GeneratedServiceClient {
 public:
  explicit OtherServiceClient(const pw::rpc2::Client& client)
      : GeneratedServiceClient(
            client,
            pw::rpc::internal::UnwrapServiceId(
                pw_rpc::raw::PwRpcTestService::service_id())) {}

  pw::rpc2::UnaryFuture<pw::ConstBuf, pw::ConstBuf> TestUnaryRpc(
      pw::ConstBuf&& request) const {
    return CallUnary<pw::ConstBuf, pw::ConstBuf>(
        pw::rpc::internal::MethodLookup::MethodIds<OtherRawLegacyService>()[0],
        std::move(request));
  }
};

// The adapter builds the pw_rpc2 method table from the IDs and types in the
// legacy generated code.
using pw::rpc::internal::MethodLookup;

constexpr std::array kExpectedMethodTypes = {
    pw::rpc2::MethodType::kUnary,
    pw::rpc2::MethodType::kUnary,
    pw::rpc2::MethodType::kServerStreaming,
    pw::rpc2::MethodType::kClientStreaming,
    pw::rpc2::MethodType::kBidirectionalStreaming,
};

template <typename Service>
constexpr bool AdaptedMethodsMatchGeneratedCode() {
  constexpr const auto& kMethods = pw::rpc::internal::kAdaptedMethods<Service>;
  constexpr const auto& kIds = MethodLookup::MethodIds<Service>();
  if (kMethods.size() != kExpectedMethodTypes.size() ||
      kIds.size() != kExpectedMethodTypes.size()) {
    return false;
  }
  for (size_t i = 0; i < kMethods.size(); ++i) {
    if (kMethods[i].id() != kIds[i] ||
        kMethods[i].type() != kExpectedMethodTypes[i]) {
      return false;
    }
  }
  return true;
}

static_assert(AdaptedMethodsMatchGeneratedCode<LegacyService>());
static_assert(AdaptedMethodsMatchGeneratedCode<RawLegacyService>());

static_assert(!pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                        /*kEnableNonOk=*/false>(
    MethodLookup::MethodIds<LegacyService>()[0],
    MethodLookup::MethodTypes<LegacyService>()[0]));
static_assert(pw::rpc::internal::DeliversNonOkResponse<
              LegacyService,
              /*kEnableNonOk=*/true,
              LegacyService::ServiceInfo::TestUnaryRpc,
              LegacyService::ServiceInfo::TestClientStreamRpc>(
    MethodLookup::MethodIds<LegacyService>()[0],
    MethodLookup::MethodTypes<LegacyService>()[0]));
static_assert(!pw::rpc::internal::DeliversNonOkResponse<
              LegacyService,
              /*kEnableNonOk=*/true,
              LegacyService::ServiceInfo::TestUnaryRpc,
              LegacyService::ServiceInfo::TestClientStreamRpc>(
    MethodLookup::MethodIds<LegacyService>()[1],
    MethodLookup::MethodTypes<LegacyService>()[1]));
static_assert(pw::rpc::internal::DeliversNonOkResponse<
              LegacyService,
              /*kEnableNonOk=*/true,
              LegacyService::ServiceInfo::TestUnaryRpc,
              LegacyService::ServiceInfo::TestClientStreamRpc>(
    MethodLookup::MethodIds<LegacyService>()[3],
    MethodLookup::MethodTypes<LegacyService>()[3]));
static_assert(pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                       /*kEnableNonOk=*/true>(
    MethodLookup::MethodIds<LegacyService>()[0],
    MethodLookup::MethodTypes<LegacyService>()[0]));
static_assert(pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                       /*kEnableNonOk=*/true>(
    MethodLookup::MethodIds<LegacyService>()[1],
    MethodLookup::MethodTypes<LegacyService>()[1]));
static_assert(!pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                        /*kEnableNonOk=*/true>(
    MethodLookup::MethodIds<LegacyService>()[2],
    MethodLookup::MethodTypes<LegacyService>()[2]));
static_assert(pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                       /*kEnableNonOk=*/true>(
    MethodLookup::MethodIds<LegacyService>()[3],
    MethodLookup::MethodTypes<LegacyService>()[3]));
static_assert(!pw::rpc::internal::DeliversNonOkResponse<LegacyService,
                                                        /*kEnableNonOk=*/true>(
    MethodLookup::MethodIds<LegacyService>()[4],
    MethodLookup::MethodTypes<LegacyService>()[4]));

#if PW_NC_TEST(NotALegacyService)
PW_NC_EXPECT("Rpc2ServiceAdapter wraps a pw_rpc service implementation");
void BadServiceType(pw::rpc::Rpc2ServerAdapter& adapter) {
  int not_a_service = 0;
  pw::rpc::Rpc2ServiceAdapter adapted(adapter, not_a_service);
}
#elif PW_NC_TEST(NonOkResponseMethodFromWrongService)
PW_NC_EXPECT("The specified pw_rpc method does not belong to this service");
void BadMethodFromWrongService(pw::rpc::Rpc2ServerAdapter& adapter) {
  pw::rpc::Rpc2ServiceFor<LegacyService> adapted(
      adapter,
      pw::rpc::DeliverNonOkResponses<
          OtherRawLegacyService::ServiceInfo::TestUnaryRpc>());
}
#elif PW_NC_TEST(NonOkResponseServerStreamingMethod)
PW_NC_EXPECT(
    "Only unary and client streaming pw_rpc methods send a response payload");
void BadServerStreamingMethod(pw::rpc::Rpc2ServerAdapter& adapter) {
  pw::rpc::Rpc2ServiceFor<LegacyService> adapted(
      adapter,
      pw::rpc::DeliverNonOkResponses<
          LegacyService::ServiceInfo::TestServerStreamRpc>());
}
#elif PW_NC_TEST(NonOkResponseBidirectionalStreamingMethod)
PW_NC_EXPECT(
    "Only unary and client streaming pw_rpc methods send a response payload");
void BadBidirectionalStreamingMethod(pw::rpc::Rpc2ServerAdapter& adapter) {
  pw::rpc::Rpc2ServiceFor<LegacyService> adapted(
      adapter,
      pw::rpc::DeliverNonOkResponses<
          LegacyService::ServiceInfo::TestBidirectionalStreamRpc>());
}
#elif PW_NC_TEST(NonOkResponseNotAGeneratedMethod)
PW_NC_EXPECT("The provided argument is not a generated pw_rpc function");
void BadNonGeneratedMethod(pw::rpc::Rpc2ServerAdapter& adapter) {
  pw::rpc::Rpc2ServiceFor<LegacyService> adapted(
      adapter, pw::rpc::DeliverNonOkResponses<&LegacyService::TestUnaryRpc>());
}
#endif  // PW_NC_TEST

// The tests below stop and destroy adapters, which requires joining threads.
#if PW_THREAD_JOINING_ENABLED

// An Rpc2ServerAdapter sharing an external pw::rpc::Server.
struct SharedServer {
  static constexpr size_t kLegacyChannelSlots = 8;

  SharedServer(pw::Allocator& allocator,
               const pw::thread::Options& options,
               const pw::rpc::Rpc2AdapterConfig& config)
      : adapter(server, allocator, options, config) {}

  // Unassigned slots, which the adapter (and tests) open channels in.
  std::array<pw::rpc::Channel, kLegacyChannelSlots> channels{};
  pw::rpc::Server server{channels};
  pw::rpc::Rpc2ServerAdapter adapter;
};

// An Rpc2Adapter with its own internal pw::rpc::Server.
struct InternalServer {
  InternalServer(pw::Allocator& allocator,
                 const pw::thread::Options& options,
                 const pw::rpc::Rpc2AdapterConfig& config)
      : adapter(allocator, options, config) {}

  pw::rpc::Rpc2Adapter adapter;
};

// A legacy service owned by its pw_rpc2 service (`Rpc2ServiceFor`), which
// registers it with the adapter's server.
template <typename Impl,
          typename ServerKind = SharedServer,
          typename Policy = void>
class Owned {
 public:
  using ServiceImpl = Impl;
  using AdapterHolder = ServerKind;

  explicit Owned(AdapterHolder& holder) : Owned(holder, Tag<Policy>{}) {}

  void RegisterWith(pw::rpc2::Server& server) {
    PW_CHECK_OK(server.RegisterService(adapted_));
  }

  ServiceImpl& legacy() { return adapted_.service(); }

 private:
  template <typename>
  struct Tag {};

  Owned(AdapterHolder& holder, Tag<void>) : adapted_(holder.adapter) {}

  template <typename P>
  Owned(AdapterHolder& holder, Tag<P>) : adapted_(holder.adapter, P{}) {}

  pw::rpc::Rpc2ServiceFor<ServiceImpl> adapted_;
};

// An existing legacy service, registered with the legacy server as usual, and
// wrapped by a pw_rpc2 service (`Rpc2ServiceAdapter`).
template <typename Impl, typename Policy = void>
class Wrapped {
 public:
  using ServiceImpl = Impl;
  using AdapterHolder = SharedServer;

  explicit Wrapped(AdapterHolder& holder) : Wrapped(holder, Tag<Policy>{}) {
    server_.RegisterService(legacy_);
  }

  ~Wrapped() { server_.UnregisterService(legacy_); }

  void RegisterWith(pw::rpc2::Server& server) {
    PW_CHECK_OK(server.RegisterService(adapted_));
  }

  ServiceImpl& legacy() { return legacy_; }

 private:
  template <typename>
  struct Tag {};

  Wrapped(AdapterHolder& holder, Tag<void>)
      : server_(holder.server), adapted_(holder.adapter, legacy_) {}

  template <typename P>
  Wrapped(AdapterHolder& holder, Tag<P>)
      : server_(holder.server), adapted_(holder.adapter, P{}, legacy_) {}

  pw::rpc::Server& server_;
  ServiceImpl legacy_;
  pw::rpc::Rpc2ServiceAdapter adapted_;
};

// Two legacy services with different IDs on one adapter.
class TwoServices {
 public:
  using ServiceImpl = LegacyService;
  using AdapterHolder = SharedServer;

  explicit TwoServices(AdapterHolder& holder)
      : first_(holder.adapter), second_(holder.adapter) {}

  void RegisterWith(pw::rpc2::Server& server) {
    PW_CHECK_OK(server.RegisterService(first_));
    PW_CHECK_OK(server.RegisterService(second_));
  }

  ServiceImpl& legacy() { return first_.service(); }

 private:
  pw::rpc::Rpc2ServiceFor<LegacyService> first_;
  pw::rpc::Rpc2ServiceFor<OtherRawLegacyService> second_;
};

// Captures the encoded packets the legacy server sends on a channel that is
// not the adapter's, standing in for a legacy transport such as a UART.
class CapturingOutput final : public pw::rpc::ChannelOutput {
 public:
  CapturingOutput() : ChannelOutput("CapturingOutput") {}

  pw::Status Send(pw::span<const std::byte> buffer) override {
    size_ = std::min(buffer.size(), buffer_.size());
    std::copy_n(buffer.begin(), size_, buffer_.begin());
    packets_ += 1;
    return pw::OkStatus();
  }

  int packets() const { return packets_; }
  pw::ConstByteSpan last_packet() const {
    return pw::ConstByteSpan(buffer_.data(), size_);
  }

 private:
  std::array<std::byte, 64> buffer_{};
  size_t size_ = 0;
  int packets_ = 0;
};

// An adapter, pw_rpc2 server and clients, and legacy services adapted onto the
// pw_rpc2 server by `Adapted` (`Owned`, `Wrapped`, or `TwoServices`).
//
// The adapter runs legacy handlers on its own thread, which wakes the test's
// dispatcher, so the helpers below poll the dispatcher until a condition holds.
template <typename Adapted>
class Harness {
 public:
  using ServiceImpl = typename Adapted::ServiceImpl;

  explicit Harness(const pw::rpc::Rpc2AdapterConfig& config)
      : adapter_(adapter_allocator_, thread_context_.options(), config),
        service_(adapter_) {
    service_.RegisterWith(server_);
    PW_CHECK_OK(server_.RegisterListenerBlocking(transport_));
    server_.Start();
    client_ = Connect();
  }

  ~Harness() {
    // Close the client, if a test did not already, and the server.
    std::ignore = RunToCompletion(client_.Close());
    client_ = pw::rpc2::Client();
    transport_.ResolveAccept(pw::Status::Cancelled());
    PW_CHECK_OK(RunToCompletion(server_.Close()));

    // Stopping the adapter cancels any legacy calls that were still open and
    // closes its channels. Stop() is idempotent; the adapter's destructor calls
    // it again.
    adapter_.adapter.Stop();
    adapter_.adapter.Stop();

    EXPECT_EQ(handler_failures.exchange(0), 0)
        << "A legacy handler's write failed";

    // Nothing leaked, and every packet the adapter queued was freed.
    EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
    EXPECT_EQ(adapter_allocator_.metrics().allocated_bytes.value(), 0u);
  }

  // Connects another pw_rpc2 client to the server. Close it before the test
  // ends.
  pw::rpc2::Client Connect() {
    pw::transport::ForwardingSocketPair sockets =
        pw::transport::ForwardingSocketPair::Create(allocator_);
    transport_.ResolveAccept(sockets.second());
    pw::rpc2::RpcResult<pw::rpc2::Client> client = RunToCompletion(
        pw::rpc2::Client::Connect(dispatcher_, allocator_, sockets.first()));
    PW_CHECK_OK(client.status());
    return std::move(*client);
  }

  Rpc2Client client() const { return Rpc2Client(client_); }
  pw::rpc2::Client& rpc2_client() { return client_; }
  ServiceImpl& legacy_service() { return service_.legacy(); }
  pw::rpc::Server& legacy_server() { return adapter_.server; }
  auto& adapter_allocator() { return adapter_allocator_; }
  pw::async2::DispatcherForTest& dispatcher() { return dispatcher_; }

  // Runs the dispatcher until `future` completes.
  template <typename Fut>
  typename Fut::value_type RunToCompletion(Fut future) {
    pw::async2::FutureTask task(std::move(future));
    dispatcher_.Post(task);
    if (!RunUntil([&task] { return task.has_value(); })) {
      task.Deregister();
      ADD_FAILURE() << "The future did not complete";
      return typename Fut::value_type(pw::rpc2::RpcStatus::ProtocolError());
    }
    return std::move(task.value());
  }

  // Polls `future` in a task until `done()` returns true, keeping the future
  // so that it can be polled again. Returns its latest `Poll`.
  template <typename Fut, typename Predicate>
  auto RunInTaskUntil(Fut& future, Predicate&& done) {
    pw::async2::FutureTask<Fut&, pw::async2::ReturnValuePolicy::kKeep> task(
        future);
    dispatcher_.Post(task);
    EXPECT_TRUE(RunUntil(done));
    if (task.IsRegistered()) {
      task.Deregister();
    }
    return task.TakePoll();
  }

  // Runs the dispatcher until `done()` returns true. Returns false if it
  // doesn't within `kTimeout`.
  template <typename Predicate>
  [[nodiscard]] bool RunUntil(Predicate&& done) {
    for (auto i = kTimeout / kPollInterval; i > 0; --i) {
      dispatcher_.RunUntilStalled();
      if (done()) {
        return true;
      }
      pw::this_thread::sleep_for(kPollInterval);
    }
    return false;
  }

 private:
  static constexpr size_t kAllocatorSize = 32768;
  static constexpr size_t kAdapterAllocatorSize = 4096;

  // Generous, since a passing test never waits this long, and sanitizers or
  // emulators may run slowly.
  static constexpr std::chrono::seconds kTimeout{30};
  static constexpr std::chrono::milliseconds kPollInterval{1};

  pw::allocator::test::AllocatorForTest<kAllocatorSize> allocator_;
  pw::allocator::test::AllocatorForTest<kAdapterAllocatorSize>
      adapter_allocator_;
  pw::async2::DispatcherForTest dispatcher_;
  pw::thread::test::TestThreadContext thread_context_;
  typename Adapted::AdapterHolder adapter_;
  Adapted service_;
  pw::rpc2::test::MockTransport transport_;
  pw::rpc2::Server server_{allocator_, dispatcher_};
  pw::rpc2::Client client_;
};

using SelectedLegacyNonOkPolicy =
    decltype(pw::rpc::DeliverNonOkResponses<
             LegacyService::ServiceInfo::TestUnaryRpc,
             LegacyService::ServiceInfo::TestClientStreamRpc>());
using AllNonOkPolicy = decltype(pw::rpc::DeliverNonOkResponses());
using SelectedRawNonOkPolicy =
    decltype(pw::rpc::DeliverNonOkResponses<
             RawLegacyService::ServiceInfo::TestAnotherUnaryRpc>());

// The harness is too large for the unit test framework's fixture pool, so all
// fixture instantiations share one static slot.
std::variant<
    std::monostate,
    Harness<Owned<LegacyService>>,
    Harness<Owned<LegacyService, SharedServer, SelectedLegacyNonOkPolicy>>,
    Harness<Owned<LegacyService, SharedServer, AllNonOkPolicy>>,
    Harness<Owned<RawLegacyService>>,
    Harness<Owned<RawLegacyService, SharedServer, SelectedRawNonOkPolicy>>,
    Harness<Wrapped<LegacyService>>,
    Harness<Wrapped<LegacyService, SelectedLegacyNonOkPolicy>>,
    Harness<Wrapped<LegacyService, AllNonOkPolicy>>,
    Harness<Owned<LegacyService, InternalServer>>,
    Harness<TwoServices>>
    harness_storage;

template <typename Adapted,
          uint16_t kMaxQueuedMessages = 4,
          uint16_t kMaxChannels = 0,
          uint32_t kFirstChannelId =
              pw::rpc::Rpc2AdapterConfig().first_channel_id>
class AdapterTest : public ::testing::Test {
 protected:
  static constexpr uint16_t kQueueLimit = kMaxQueuedMessages;

  void SetUp() override {
    pw::rpc::Rpc2AdapterConfig config;
    config.first_channel_id = kFirstChannelId;
    config.max_channels = kMaxChannels;
    config.max_queued_messages_per_call = kMaxQueuedMessages;
    harness_storage.emplace<Harness<Adapted>>(config);
  }

  void TearDown() override { harness_storage.emplace<std::monostate>(); }

  Harness<Adapted>& h() { return std::get<Harness<Adapted>>(harness_storage); }

  template <typename Fut>
  typename Fut::value_type RunToCompletion(Fut future) {
    return h().RunToCompletion(std::move(future));
  }

  void RetireClosedConnections() { h().dispatcher().RunUntilStalled(); }

  // Checks that the stream has no more messages.
  template <typename T>
  void ExpectEndOfStream(pw::rpc2::Reader<T>& reader) {
    EXPECT_EQ(RunToCompletion(reader.Read()).status(),
              pw::rpc2::RpcStatus::EndOfStream());
  }

  // Reads the next message and checks its number.
  void ExpectNext(pw::rpc2::Reader<TestStreamResponse>& reader,
                  uint32_t number) {
    pw::rpc2::RpcResult<TestStreamResponse> message =
        RunToCompletion(reader.Read());
    PW_TEST_ASSERT_OK(message.status());
    EXPECT_EQ(message->number, number);
  }

  // Makes a call on `client` and returns the legacy channel ID it arrived on.
  uint32_t ChannelIdOf(pw::rpc2::Client& client) {
    Rpc2Client service_client(client);
    pw::rpc2::RpcResult<pw::rpc2::Reader<TestStreamResponse>> reader =
        RunToCompletion(service_client.TestServerStreamRpc({.integer = 0}));
    PW_TEST_EXPECT_OK(reader.status());
    if (reader.ok()) {
      ExpectEndOfStream(*reader);
    }
    return h().legacy_service().last_channel_id.load();
  }

  // Behaviors shared by the fixtures for each way of adapting a service.

  void ExpectUnaryCallWorks() {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        TestResponse response,
        RunToCompletion(h().client().TestUnaryRpc({.integer = 41})));
    EXPECT_EQ(response.value, 42);
    EXPECT_EQ(h().legacy_service().unary_calls.load(), 1);
  }

  void ExpectServerStreamWorks() {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        pw::rpc2::Reader<TestStreamResponse> reader,
        RunToCompletion(h().client().TestServerStreamRpc({.integer = 3})));
    ExpectNext(reader, 0);
    ExpectNext(reader, 1);
    ExpectNext(reader, 2);
    ExpectEndOfStream(reader);
  }

  void ExpectBidirectionalStreamWorks() {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        auto stream,
        RunToCompletion(h().client().TestBidirectionalStreamRpc()));
    for (int64_t value : {4, 9, 16}) {
      PW_TEST_ASSERT_OK(
          RunToCompletion(stream.writer().Write({.integer = value})));
      ExpectNext(stream.reader(), static_cast<uint32_t>(value));
    }
    PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Write({.integer = 0})));
    ExpectEndOfStream(stream.reader());
  }

  void ExpectConnectionsGetTheirOwnChannels() {
    EXPECT_EQ(ChannelIdOf(h().rpc2_client()), kFirstChannelId);

    pw::rpc2::Client second = h().Connect();
    EXPECT_EQ(ChannelIdOf(second), kFirstChannelId + 1);
    PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
  }
};

using Rpc2AdapterTest = AdapterTest<Owned<LegacyService>>;
using Rpc2AdapterNonOkResponseTest =
    AdapterTest<Owned<LegacyService, SharedServer, SelectedLegacyNonOkPolicy>>;
using Rpc2AdapterAllNonOkResponsesTest =
    AdapterTest<Owned<LegacyService, SharedServer, AllNonOkPolicy>>;
using Rpc2AdapterRawTest = AdapterTest<Owned<RawLegacyService>>;
using Rpc2AdapterRawNonOkResponseTest =
    AdapterTest<Owned<RawLegacyService, SharedServer, SelectedRawNonOkPolicy>>;
using Rpc2ServiceAdapterTest = AdapterTest<Wrapped<LegacyService>>;
using Rpc2ServiceAdapterNonOkResponseTest =
    AdapterTest<Wrapped<LegacyService, SelectedLegacyNonOkPolicy>>;
using Rpc2ServiceAdapterAllNonOkResponsesTest =
    AdapterTest<Wrapped<LegacyService, AllNonOkPolicy>>;
using Rpc2AdapterInternalServerTest =
    AdapterTest<Owned<LegacyService, InternalServer>>;
using Rpc2AdapterTwoServicesTest = AdapterTest<TwoServices>;
// A legacy handler that writes in a loop can outpace the pw_rpc2 transport, so
// bursts need a deeper queue.
using Rpc2AdapterDeepQueueTest = AdapterTest<Owned<LegacyService>, 32>;
using Rpc2AdapterOneChannelTest = AdapterTest<Owned<LegacyService>, 4, 1>;
using Rpc2AdapterFirstChannelIdTest =
    AdapterTest<Owned<LegacyService>, 4, 0, 7>;
using Rpc2AdapterMaxChannelIdTest =
    AdapterTest<Owned<LegacyService>,
                4,
                0,
                std::numeric_limits<uint32_t>::max()>;

TEST_F(Rpc2AdapterTest, Unary) { ExpectUnaryCallWorks(); }

TEST_F(Rpc2AdapterTest, UnaryWithErrorStatusCancelsCall) {
  pw::rpc2::RpcResult<TestResponse> response = RunToCompletion(
      h().client().TestUnaryRpc({.integer = 1,
                                 .status_code = static_cast<uint32_t>(
                                     pw::Status::NotFound().code())}));
  EXPECT_EQ(response.status(), pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterNonOkResponseTest,
       OptedInUnaryMethodDeliversResponseOnNonOkStatus) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(TestResponse response,
                               RunToCompletion(h().client().TestUnaryRpc(
                                   {.integer = 41,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::NotFound().code())})));
  EXPECT_EQ(response.value, 42);
}

TEST_F(Rpc2AdapterNonOkResponseTest,
       UnlistedUnaryMethodStillCancelsOnNonOkStatus) {
  pw::rpc2::UnaryFuture<TestRequest, TestResponse> call =
      h().client().TestAnotherUnaryRpc({.integer = 99});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(
      h().RunInTaskUntil(call, [&] { return service.unary_responder.active(); })
          .IsPending());

  PW_TEST_EXPECT_OK(service.unary_responder.Finish(
      {.value = 7, .repeated_field = {}}, pw::Status::NotFound()));

  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> response =
      h().RunInTaskUntil(call, [&] { return call.is_complete(); });
  ASSERT_TRUE(response.IsReady());
  EXPECT_EQ(response->status(), pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterNonOkResponseTest,
       OptedInClientStreamMethodDeliversResponseOnNonOkStatus) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 15})));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write(
      {.integer = 0,
       .status_code = static_cast<uint32_t>(pw::Status::NotFound().code())})));

  PW_TEST_ASSERT_OK_AND_ASSIGN(TestStreamResponse response,
                               RunToCompletion(std::move(call).response()));
  EXPECT_EQ(response.number, 15u);
}

TEST_F(Rpc2AdapterNonOkResponseTest,
       OptedInMethodDeliversDefaultEmptyPayloadOnNonOkStatus) {
  // When `sum` is 0, the encoded proto3 response payload is 0 bytes.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write(
      {.integer = 0,
       .status_code =
           static_cast<uint32_t>(pw::Status::FailedPrecondition().code())})));

  PW_TEST_ASSERT_OK_AND_ASSIGN(TestStreamResponse response,
                               RunToCompletion(std::move(call).response()));
  EXPECT_EQ(response.number, 0u);
}

TEST_F(Rpc2AdapterNonOkResponseTest,
       OptedInMethodStillFailsOnInvalidRequestPayload) {
  constexpr std::array<std::byte, 2> kCorruptProto = {std::byte{0xff},
                                                      std::byte{0xff}};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::UnaryReservation<TestResponse> reservation,
      RunToCompletion(h().client().TestUnaryRpc(kCorruptProto.size())));
  std::copy(kCorruptProto.begin(), kCorruptProto.end(), reservation.data());
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::ResponseFuture<TestResponse> response_future,
      std::move(reservation).Commit(kCorruptProto.size()));
  EXPECT_EQ(RunToCompletion(std::move(response_future)).status(),
            pw::rpc2::RpcStatus::DataError());
}

TEST_F(Rpc2ServiceAdapterNonOkResponseTest,
       DeliverNonOkResponsesDeliversOnlyListedMethods) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(TestResponse response,
                               RunToCompletion(h().client().TestUnaryRpc(
                                   {.integer = 9,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::Aborted().code())})));
  EXPECT_EQ(response.value, 10);

  pw::rpc2::UnaryFuture<TestRequest, TestResponse> unlisted =
      h().client().TestAnotherUnaryRpc({.integer = 1});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(h().RunInTaskUntil(
                     unlisted, [&] { return service.unary_responder.active(); })
                  .IsPending());
  PW_TEST_EXPECT_OK(service.unary_responder.Finish(
      {.value = 1, .repeated_field = {}}, pw::Status::Aborted()));
  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> unlisted_response =
      h().RunInTaskUntil(unlisted, [&] { return unlisted.is_complete(); });
  ASSERT_TRUE(unlisted_response.IsReady());
  EXPECT_EQ(unlisted_response->status(), pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterAllNonOkResponsesTest,
       DeliverNonOkResponsesWithNoArgsDeliversAllUnaryAndClientStreamMethods) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(TestResponse first_unary,
                               RunToCompletion(h().client().TestUnaryRpc(
                                   {.integer = 41,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::NotFound().code())})));
  EXPECT_EQ(first_unary.value, 42);

  pw::rpc2::UnaryFuture<TestRequest, TestResponse> second_unary =
      h().client().TestAnotherUnaryRpc({.integer = 99});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(
      h().RunInTaskUntil(second_unary,
                         [&] { return service.unary_responder.active(); })
          .IsPending());
  PW_TEST_EXPECT_OK(service.unary_responder.Finish(
      {.value = 7, .repeated_field = {}}, pw::Status::NotFound()));
  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> second_response =
      h().RunInTaskUntil(second_unary,
                         [&] { return second_unary.is_complete(); });
  ASSERT_TRUE(second_response.IsReady());
  PW_TEST_ASSERT_OK(second_response->status());
  EXPECT_EQ((*second_response)->value, 7);

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto client_stream, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(
      RunToCompletion(client_stream.writer().Write({.integer = 15})));
  PW_TEST_ASSERT_OK(RunToCompletion(client_stream.writer().Write(
      {.integer = 0,
       .status_code = static_cast<uint32_t>(pw::Status::NotFound().code())})));
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestStreamResponse stream_response,
      RunToCompletion(std::move(client_stream).response()));
  EXPECT_EQ(stream_response.number, 15u);

  // Server streaming methods still cancel on a non-OK status.
  PW_TEST_ASSERT_OK_AND_ASSIGN(pw::rpc2::Reader<TestStreamResponse> reader,
                               RunToCompletion(h().client().TestServerStreamRpc(
                                   {.integer = 1,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::Aborted().code())})));
  ExpectNext(reader, 0);
  EXPECT_EQ(RunToCompletion(reader.Read()).status(),
            pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2ServiceAdapterAllNonOkResponsesTest,
       DeliverNonOkResponsesWithNoArgsDeliversAllUnaryMethods) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(TestResponse first_unary,
                               RunToCompletion(h().client().TestUnaryRpc(
                                   {.integer = 9,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::Aborted().code())})));
  EXPECT_EQ(first_unary.value, 10);

  pw::rpc2::UnaryFuture<TestRequest, TestResponse> second_unary =
      h().client().TestAnotherUnaryRpc({.integer = 2});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(
      h().RunInTaskUntil(second_unary,
                         [&] { return service.unary_responder.active(); })
          .IsPending());
  PW_TEST_EXPECT_OK(service.unary_responder.Finish(
      {.value = 55, .repeated_field = {}}, pw::Status::Aborted()));
  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> second_response =
      h().RunInTaskUntil(second_unary,
                         [&] { return second_unary.is_complete(); });
  ASSERT_TRUE(second_response.IsReady());
  PW_TEST_ASSERT_OK(second_response->status());
  EXPECT_EQ((*second_response)->value, 55);
}

TEST(Rpc2ServicePolicyConstructionTest, SupportsInPlaceAndLvalueTag) {
  pw::allocator::test::AllocatorForTest<4096> allocator;
  pw::thread::test::TestThreadContext thread;
  SharedServer holder(allocator, thread.options(), {});

  // Non-movable Rpc2ServiceAdapter and Rpc2ServiceFor construct in place (as
  // required by pw::RuntimeInitGlobal, pw::NoDestructor, and emplace), and
  // accept non-const lvalue policy tags without matching forwarding `Args&&`.
  auto policy = pw::rpc::DeliverNonOkResponses<
      LegacyService::ServiceInfo::TestUnaryRpc>();
  std::optional<pw::rpc::Rpc2ServiceFor<LegacyService>> owned;
  owned.emplace(holder.adapter, policy);

  OtherRawLegacyService raw_service;
  std::optional<pw::rpc::Rpc2ServiceAdapter> wrapped;
  wrapped.emplace(
      holder.adapter, pw::rpc::DeliverNonOkResponses(), raw_service);

  holder.adapter.Stop();
}

TEST_F(Rpc2AdapterTest, ConcurrentUnaryCallsGetTheirOwnResponses) {
  using Task =
      pw::async2::FutureTask<pw::rpc2::UnaryFuture<TestRequest, TestResponse>>;
  std::array<Task, 3> tasks = {
      Task(h().client().TestUnaryRpc({.integer = 10})),
      Task(h().client().TestUnaryRpc({.integer = 20})),
      Task(h().client().TestUnaryRpc({.integer = 30})),
  };
  for (auto& task : tasks) {
    h().dispatcher().Post(task);
  }
  ASSERT_TRUE(h().RunUntil([&tasks] {
    return std::all_of(tasks.begin(), tasks.end(), [](const Task& task) {
      return task.has_value();
    });
  }));

  int32_t expected = 11;
  for (auto& task : tasks) {
    ASSERT_TRUE(task.has_value());
    PW_TEST_ASSERT_OK(task.value().status());
    EXPECT_EQ(task.value()->value, expected);
    expected += 10;
  }
  EXPECT_EQ(h().legacy_service().unary_calls.load(), 3);
}

TEST_F(Rpc2AdapterTest, AsynchronousUnaryResponse) {
  pw::rpc2::UnaryFuture<TestRequest, TestResponse> call =
      h().client().TestAnotherUnaryRpc({.integer = 99});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(
      h().RunInTaskUntil(call, [&] { return service.unary_responder.active(); })
          .IsPending());
  EXPECT_EQ(service.last_request.load(), 99);

  // Respond outside the legacy handler.
  PW_TEST_EXPECT_OK(h().legacy_service().unary_responder.Finish(
      {.value = 7, .repeated_field = {}}));

  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> response =
      h().RunInTaskUntil(call, [&] { return call.is_complete(); });
  ASSERT_TRUE(response.IsReady());
  PW_TEST_ASSERT_OK(response->status());
  EXPECT_EQ((*response)->value, 7);
}

TEST_F(Rpc2AdapterTest, ServerStream) { ExpectServerStreamWorks(); }

TEST_F(Rpc2AdapterTest, ServerStreamWithErrorStatusCancelsCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(pw::rpc2::Reader<TestStreamResponse> reader,
                               RunToCompletion(h().client().TestServerStreamRpc(
                                   {.integer = 2,
                                    .status_code = static_cast<uint32_t>(
                                        pw::Status::Aborted().code())})));
  ExpectNext(reader, 0);
  ExpectNext(reader, 1);
  EXPECT_EQ(RunToCompletion(reader.Read()).status(),
            pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterTest, ServerStreamWrittenOutsideHandler) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -1})));
  pw::rpc::PwpbServerWriter<TestStreamResponse>& writer =
      h().legacy_service().writer;
  ASSERT_TRUE(h().RunUntil([&writer] { return writer.active(); }));

  for (uint32_t i = 0; i < 3; ++i) {
    PW_TEST_EXPECT_OK(writer.Write({.chunk = {}, .number = 100 + i}));
    ExpectNext(reader, 100 + i);
  }
  PW_TEST_EXPECT_OK(writer.Finish());
  ExpectEndOfStream(reader);
}

TEST_F(Rpc2AdapterTest, LegacyWritesBeyondQueueLimitFail) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -1})));
  pw::rpc::PwpbServerWriter<TestStreamResponse>& writer =
      h().legacy_service().writer;
  ASSERT_TRUE(h().RunUntil([&writer] { return writer.active(); }));

  // Nothing drains the queue until the dispatcher runs.
  for (uint32_t i = 0; i < kQueueLimit; ++i) {
    PW_TEST_EXPECT_OK(writer.Write({.chunk = {}, .number = i}));
  }
  // pw_rpc reports any ChannelOutput failure as UNKNOWN.
  EXPECT_EQ(writer.Write({.chunk = {}, .number = kQueueLimit}),
            pw::Status::Unknown());

  for (uint32_t i = 0; i < kQueueLimit; ++i) {
    ExpectNext(reader, i);
  }
  // Once drained, writes succeed again.
  PW_TEST_EXPECT_OK(writer.Write({.chunk = {}, .number = 50}));
  ExpectNext(reader, 50);
  PW_TEST_EXPECT_OK(writer.Finish());
  ExpectEndOfStream(reader);
}

TEST_F(Rpc2AdapterTest, LegacyWritesFailWhenAllocatorIsExhausted) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -1})));
  pw::rpc::PwpbServerWriter<TestStreamResponse>& writer =
      h().legacy_service().writer;
  ASSERT_TRUE(h().RunUntil([&writer] { return writer.active(); }));

  h().adapter_allocator().DisableAllocate();
  // pw_rpc reports any ChannelOutput failure as UNKNOWN.
  EXPECT_EQ(writer.Write({.chunk = {}, .number = 1}), pw::Status::Unknown());

  h().adapter_allocator().EnableAllocate();
  PW_TEST_EXPECT_OK(writer.Write({.chunk = {}, .number = 2}));
  ExpectNext(reader, 2);
  PW_TEST_EXPECT_OK(writer.Finish());
  ExpectEndOfStream(reader);
}

TEST_F(Rpc2AdapterTest, ClientStream) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  for (int64_t value : {5, 15, 25}) {
    PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = value})));
  }
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 0})));

  PW_TEST_ASSERT_OK_AND_ASSIGN(TestStreamResponse response,
                               RunToCompletion(std::move(call).response()));
  EXPECT_EQ(response.number, 45u);
}

TEST_F(Rpc2AdapterTest, ClientStreamWithErrorStatusCancelsCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 3})));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write(
      {.integer = 0,
       .status_code = static_cast<uint32_t>(pw::Status::NotFound().code())})));

  EXPECT_EQ(RunToCompletion(std::move(call).response()).status(),
            pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterTest, ClientStreamAllocationFailureAbortsCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 5})));
  ASSERT_TRUE(
      h().RunUntil([this] { return h().legacy_service().sum.load() == 5u; }));

  // The adapter cannot queue the next message for the legacy server.
  h().adapter_allocator().DisableAllocate();
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 6})));
  EXPECT_EQ(RunToCompletion(std::move(call).response()).status(),
            pw::rpc2::RpcStatus::Aborted());
  h().adapter_allocator().EnableAllocate();

  // Ending the pw_rpc2 call cancels the legacy call.
  EXPECT_TRUE(
      h().RunUntil([this] { return !h().legacy_service().reader.active(); }));
}

TEST_F(Rpc2AdapterTest, BidirectionalStream) {
  ExpectBidirectionalStreamWorks();
}

TEST_F(Rpc2AdapterTest, BidirectionalStreamWithErrorStatusCancelsCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto stream, RunToCompletion(h().client().TestBidirectionalStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Write({.integer = 5})));
  ExpectNext(stream.reader(), 5);
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Write(
      {.integer = 0,
       .status_code = static_cast<uint32_t>(pw::Status::Aborted().code())})));

  EXPECT_EQ(RunToCompletion(stream.reader().Read()).status(),
            pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterTest, BidirectionalStreamHalfClose) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto stream, RunToCompletion(h().client().TestBidirectionalStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Write({.integer = 3})));
  ExpectNext(stream.reader(), 3);
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Finish()));

  // The legacy service can keep streaming after the client closes its stream.
  auto& reader_writer = h().legacy_service().reader_writer;
  PW_TEST_EXPECT_OK(reader_writer.Write({.chunk = {}, .number = 77}));
  ExpectNext(stream.reader(), 77);
  PW_TEST_EXPECT_OK(reader_writer.Finish());
  ExpectEndOfStream(stream.reader());
}

TEST_F(Rpc2AdapterTest, ClientCancellingCallCancelsLegacyCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -1})));
  pw::rpc::PwpbServerWriter<TestStreamResponse>& writer =
      h().legacy_service().writer;
  ASSERT_TRUE(h().RunUntil([&writer] { return writer.active(); }));

  reader.Cancel();
  EXPECT_TRUE(h().RunUntil([&writer] { return !writer.active(); }));
}

TEST_F(Rpc2AdapterTest, ClientDisconnectingCancelsLegacyCalls) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto stream, RunToCompletion(h().client().TestBidirectionalStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(stream.writer().Write({.integer = 1})));
  ExpectNext(stream.reader(), 1);
  ASSERT_TRUE(h().legacy_service().reader_writer.active());

  PW_TEST_EXPECT_OK(RunToCompletion(h().rpc2_client().Close()));
  EXPECT_TRUE(h().RunUntil(
      [this] { return !h().legacy_service().reader_writer.active(); }));
}

TEST_F(Rpc2AdapterRawTest, RawLegacyService) {
  // The raw service echoes the encoded request. `TestRequest.integer` and
  // `TestResponse.value` are both field 1, so the echo decodes as a response.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 1234})));
  EXPECT_EQ(response.value, 1234);
}

TEST_F(Rpc2AdapterRawTest, RawLegacyServiceErrorCancelsCall) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = 1})));
  EXPECT_EQ(RunToCompletion(reader.Read()).status(),
            pw::rpc2::RpcStatus::Cancelled());
}

TEST_F(Rpc2AdapterTest, ManySequentialCalls) {
  for (int64_t i = 0; i < 50; ++i) {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        TestResponse response,
        RunToCompletion(h().client().TestUnaryRpc({.integer = i})));
    EXPECT_EQ(response.value, i + 1);
  }
}

TEST_F(Rpc2AdapterDeepQueueTest, ServerStreamBurstFromHandler) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = 20})));
  for (uint32_t i = 0; i < 20; ++i) {
    ExpectNext(reader, i);
  }
  ExpectEndOfStream(reader);
}

TEST_F(Rpc2AdapterTest, CallEndsWhileLegacyHandlerIsBlocked) {
  {
    pw::async2::FutureTask task(h().client().TestUnaryRpc({.integer = -1}));
    h().dispatcher().Post(task);
    h().dispatcher().RunUntilStalled();
    h().legacy_service().blocked.acquire();
    // Destroying the task drops the call, which cancels it.
    task.Deregister();
  }

  // The pw_rpc2 side tears the call down without waiting for the handler.
  h().dispatcher().RunUntilStalled();

  // The handler's response, once it returns, is dropped.
  h().legacy_service().unblock.release();

  // The adapter keeps working.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 5})));
  EXPECT_EQ(response.value, 6);
  EXPECT_EQ(h().legacy_service().unary_calls.load(), 2);
}

TEST_F(Rpc2AdapterTest, CallCancelledWhileRequestQueuedDoesNotRunHandler) {
  // Park the adapter's worker thread in a blocking unary handler.
  pw::async2::FutureTask blocker(h().client().TestUnaryRpc({.integer = -1}));
  h().dispatcher().Post(blocker);
  h().dispatcher().RunUntilStalled();
  h().legacy_service().blocked.acquire();

  // Start and immediately cancel a second call while its REQUEST is queued.
  {
    pw::async2::FutureTask cancelled(
        h().client().TestUnaryRpc({.integer = 41}));
    h().dispatcher().Post(cancelled);
    h().dispatcher().RunUntilStalled();
    cancelled.Deregister();
  }
  h().dispatcher().RunUntilStalled();

  h().legacy_service().unblock.release();
  ASSERT_TRUE(h().RunUntil([&blocker] { return blocker.has_value(); }));
  PW_TEST_ASSERT_OK(blocker.value().status());

  // Complete one more call to flush the worker queue; the cancelled call's
  // handler must not have run.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 5})));
  EXPECT_EQ(response.value, 6);
  EXPECT_EQ(h().legacy_service().unary_calls.load(), 2);
}

TEST_F(Rpc2AdapterTest, ClientStreamCancelledWhileMessageQueuedDropsMessage) {
  std::optional<
      pw::async2::FutureTask<pw::rpc2::UnaryFuture<TestRequest, TestResponse>>>
      blocker;
  {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        auto call, RunToCompletion(h().client().TestClientStreamRpc()));
    PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 10})));
    ASSERT_TRUE(h().RunUntil(
        [this] { return h().legacy_service().sum.load() == 10u; }));

    // Park the worker thread, then queue another client stream message and
    // cancel the call before the worker can process that message.
    blocker.emplace(h().client().TestUnaryRpc({.integer = -1}));
    h().dispatcher().Post(*blocker);
    h().dispatcher().RunUntilStalled();
    h().legacy_service().blocked.acquire();

    PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 20})));
    h().dispatcher().RunUntilStalled();
  }
  h().dispatcher().RunUntilStalled();

  h().legacy_service().unblock.release();
  ASSERT_TRUE(h().RunUntil([&blocker] { return blocker->has_value(); }));
  PW_TEST_ASSERT_OK(blocker->value().status());
  ASSERT_TRUE(
      h().RunUntil([this] { return !h().legacy_service().reader.active(); }));
  EXPECT_EQ(h().legacy_service().sum.load(), 10u);
}

TEST_F(Rpc2AdapterTest,
       CallCancelledDuringRequestCancelsBeforeQueuedNextRequest) {
  // Start a server stream whose handler pauses inside ProcessPacket(REQUEST)
  // after saving its writer.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> first_reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -2})));
  h().dispatcher().RunUntilStalled();
  h().legacy_service().blocked.acquire();

  // Cancel the first call while its REQUEST is still running (`call->cancel` is
  // not yet populated), then queue a second call to the same method behind it.
  first_reader.Cancel();
  h().dispatcher().RunUntilStalled();

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<TestStreamResponse> second_reader,
      RunToCompletion(h().client().TestServerStreamRpc({.integer = -1})));
  h().dispatcher().RunUntilStalled();

  // Let the first handler return. ProcessPacket() must cancel the first legacy
  // call immediately (invoking `on_error(CANCELLED)`) before processing the
  // second call's REQUEST, which moves a new writer into the same slot.
  h().legacy_service().unblock.release();
  ASSERT_TRUE(h().RunUntil(
      [this] { return h().legacy_service().server_stream_calls.load() == 2; }));
  EXPECT_EQ(h().legacy_service().stream_errors.load(), 1);
  EXPECT_EQ(h().legacy_service().last_stream_error.load(),
            pw::Status::Cancelled().code());

  PW_TEST_EXPECT_OK(
      h().legacy_service().writer.Write({.chunk = {}, .number = 99}));
  ExpectNext(second_reader, 99);
  PW_TEST_EXPECT_OK(h().legacy_service().writer.Finish());
  ExpectEndOfStream(second_reader);
}

TEST_F(Rpc2AdapterTest, InvalidRequestPayloadEndsWithDataError) {
  constexpr std::array<std::byte, 2> kCorruptProto = {std::byte{0xff},
                                                      std::byte{0xff}};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::UnaryReservation<TestResponse> reservation,
      RunToCompletion(h().client().TestUnaryRpc(kCorruptProto.size())));
  std::copy(kCorruptProto.begin(), kCorruptProto.end(), reservation.data());
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::ResponseFuture<TestResponse> response_future,
      std::move(reservation).Commit(kCorruptProto.size()));
  EXPECT_EQ(RunToCompletion(std::move(response_future)).status(),
            pw::rpc2::RpcStatus::DataError());
}

TEST_F(Rpc2AdapterTest, InvalidClientStreamPayloadEndsWithDataError) {
  constexpr std::array<std::byte, 2> kCorruptProto = {std::byte{0xff},
                                                      std::byte{0xff}};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 10})));
  PW_TEST_ASSERT_OK(RunToCompletion(
      call.writer().WriteCopy(pw::ConstBuf::Unowned(kCorruptProto))));

  EXPECT_EQ(RunToCompletion(std::move(call).response()).status(),
            pw::rpc2::RpcStatus::DataError());
}

TEST_F(Rpc2AdapterTest, ClientStreamHalfClose) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto call, RunToCompletion(h().client().TestClientStreamRpc()));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Write({.integer = 12})));
  PW_TEST_ASSERT_OK(RunToCompletion(call.writer().Finish()));

#if !PW_RPC_COMPLETION_REQUEST_CALLBACK
  ASSERT_TRUE(
      h().RunUntil([this] { return h().legacy_service().sum.load() == 12u; }));
  PW_TEST_EXPECT_OK(h().legacy_service().reader.Finish(
      {.chunk = {}, .number = h().legacy_service().sum.load()}));
#endif  // !PW_RPC_COMPLETION_REQUEST_CALLBACK

  PW_TEST_ASSERT_OK_AND_ASSIGN(TestStreamResponse response,
                               RunToCompletion(std::move(call).response()));
  EXPECT_EQ(response.number, 12u);
}

TEST_F(Rpc2AdapterTest, BindingAllocationFailureEndsWithUnavailable) {
  // The connection's first call cannot allocate its channel binding.
  h().adapter_allocator().DisableAllocate();
  EXPECT_EQ(RunToCompletion(h().client().TestUnaryRpc({.integer = 1})).status(),
            pw::rpc2::RpcStatus::Unavailable());

  h().adapter_allocator().EnableAllocate();
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 1})));
  EXPECT_EQ(response.value, 2);
}

TEST_F(Rpc2AdapterTest, RequestAllocationFailureEndsWithUnavailable) {
  // The connection already has its channel binding, so only the request packet
  // fails to allocate.
  const uint32_t channel_id = ChannelIdOf(h().rpc2_client());
  h().adapter_allocator().DisableAllocate();
  EXPECT_EQ(RunToCompletion(h().client().TestUnaryRpc({.integer = 1})).status(),
            pw::rpc2::RpcStatus::Unavailable());
  h().adapter_allocator().EnableAllocate();

  // The failed call released the binding: once the connection closes, a new
  // connection takes the binding over.
  PW_TEST_EXPECT_OK(RunToCompletion(h().rpc2_client().Close()));
  RetireClosedConnections();
  pw::rpc2::Client second = h().Connect();
  EXPECT_EQ(ChannelIdOf(second), channel_id);
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
}

TEST_F(Rpc2AdapterTest, AsyncTryFinishAllocationFailureCancelsLegacyCall) {
  pw::rpc2::UnaryFuture<TestRequest, TestResponse> call =
      h().client().TestAnotherUnaryRpc({.integer = 1});
  LegacyService& service = h().legacy_service();
  EXPECT_TRUE(
      h().RunInTaskUntil(call, [&] { return service.unary_responder.active(); })
          .IsPending());

  // TryFinish() leaves the legacy call open when sending fails; ending the
  // pw_rpc2 call must still cancel the open legacy call.
  h().adapter_allocator().DisableAllocate();
  EXPECT_EQ(
      service.unary_responder.TryFinish({.value = 2, .repeated_field = {}}),
      pw::Status::Unknown());
  EXPECT_TRUE(service.unary_responder.active());

  pw::async2::Poll<pw::rpc2::RpcResult<TestResponse>> response =
      h().RunInTaskUntil(call, [&] { return call.is_complete(); });
  ASSERT_TRUE(response.IsReady());
  EXPECT_EQ(response->status(), pw::rpc2::RpcStatus::Aborted());
  h().adapter_allocator().EnableAllocate();

  EXPECT_TRUE(h().RunUntil([&] { return !service.unary_responder.active(); }));
}

TEST_F(Rpc2AdapterTest, InHandlerTryFinishAllocationFailureCancelsLegacyCall) {
  // Park the worker thread so `TestAnotherUnaryRpc`'s REQUEST packet can be
  // queued before disabling allocations.
  pw::async2::FutureTask blocker(h().client().TestUnaryRpc({.integer = -1}));
  h().dispatcher().Post(blocker);
  h().dispatcher().RunUntilStalled();
  h().legacy_service().blocked.acquire();

  pw::async2::FutureTask call(
      h().client().TestAnotherUnaryRpc({.integer = -1}));
  h().dispatcher().Post(call);
  h().dispatcher().RunUntilStalled();

  h().adapter_allocator().DisableAllocate();
  h().legacy_service().unblock.release();

  ASSERT_TRUE(h().RunUntil([&] { return call.has_value(); }));
  EXPECT_EQ(call.value().status(), pw::rpc2::RpcStatus::Aborted());
  EXPECT_EQ(h().legacy_service().last_try_finish_status.load(),
            pw::Status::Unknown().code());
  EXPECT_TRUE(h().RunUntil(
      [this] { return !h().legacy_service().unary_responder.active(); }));

  h().adapter_allocator().EnableAllocate();
  ASSERT_TRUE(h().RunUntil([&] { return blocker.has_value(); }));
}

TEST_F(Rpc2AdapterTest, ReopensChannelClosedOutsideAdapter) {
  const uint32_t channel_id = ChannelIdOf(h().rpc2_client());
  PW_TEST_ASSERT_OK(h().legacy_server().CloseChannel(channel_id));

  // The connection's next call opens the channel again.
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), channel_id);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 1})));
  EXPECT_EQ(response.value, 2);
}

// The tests above use Rpc2ServiceFor, which owns its legacy service. These
// check that Rpc2ServiceAdapter serves an existing legacy service the same way.

TEST_F(Rpc2ServiceAdapterTest, Unary) { ExpectUnaryCallWorks(); }

TEST_F(Rpc2ServiceAdapterTest, ServerStream) { ExpectServerStreamWorks(); }

TEST_F(Rpc2ServiceAdapterTest, BidirectionalStream) {
  ExpectBidirectionalStreamWorks();
}

TEST_F(Rpc2ServiceAdapterTest, UnregisteredServiceIsUnimplemented) {
  h().legacy_server().UnregisterService(h().legacy_service());
  EXPECT_EQ(RunToCompletion(h().client().TestUnaryRpc({.integer = 1})).status(),
            pw::rpc2::RpcStatus::Unimplemented());

  h().legacy_server().RegisterService(h().legacy_service());
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 41})));
  EXPECT_EQ(response.value, 42);
}

// Rpc2Adapter owns an internal pw::rpc::Server instead of sharing an
// external one.

TEST_F(Rpc2AdapterInternalServerTest, Unary) { ExpectUnaryCallWorks(); }

TEST_F(Rpc2AdapterInternalServerTest, ServerStream) {
  ExpectServerStreamWorks();
}

TEST_F(Rpc2AdapterInternalServerTest, BidirectionalStream) {
  ExpectBidirectionalStreamWorks();
}

TEST_F(Rpc2AdapterInternalServerTest, ConnectionsGetTheirOwnChannels) {
  ExpectConnectionsGetTheirOwnChannels();
}

TEST_F(Rpc2AdapterTwoServicesTest, CallsReachTheServiceWithTheirId) {
  ExpectUnaryCallWorks();

  // The raw service for PwRpcTestService echoes the request.
  static constexpr std::array<std::byte, 3> kPayload = {
      std::byte{1}, std::byte{2}, std::byte{3}};
  OtherServiceClient other_client(h().rpc2_client());
  PW_TEST_ASSERT_OK_AND_ASSIGN(pw::ConstBuf response,
                               RunToCompletion(other_client.TestUnaryRpc(
                                   pw::ConstBuf::Unowned(kPayload))));
  const pw::ConstByteSpan echoed = response;
  EXPECT_TRUE(std::equal(
      echoed.begin(), echoed.end(), kPayload.begin(), kPayload.end()));
  EXPECT_EQ(h().legacy_service().unary_calls.load(), 1);
}

// Channel assignment: each pw_rpc2 connection is its own legacy channel.

TEST_F(Rpc2AdapterTest, ConnectionsGetTheirOwnChannels) {
  ExpectConnectionsGetTheirOwnChannels();
}

TEST_F(Rpc2AdapterFirstChannelIdTest, ConnectionsGetTheirOwnChannels) {
  ExpectConnectionsGetTheirOwnChannels();
}

TEST_F(Rpc2AdapterTest, ChannelIdPersistsForTheConnection) {
  const uint32_t first_id = ChannelIdOf(h().rpc2_client());
  pw::rpc2::Client second = h().Connect();
  const uint32_t second_id = ChannelIdOf(second);
  EXPECT_NE(first_id, second_id);

  // Each connection keeps its ID across calls, though it had no calls in
  // progress in between.
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), first_id);
  EXPECT_EQ(ChannelIdOf(second), second_id);
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), first_id);
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
}

TEST_F(Rpc2AdapterTest, ClosedConnectionChannelIdIsReused) {
  const uint32_t first_id = ChannelIdOf(h().rpc2_client());

  pw::rpc2::Client second = h().Connect();
  const uint32_t second_id = ChannelIdOf(second);
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
  RetireClosedConnections();

  // A later connection takes over the closed connection's channel.
  pw::rpc2::Client third = h().Connect();
  EXPECT_EQ(ChannelIdOf(third), second_id);
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), first_id);
  PW_TEST_EXPECT_OK(RunToCompletion(third.Close()));
}

TEST_F(Rpc2AdapterTest, SkipsChannelIdsInUse) {
  constexpr uint32_t kTakenId = pw::rpc::Rpc2AdapterConfig().first_channel_id;
  CapturingOutput other_output;
  PW_TEST_ASSERT_OK(h().legacy_server().OpenChannel(kTakenId, other_output));

  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), kTakenId + 1);
  PW_TEST_EXPECT_OK(h().legacy_server().CloseChannel(kTakenId));
}

TEST_F(Rpc2AdapterTest, SkipsChannelIdTakenAfterConnectionsOpen) {
  const uint32_t first_id = ChannelIdOf(h().rpc2_client());

  // The project takes the ID the next connection would get.
  CapturingOutput other_output;
  PW_TEST_ASSERT_OK(
      h().legacy_server().OpenChannel(first_id + 1, other_output));

  pw::rpc2::Client second = h().Connect();
  EXPECT_EQ(ChannelIdOf(second), first_id + 2);
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
  PW_TEST_EXPECT_OK(h().legacy_server().CloseChannel(first_id + 1));
}

TEST_F(Rpc2AdapterTest, NoFreeChannelSlotEndsWithUnavailable) {
  if (PW_RPC_DYNAMIC_ALLOCATION) {
    GTEST_SKIP()
        << "Channels are allocated dynamically, so slots never run out";
  }

  // The project uses every channel slot.
  std::array<CapturingOutput, SharedServer::kLegacyChannelSlots> outputs;
  for (uint32_t i = 0; i < outputs.size(); ++i) {
    PW_TEST_ASSERT_OK(h().legacy_server().OpenChannel(i + 1, outputs[i]));
  }
  EXPECT_EQ(RunToCompletion(h().client().TestUnaryRpc({.integer = 1})).status(),
            pw::rpc2::RpcStatus::Unavailable());

  // Once a slot is free, the connection's next call opens its channel.
  PW_TEST_ASSERT_OK(h().legacy_server().CloseChannel(1));
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 1})));
  EXPECT_EQ(response.value, 2);

  for (uint32_t i = 1; i < outputs.size(); ++i) {
    PW_TEST_EXPECT_OK(h().legacy_server().CloseChannel(i + 1));
  }
}

TEST_F(Rpc2AdapterOneChannelTest, MaxChannelsLimitsConnections) {
  const uint32_t first_id = ChannelIdOf(h().rpc2_client());

  pw::rpc2::Client second = h().Connect();
  EXPECT_EQ(
      RunToCompletion(Rpc2Client(second).TestUnaryRpc({.integer = 1})).status(),
      pw::rpc2::RpcStatus::Unavailable());

  // Once the first connection closes, the second takes over its channel.
  PW_TEST_EXPECT_OK(RunToCompletion(h().rpc2_client().Close()));
  RetireClosedConnections();
  EXPECT_EQ(ChannelIdOf(second), first_id);
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
}

TEST_F(Rpc2AdapterMaxChannelIdTest, TakenMaxChannelIdEndsWithUnavailable) {
  constexpr uint32_t kMaxId = std::numeric_limits<uint32_t>::max();
  CapturingOutput other_output;
  PW_TEST_ASSERT_OK(h().legacy_server().OpenChannel(kMaxId, other_output));

  EXPECT_EQ(RunToCompletion(h().client().TestUnaryRpc({.integer = 1})).status(),
            pw::rpc2::RpcStatus::Unavailable());

  // Once the channel is freed, the connection can still open `kMaxId`.
  PW_TEST_ASSERT_OK(h().legacy_server().CloseChannel(kMaxId));
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), kMaxId);
}

TEST_F(Rpc2AdapterMaxChannelIdTest,
       SecondConnectionBeyondMaxChannelIdEndsWithUnavailable) {
  constexpr uint32_t kMaxId = std::numeric_limits<uint32_t>::max();
  EXPECT_EQ(ChannelIdOf(h().rpc2_client()), kMaxId);

  pw::rpc2::Client second = h().Connect();
  EXPECT_EQ(
      RunToCompletion(Rpc2Client(second).TestUnaryRpc({.integer = 1})).status(),
      pw::rpc2::RpcStatus::Unavailable());
  PW_TEST_EXPECT_OK(RunToCompletion(second.Close()));
}

TEST_F(Rpc2AdapterRawTest, LegacyAndRpc2ClientsShareTheServer) {
  // A legacy transport's channel on the same server, as for a UART.
  constexpr uint32_t kLegacyChannelId = 1;
  CapturingOutput legacy_output;
  PW_TEST_ASSERT_OK(
      h().legacy_server().OpenChannel(kLegacyChannelId, legacy_output));

  // A legacy client calls the raw echo service directly. Methods are in
  // declaration order, so TestUnaryRpc is first.
  constexpr uint32_t kCallId = 7;
  constexpr std::array<std::byte, 3> kPayload = {
      std::byte{1}, std::byte{2}, std::byte{3}};
  const pw::rpc::internal::Packet request(
      pw::rpc::internal::pwpb::PacketType::REQUEST,
      kLegacyChannelId,
      pw::rpc::internal::UnwrapServiceId(h().legacy_service().service_id()),
      MethodLookup::MethodIds<RawLegacyService>()[0],
      kCallId,
      kPayload);
  std::array<std::byte, 64> buffer;
  PW_TEST_ASSERT_OK_AND_ASSIGN(pw::ConstByteSpan encoded,
                               request.Encode(buffer));
  PW_TEST_EXPECT_OK(h().legacy_server().ProcessPacket(encoded));

  ASSERT_EQ(legacy_output.packets(), 1);
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc::internal::Packet response,
      pw::rpc::internal::Packet::FromBuffer(legacy_output.last_packet()));
  EXPECT_EQ(response.type(), pw::rpc::internal::pwpb::PacketType::RESPONSE);
  EXPECT_EQ(response.call_id(), kCallId);
  EXPECT_TRUE(std::equal(response.payload().begin(),
                         response.payload().end(),
                         kPayload.begin(),
                         kPayload.end()));

  // A pw_rpc2 client calls the same service through the adapter.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse rpc2_response,
      RunToCompletion(h().client().TestUnaryRpc({.integer = 1234})));
  EXPECT_EQ(rpc2_response.value, 1234);
  EXPECT_EQ(RunToCompletion(h().client().TestAnotherUnaryRpc({.integer = 1234}))
                .status(),
            pw::rpc2::RpcStatus::Cancelled());

  PW_TEST_EXPECT_OK(h().legacy_server().CloseChannel(kLegacyChannelId));
}

TEST_F(Rpc2AdapterRawNonOkResponseTest,
       OptedInRawUnaryMethodDeliversPayloadOnNonOkStatus) {
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      TestResponse response,
      RunToCompletion(h().client().TestAnotherUnaryRpc({.integer = 1234})));
  EXPECT_EQ(response.value, 1234);
}

#endif  // PW_THREAD_JOINING_ENABLED

}  // namespace
