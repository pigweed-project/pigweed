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
#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/future_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/service.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/status.h"
#include "pw_status/status.h"

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
#include "pw_async2/coro.h"
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

namespace pw::rpc2::test {

/// @submodule{pw_rpc2,testing}

/// End-to-end dependency-injection test fixture for a `pw_rpc2` service or
/// client.
///
/// Instantiates and connects a real `Server` (hosting `ServiceImpl`) and a
/// generated `ServiceClient` over an in-memory `PairedConnection` on a single
/// `DispatcherForTest`. Tests drive RPCs through the generated client API
/// using `RunToCompletion()`, `RunInTaskUntilStalled()`, or `dispatcher()`.
template <typename ServiceImpl, size_t kAllocatorSizeBytes = 16384>
class TestHarness {
 public:
  static_assert(std::is_base_of_v<::pw::rpc2::Service, ServiceImpl>,
                "ServiceImpl must derive from a generated pw_rpc2 Service "
                "base class (which inherits from pw::rpc2::Service).");

  using ServiceClient = typename ServiceImpl::PwInternal_Client;
  using RawClient = typename ServiceImpl::PwInternal_RawClient;
  using PwpbClient = typename ServiceImpl::PwInternal_PwpbClient;

  template <typename... Args,
            typename =
                std::enable_if_t<std::is_constructible_v<ServiceImpl, Args...>>>
  explicit TestHarness(Args&&... service_args)
      : service_(std::forward<Args>(service_args)...),
        server_(allocator_, dispatcher_) {
    PW_CHECK_OK(server_.RegisterService(service_));
    PW_CHECK_OK(server_.RegisterListenerBlocking(transport_));
    server_.Start();

    PairedConnections paired = MakePairedConnections(allocator_);
    transport_.ResolveAccept(std::move(paired.second));

    RpcResult<Client> client = RunToCompletion(
        Client::Connect(dispatcher_, allocator_, std::move(paired.first)));
    PW_CHECK_OK(client.status());
    service_client_ = ServiceClient(*client);
  }

  ~TestHarness() {
    service_client_ = ServiceClient();
    transport_.ResolveAccept(Status::Cancelled());
    PW_CHECK_OK(RunToCompletion(server_.Close()));
  }

  TestHarness(const TestHarness&) = delete;
  TestHarness& operator=(const TestHarness&) = delete;
  TestHarness(TestHarness&&) = delete;
  TestHarness& operator=(TestHarness&&) = delete;

  /// Returns the connected `pw::rpc2::Client`.
  const Client& client() const { return service_client_.client(); }

  /// Returns the generated service client matching `ServiceImpl`'s flavor
  /// (`raw` or `pwpb`), bound to the connected `Client`.
  const ServiceClient& service_client() const { return service_client_; }

  /// Returns a generated `raw` service client bound to the connected `Client`.
  /// Requires the `.raw.rpc2.h` header for this service to be included.
  RawClient raw_client() const { return RawClient(client()); }

  /// Returns a generated `pwpb` service client bound to the connected `Client`.
  /// Requires the `.pwpb.rpc2.h` header for this service to be included.
  PwpbClient pwpb_client() const { return PwpbClient(client()); }

  /// Returns the service instance under test.
  ServiceImpl& service() { return service_; }
  const ServiceImpl& service() const { return service_; }

  /// Returns the shared test dispatcher driving both client and server.
  pw::async2::DispatcherForTest& dispatcher() { return dispatcher_; }

  /// Returns the test allocator (which also implicitly converts to
  /// `pw::async2::CoroContext`).
  pw::Allocator& allocator() { return allocator_; }

  /// Advances the dispatcher until all ready tasks have stalled.
  bool RunUntilStalled() { return dispatcher_.RunUntilStalled(); }

  /// Runs a persistent future or coroutine in a task until no further progress
  /// can be made, returning its `Poll` state without destroying the future.
  template <typename Fut>
  auto RunInTaskUntilStalled(Fut& future) {
    async2::FutureTask<Fut&, async2::ReturnValuePolicy::kKeep> task(future);
    dispatcher_.Post(task);
    dispatcher_.RunUntilStalled();
    task.Deregister();
    return task.TakePoll();
  }

  /// Drives `runnable` to completion on the test dispatcher and returns its
  /// result (or `void`). Crashes with `PW_CHECK` if the dispatcher stalls
  /// before `runnable` completes.
  ///
  /// Accepts:
  /// 1. Any `pw::async2::Future` (e.g. `tester.service_client().MyUnary(req)`
  ///    or a `pw::async2::Coro<T>`).
  /// 2. Any `pw::async2::Task` lvalue.
  /// 3. A coroutine callable `(pw::async2::CoroContext) ->
  ///    pw::async2::Coro<T>`.
  template <typename T>
  auto RunToCompletion(T&& runnable) {
    using Decayed = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (async2::Future<Decayed>) {
      using ValueType = typename Decayed::value_type;
      async2::Poll<ValueType> poll = RunInTaskUntilStalled(runnable);
      PW_CHECK(poll.IsReady(),
               "TestHarness::RunToCompletion() stalled before the future "
               "completed.");
      if constexpr (!std::is_void_v<ValueType>) {
        return std::move(*poll);
      }
    } else if constexpr (std::is_base_of_v<async2::Task, Decayed>) {
      static_assert(std::is_lvalue_reference_v<T&&>,
                    "pw::async2::Task must be passed as an lvalue to "
                    "RunToCompletion().");
      dispatcher_.Post(runnable);
      dispatcher_.RunUntilStalled();
      const bool completed = !runnable.IsRegistered();
      runnable.Deregister();
      PW_CHECK(completed,
               "TestHarness::RunToCompletion() stalled before the task "
               "completed.");
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
    } else if constexpr (std::is_invocable_v<T&, async2::CoroContext&>) {
      async2::CoroContext coro_cx(allocator_);
      auto coro = runnable(coro_cx);
      return RunToCompletion(std::move(coro));
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
    } else {
      static_assert(
          !std::is_same_v<T, T>,
          "RunToCompletion() requires a pw::async2::Future, a pw::async2::Task "
          "lvalue, or a coroutine callable taking pw::async2::CoroContext.");
    }
  }

 private:
  // Declared first so it outlives everything allocated from it, including the
  // dispatcher's posted tasks and connection buffers.
  pw::allocator::test::AllocatorForTest<kAllocatorSizeBytes> allocator_;
  pw::async2::DispatcherForTest dispatcher_;
  MockTransport transport_;
  // Declared before `server_` so that it is destroyed after active calls are
  // torn down during `~TestHarness()`.
  ServiceImpl service_;
  Server server_;
  ServiceClient service_client_;
};

/// @endsubmodule

}  // namespace pw::rpc2::test
