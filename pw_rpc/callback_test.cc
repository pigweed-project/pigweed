// Copyright 2022 The Pigweed Authors
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

#include <atomic>

#include "pw_rpc/dynamic_call.h"
#include "pw_rpc/raw/client_testing.h"
#include "pw_rpc_test_protos/test.raw_rpc.pb.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_thread/non_portable_test_thread_options.h"
#include "pw_thread/sleep.h"
#include "pw_thread/thread.h"
#include "pw_thread/yield.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc {
namespace {

using namespace std::chrono_literals;

using test::pw_rpc::raw::TestService;

// These tests cover interactions between a thread moving or destroying an RPC
// call object and a thread running callbacks for that call. In order to test
// that the first thread waits for callbacks to complete when trying to move or
// destroy the call, it is necessary to have the callback thread yield to the
// other thread. There isn't a good way to synchronize these threads without
// changing the code under test.
void YieldToOtherThread() {
  // Sleep for a while and then yield just to be sure the other thread runs.
  this_thread::sleep_for(100ms);
  this_thread::yield();
}

#if PW_RPC_DYNAMIC_ALLOCATION
// Counts call object deletions, which may happen on the callback thread.
struct CountingCallDeleter {
  static inline std::atomic<int> deletions{0};

  void operator()(RawClientReaderWriter* call) const {
    deletions.fetch_add(1);
    delete call;
  }
};
using CountingCallPtr =
    std::unique_ptr<RawClientReaderWriter, CountingCallDeleter>;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

class CallbacksTest : public ::testing::Test {
 protected:
  CallbacksTest()
      // TODO: b/290860904 - Replace TestOptionsThread0 with
      // TestThreadContext.
      : callback_thread_(thread::test::TestOptionsThread0(),
                         [this] { SendResponseAfterSemaphore(); }) {}

  ~CallbacksTest() override {
    EXPECT_FALSE(callback_thread_.joinable());  // Tests must join the thread!
  }

  void RespondToCall(const RawClientReaderWriter& call) {
    respond_to_call_ = &call;
  }

  RawClientTestContext<> context_;
  sync::BinarySemaphore callback_thread_sem_;
  sync::BinarySemaphore main_thread_sem_;

  Thread callback_thread_;

  // Must be incremented exactly once by the RPC callback in each test.
  volatile int callback_executed_ = 0;

  // Variables optionally used by tests. These are in this object so lambads
  // only need to capture [this] to access them.
  volatile bool call_is_in_scope_ = false;

  RawClientReaderWriter call_1_;
  RawClientReaderWriter call_2_;
#if PW_RPC_DYNAMIC_ALLOCATION
  DynamicCallPtr<RawClientReaderWriter, CountingCallPtr> dynamic_call_;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

 private:
  void SendResponseAfterSemaphore() {
    // Wait until the main thread says to send the response.
    callback_thread_sem_.acquire();

    context_.server().SendServerStream<TestService::TestBidirectionalStreamRpc>(
        {}, respond_to_call_->id());
  }

  const RawClientReaderWriter* respond_to_call_ = &call_1_;
};

TEST_F(CallbacksTest, DestructorWaitsUntilCallbacksComplete) {
  if (PW_RPC_USE_GLOBAL_MUTEX == 0) {
    callback_thread_sem_.release();
    callback_thread_.join();
    GTEST_SKIP()
        << "Skipping because locks are disabled, so this thread cannot yield.";
  }

  {
    RawClientReaderWriter local_call = TestService::TestBidirectionalStreamRpc(
        context_.client(), context_.channel().id());
    RespondToCall(local_call);

    call_is_in_scope_ = true;

    local_call.set_on_next([this](ConstByteSpan) {
      main_thread_sem_.release();

      // Wait for a while so the main thread tries to destroy the call.
      YieldToOtherThread();

      // Now, make sure the call is still in scope. The main thread should
      // block in the call's destructor until this callback completes.
      EXPECT_TRUE(call_is_in_scope_);

      callback_executed_ = callback_executed_ + 1;
    });

    // Start the callback thread so it can invoke the callback.
    callback_thread_sem_.release();

    // Wait until the callback thread starts.
    main_thread_sem_.acquire();
  }

  // The callback thread will sleep for a bit. Meanwhile, let the call go out
  // of scope, and mark it as such.
  call_is_in_scope_ = false;

  // Wait for the callback thread to finish.
  callback_thread_.join();

  EXPECT_EQ(callback_executed_, 1);
}

TEST_F(CallbacksTest,
       DestructorWaitsUntilCallbacksComplete_AfterMovingInClosedCall) {
  if (PW_RPC_USE_GLOBAL_MUTEX == 0) {
    callback_thread_sem_.release();
    callback_thread_.join();
    GTEST_SKIP()
        << "Skipping because locks are disabled, so this thread cannot yield.";
  }

  {
    RawClientReaderWriter local_call = TestService::TestBidirectionalStreamRpc(
        context_.client(), context_.channel().id());
    RespondToCall(local_call);

    call_is_in_scope_ = true;

    local_call.set_on_next([this](ConstByteSpan) {
      main_thread_sem_.release();

      // Wait for a while so the main thread tries to destroy the call.
      YieldToOtherThread();

      // Now, make sure the call is still in scope. The main thread should
      // block in the call's destructor until this callback completes.
      EXPECT_TRUE(call_is_in_scope_);

      callback_executed_ = callback_executed_ + 1;
    });

    // Start the callback thread so it can invoke the callback.
    callback_thread_sem_.release();

    // Wait until the callback thread starts.
    main_thread_sem_.acquire();

    // Move a call that was closed with CloseAndWaitForCallbacks() onto
    // local_call while local_call's callback is running. local_call's
    // destructor must still wait for that callback.
    RawClientReaderWriter closed_call = TestService::TestBidirectionalStreamRpc(
        context_.client(), context_.channel().id());
    closed_call.CloseAndWaitForCallbacks();
    local_call = std::move(closed_call);
  }

  // Wait for the callback thread to finish.
  callback_thread_.join();

  call_is_in_scope_ = false;

  EXPECT_EQ(callback_executed_, 1);
}

TEST_F(CallbacksTest, MoveActiveCall_WaitsForCallbackToComplete) {
  if (PW_RPC_USE_GLOBAL_MUTEX == 0) {
    callback_thread_sem_.release();
    callback_thread_.join();
    GTEST_SKIP()
        << "Skipping because locks are disabled, so this thread cannot yield.";
  }

  call_1_ = TestService::TestBidirectionalStreamRpc(
      context_.client(), context_.channel().id(), [this](ConstByteSpan) {
        main_thread_sem_.release();  // Confirm that this thread started

        YieldToOtherThread();

        callback_executed_ = callback_executed_ + 1;
      });

  // Start the callback thread so it can invoke the callback.
  callback_thread_sem_.release();

  // Confirm that the callback thread started.
  main_thread_sem_.acquire();

  // Move the call object. This thread should wait until the on_completed
  // callback is done.
  EXPECT_TRUE(call_1_.active());
  call_2_ = std::move(call_1_);

  // The callback should already have finished. This thread should have waited
  // for it to finish during the move.
  EXPECT_EQ(callback_executed_, 1);
  EXPECT_FALSE(call_1_.active());
  EXPECT_TRUE(call_2_.active());

  callback_thread_.join();
}

TEST_F(CallbacksTest, MoveOtherCallIntoOwnCallInCallback) {
  call_1_ = TestService::TestBidirectionalStreamRpc(
      context_.client(), context_.channel().id(), [this](ConstByteSpan) {
        main_thread_sem_.release();  // Confirm that this thread started

        call_1_ = std::move(call_2_);

        callback_executed_ = callback_executed_ + 1;
      });

  call_2_ = TestService::TestBidirectionalStreamRpc(context_.client(),
                                                    context_.channel().id());

  EXPECT_TRUE(call_1_.active());
  EXPECT_TRUE(call_2_.active());

  // Start the callback thread and wait for it to finish.
  callback_thread_sem_.release();
  callback_thread_.join();

  EXPECT_EQ(callback_executed_, 1);
  EXPECT_TRUE(call_1_.active());
  EXPECT_FALSE(call_2_.active());
}

TEST_F(CallbacksTest, MoveOwnCallInCallback) {
  call_1_ = TestService::TestBidirectionalStreamRpc(
      context_.client(), context_.channel().id(), [this](ConstByteSpan) {
        main_thread_sem_.release();  // Confirm that this thread started

        // Cancel this call first, or the move will deadlock, since the moving
        // thread will wait for the callback thread (both this thread) to
        // terminate if the call is active.
        EXPECT_EQ(OkStatus(), call_1_.Cancel());
        call_2_ = std::move(call_1_);

        callback_executed_ = callback_executed_ + 1;
      });

  call_2_ = TestService::TestBidirectionalStreamRpc(context_.client(),
                                                    context_.channel().id());

  EXPECT_TRUE(call_1_.active());
  EXPECT_TRUE(call_2_.active());

  // Start the callback thread and wait for it to finish.
  callback_thread_sem_.release();
  callback_thread_.join();

  EXPECT_EQ(callback_executed_, 1);
  EXPECT_FALSE(call_1_.active());
  EXPECT_FALSE(call_2_.active());
}

TEST_F(CallbacksTest, PacketDroppedIfOnNextIsBusy) {
  call_1_ = TestService::TestBidirectionalStreamRpc(
      context_.client(), context_.channel().id(), [this](ConstByteSpan) {
        main_thread_sem_.release();  // Confirm that this thread started

        callback_thread_sem_.acquire();  // Wait for the main thread to release

        callback_executed_ = callback_executed_ + 1;
      });

  // Start the callback thread.
  callback_thread_sem_.release();

  main_thread_sem_.acquire();  // Confirm that the callback is running

  // Handle a few packets for this call, which should be dropped since on_next
  // is busy. callback_executed_ should remain at 1.
  for (int i = 0; i < 5; ++i) {
    context_.server().SendServerStream<TestService::TestBidirectionalStreamRpc>(
        {}, call_1_.id());
  }

  // Wait for the callback thread to finish.
  callback_thread_sem_.release();
  callback_thread_.join();

  EXPECT_EQ(callback_executed_, 1);
}

#if PW_RPC_DYNAMIC_ALLOCATION
TEST_F(CallbacksTest, DynamicCall_ResetWhileCallbackIsRunningDoesNotBlock) {
  CountingCallDeleter::deletions = 0;
  dynamic_call_ = DynamicCallPtr(CountingCallPtr(
      new RawClientReaderWriter(TestService::TestBidirectionalStreamRpc(
          context_.client(), context_.channel().id()))));
  RespondToCall(*dynamic_call_);

  dynamic_call_->set_on_next([this](ConstByteSpan) {
    main_thread_sem_.release();
    callback_thread_sem_.acquire();
    EXPECT_EQ(dynamic_call_, nullptr);
    callback_executed_ = callback_executed_ + 1;
  });

  callback_thread_sem_.release();
  main_thread_sem_.acquire();

  // Resetting dynamic_call_ while on_next is blocked on callback_thread_sem_
  // must return immediately without waiting for the callback to finish.
  dynamic_call_.reset();
  EXPECT_EQ(callback_executed_, 0);
  // The call object is deleted on the callback thread after on_next returns.
  EXPECT_EQ(CountingCallDeleter::deletions.load(), 0);

  callback_thread_sem_.release();
  callback_thread_.join();
  EXPECT_EQ(callback_executed_, 1);
  EXPECT_EQ(CountingCallDeleter::deletions.load(), 1);
}
#endif  // PW_RPC_DYNAMIC_ALLOCATION

}  // namespace
}  // namespace pw::rpc
