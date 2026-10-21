// Copyright 2024 The Pigweed Authors
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

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_thread/attrs.h"
#include "pw_thread/context.h"
#include "pw_thread/native_options.h"
#include "pw_thread/stack.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

#if PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

// If a thread can't be joined, it's unknown whether the stack/context is safe
// to reuse, so the test can only be run once. This macro ensures the test only
// runs once and fails on subsequent attempts, so the user doesn't assume a
// pass.
#define FAIL_IF_TEST_ALREADY_RAN()                       \
  do {                                                   \
    static std::atomic_flag test_ran = ATOMIC_FLAG_INIT; \
    if (test_ran.test_and_set()) {                       \
      FAIL();                                            \
    }                                                    \
  } while (false)

constexpr pw::ThreadAttrs kThread0 = pw::ThreadAttrs().set_stack_size_bytes(0);
constexpr pw::ThreadAttrs kThread1023 =
    pw::ThreadAttrs().set_name("hello world").set_stack_size_bytes(1023);
constexpr pw::ThreadAttrs kThread1024 =
    pw::ThreadAttrs(kThread1023).set_stack_size_bytes(1024);
[[maybe_unused]] constexpr pw::ThreadAttrs kThread1025 =
    pw::ThreadAttrs(kThread1024).set_stack_size_bytes(1025);

PW_CONSTINIT pw::ThreadStack<0> stack_0;
PW_CONSTINIT pw::ThreadStack<1024> stack_1024;

[[maybe_unused]] PW_CONSTINIT pw::ThreadContext<> must_be_constinit_1;
[[maybe_unused]] PW_CONSTINIT pw::ThreadContext<1024> must_be_constinit_2;

constexpr pw::ThreadAttrs kThreadExternalMin =
    pw::ThreadAttrs().set_stack(stack_0);
constexpr pw::ThreadAttrs kThreadExternal =
    pw::ThreadAttrs().set_stack(stack_1024);

class ThreadCreationTest : public ::testing::Test {
 public:
  ~ThreadCreationTest() override {
    EXPECT_EQ(expected_thread_runs_, thread_runs_);
  }

  auto TestThread() {
    expected_thread_runs_ += 1;
    return [this] { thread_runs_ += 1; };
  }

 private:
  int expected_thread_runs_ = 0;
  int thread_runs_ = 0;
};

class ThreadCreationNoJoinTest : public ::testing::Test {
 public:
  ~ThreadCreationNoJoinTest() override { EXPECT_TRUE(was_thread_run_); }

  auto TestThread() {
    return [this] {
      was_thread_run_ = true;
      thread_complete_.release();
    };
  }

 protected:
  pw::sync::BinarySemaphore thread_complete_;

 private:
  bool was_thread_run_ = false;
};

TEST_F(ThreadCreationNoJoinTest, StartThreadWithContextWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContext<1024> context_1024;

  pw::Thread(context_1024, kThread1024, TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContext<1024> context_1024;

  pw::Thread(pw::GetThreadOptions<kThread1024>(context_1024), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadGetOptionsStaticAttrsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContext<1024> context_1024;

  pw::Thread(pw::GetThreadOptions(context_1024, kThread1024), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadGetOptionsStackSizeSmallerThanStackStaticAttrsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContext<1024> context_1024;

  pw::Thread(pw::GetThreadOptions<kThread1023>(context_1024), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadGetOptionsStackSizeSmallerThanStackWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContext<1024> context_1024;

  pw::Thread(pw::GetThreadOptions(context_1024, kThread1023), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadDefaultContextWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::DefaultThreadContext default_context;

  pw::Thread(default_context, pw::ThreadAttrs(), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadDefaultContextGetOptionsStaticAttrsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::DefaultThreadContext default_context;
  static constexpr pw::ThreadAttrs kDefaultAttrs;

  pw::Thread(pw::GetThreadOptions<kDefaultAttrs>(default_context), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadDefaultContextGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::DefaultThreadContext default_context;

  pw::Thread(pw::GetThreadOptions(default_context, pw::ThreadAttrs()),
             TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadExtStackWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  pw::ThreadContext<> context;  // external stack

  pw::Thread(context, pw::ThreadAttrs().set_stack(stack), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadExtStackGetOptionsStaticAttrsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  static constexpr pw::ThreadAttrs kAttrs = pw::ThreadAttrs().set_stack(stack);
  pw::ThreadContext<> context;  // external stack

  pw::Thread(pw::GetThreadOptions<kAttrs>(context), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadExtStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  pw::ThreadContext<> context;  // external stack

  pw::Thread(pw::GetThreadOptions(context, pw::ThreadAttrs().set_stack(stack)),
             TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadContextWithStackButStaticAttrsWithExtStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  // This ThreadContext allocates space for a stack, but it is not used.
  static pw::ThreadContext<128> context;
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  static constexpr pw::ThreadAttrs kAttrs = pw::ThreadAttrs().set_stack(stack);

  pw::Thread(pw::GetThreadOptions<kAttrs>(context), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadContextWithStackButAttrsWithExtStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  // This ThreadContext allocates space for a stack, but it is not used.
  static pw::ThreadContext<128> context;
  PW_CONSTINIT static pw::ThreadStack<1024> stack;

  pw::Thread(pw::GetThreadOptions(context, pw::ThreadAttrs().set_stack(stack)),
             TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadMinSizeExtStackWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<0> stack_min;
  pw::ThreadContext<> context;  // external stack

  pw::Thread(context, pw::ThreadAttrs().set_stack(stack_min), TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadMinSizeExtStackGetOptionsStaticAttrsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<0> stack_min;
  static constexpr pw::ThreadAttrs kAttrs =
      pw::ThreadAttrs().set_stack(stack_min);
  pw::ThreadContext<> context;  // external stack

  pw::Thread(pw::GetThreadOptions<kAttrs>(context), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadMinSizeExtStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<0> stack_min;
  pw::ThreadContext<> context;  // external stack

  pw::Thread(
      pw::GetThreadOptions(context, pw::ThreadAttrs().set_stack(stack_min)),
      TestThread())
      .detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadWithContextForWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContextFor<kThread1024> context_for;

  pw::Thread(context_for, TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadWithContexForGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContextFor<kThread1024> context_for;

  pw::Thread(pw::GetThreadOptions(context_for), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadWithContextForExtStackWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  static constexpr pw::ThreadAttrs kAttrs = pw::ThreadAttrs().set_stack(stack);
  pw::ThreadContextFor<kAttrs> context;  // external stack

  pw::Thread(context, TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadWithContextForExtStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  PW_CONSTINIT static pw::ThreadStack<1024> stack;
  static constexpr pw::ThreadAttrs kAttrs = pw::ThreadAttrs().set_stack(stack);
  pw::ThreadContextFor<kAttrs> context;  // external stack

  pw::Thread(pw::GetThreadOptions(context), TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest, StartThreadWithContextForMinStackWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContextFor<kThread0> context_for_min_stack;

  pw::Thread(context_for_min_stack, TestThread()).detach();
  thread_complete_.acquire();
}

TEST_F(ThreadCreationNoJoinTest,
       StartThreadWithContextForMinStackGetOptionsWorks) {
  FAIL_IF_TEST_ALREADY_RAN();
  static pw::ThreadContextFor<kThread0> context_for_min_stack;

  pw::Thread(pw::GetThreadOptions(context_for_min_stack), TestThread())
      .detach();
  thread_complete_.acquire();
}

pw::ThreadContext<1024> context_1024;

TEST(ThreadCreationGetOptionsTest, GetOptionsStackSizeLargerThanStackFails) {
#if PW_NC_TEST(StackTooSmall_TemplateArgument)
  PW_NC_EXPECT(
      "ThreadContext stack is too small for the requested ThreadAttrs");
  pw::GetThreadOptions<kThread1025>(context_1024), TestThread();
#elif PW_NC_TEST(StackTooSmall_FunctionArgument)
  PW_NC_EXPECT(
      "PW_ASSERT\(kContextStackSizeBytes >= "
      "attributes.stack_size_bytes\(\)\);");
  [[maybe_unused]] constexpr auto options =
      pw::GetThreadOptions(context_1024, kThread1025);
#endif  // PW_NC_TEST
}

TEST(ThreadCreationGetOptionsTest, GetOptionsExtStackFails) {
  [[maybe_unused]] pw::ThreadContext<> context;  // external stack

#if PW_NC_TEST(NoStack)
  PW_NC_EXPECT("No stack was provided!");
  std::ignore = pw::GetThreadOptions<kThread1024>(context);
#endif  // PW_NC_TEST
}

#if PW_THREAD_JOINING_ENABLED

TEST_F(ThreadCreationTest, ThreadContext) {
  pw::Thread(context_1024, kThread1024, TestThread()).join();

  pw::Thread(pw::GetThreadOptions<kThread1023>(context_1024), TestThread())
      .join();
  pw::Thread(pw::GetThreadOptions(context_1024, kThread1023), TestThread())
      .join();

  pw::Thread(pw::GetThreadOptions<kThread1024>(context_1024), TestThread())
      .join();
  pw::Thread(pw::GetThreadOptions(context_1024, kThread1024), TestThread())
      .join();
}

pw::DefaultThreadContext default_context;

TEST_F(ThreadCreationTest, DefaultThreadContext) {
  pw::Thread(default_context, pw::ThreadAttrs(), TestThread()).join();

  static constexpr pw::ThreadAttrs kDefault;
  pw::Thread(pw::GetThreadOptions<kDefault>(default_context), TestThread())
      .join();
  pw::Thread(pw::GetThreadOptions(default_context, pw::ThreadAttrs()),
             TestThread())
      .join();
}

TEST_F(ThreadCreationTest, ThreadContextExternalStack) {
  pw::ThreadContext<> context;

  pw::Thread(context, kThreadExternal, TestThread()).join();

  pw::Thread(pw::GetThreadOptions<kThreadExternal>(context), TestThread())
      .join();
  pw::Thread(pw::GetThreadOptions(context, kThreadExternal), TestThread())
      .join();
}

TEST_F(ThreadCreationTest,
       ThreadContextWithStackButAttrsWithExternallyAllocatedStack) {
  // This ThreadContext allocates space for a stack, but it is not used.
  pw::ThreadContext<128> context;

  pw::Thread(pw::GetThreadOptions<kThreadExternal>(context), TestThread())
      .join();

  pw::Thread(pw::GetThreadOptions(context, kThreadExternal), TestThread())
      .join();
}

TEST_F(ThreadCreationTest, ThreadContextMinimumSizedExternalStack) {
  pw::ThreadContext<> context;  // external stack

  pw::Thread(context, kThreadExternalMin, TestThread()).join();

  pw::Thread(pw::GetThreadOptions<kThreadExternalMin>(context), TestThread())
      .join();
  pw::Thread(pw::GetThreadOptions(context, kThreadExternalMin), TestThread())
      .join();
}

pw::ThreadContextFor<kThread1024> context_for;

TEST_F(ThreadCreationTest, ThreadContextFor) {
  pw::Thread(context_for, TestThread()).join();

  pw::Thread(pw::GetThreadOptions(context_for), TestThread()).join();
}

TEST_F(ThreadCreationTest, ThreadContextForExternalStack) {
  pw::ThreadContextFor<kThreadExternal> context;

  pw::Thread(context, TestThread()).join();

  pw::Thread(pw::GetThreadOptions(context), TestThread()).join();
}

pw::ThreadContextFor<kThread0> context_for_min_stack;

TEST_F(ThreadCreationTest, ThreadContextForMinimumSizedStack) {
  pw::Thread(context_for_min_stack, TestThread()).join();

  pw::Thread(pw::GetThreadOptions(context_for_min_stack), TestThread()).join();
}

#endif  // PW_THREAD_JOINING_ENABLED

#else  // thread creation is not supported

[[maybe_unused]] constexpr pw::ThreadAttrs kAttrs;

#if PW_NC_TEST(CreationUnsupported_ThreadContext)
PW_NC_EXPECT("Generic thread creation is not yet supported");
pw::ThreadContext<> context;
#elif PW_NC_TEST(CreationUnsupported_ThreadContextFor)
PW_NC_EXPECT("Generic thread creation is not yet supported");
pw::ThreadContextFor<kAttrs> context_for_attrs;
#elif PW_NC_TEST(CreationUnsupported_ThreadStack)
PW_NC_EXPECT("Generic thread creation is not yet supported");
pw::ThreadStack<1000> stack;
#endif  // PW_NC_TEST

#endif  // PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

}  // namespace
