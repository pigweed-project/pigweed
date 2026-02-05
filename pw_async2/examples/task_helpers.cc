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

#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future_task.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

pw::async2::Poll<int> ReadSensor(pw::async2::Context& cx) {
  static bool read_value = false;
  if (read_value) {
    return cx.Unschedule();  // For test use, only produce one value
  }
  read_value = true;
  return pw::async2::Ready(2468);
}

float ConvertSensorReadingToValue(int reading) {
  return static_cast<float>(reading);
}

// DOCSTAG: [pw_async2-examples-future-task-owned]
int FutureTaskOwnsTheFuture(pw::async2::Dispatcher& dispatcher,
                            pw::async2::ValueFuture<int>&& future) {
  pw::async2::FutureTask task(std::move(future));
  dispatcher.Post(task);
  return task.Wait();  // Join the FutureTask and return its value
}
// DOCSTAG: [pw_async2-examples-future-task-owned]

// DOCSTAG: [pw_async2-examples-future-task-ref]
int FutureTaskReferencesTheFuture(pw::async2::Dispatcher& dispatcher,
                                  pw::async2::ValueFuture<int>& future) {
  pw::async2::FutureTask task(future);
  dispatcher.Post(task);
  return task.Wait();  // Join the FutureTask and return its value
}
// DOCSTAG: [pw_async2-examples-future-task-ref]

}  // namespace

class TaskHelpersExampleTest : public ::testing::Test {
 public:
  TaskHelpersExampleTest()
      : thread_(pw::Thread(context_.options(), [this] {
          dispatcher_.RunToCompletionUntilReleased();
        })) {}

  ~TaskHelpersExampleTest() override {
    dispatcher_.Release();
    thread_.join();
  }

  pw::async2::DispatcherForTest dispatcher_;

 private:
  pw::thread::test::TestThreadContext context_;
  pw::Thread thread_;
};

class ExampleClass {
 public:
  pw::async2::Poll<> RunCalibration(pw::async2::Context&) {
    return pw::async2::Ready();
  }

  void RunCalibrationTask(pw::async2::Dispatcher& dispatcher) {
    // DOCSTAG: [pw_async2-examples-func-task-class]
    pw::async2::FuncTask task(
        [this](pw::async2::Context& cx) { return RunCalibration(cx); });
    dispatcher.Post(task);
    // DOCSTAG: [pw_async2-examples-func-task-class]
    task.Join();
  }
};

TEST_F(TaskHelpersExampleTest, FuncTaskLambda) {
  pw::async2::ValueProvider<float> float_provider;
  pw::async2::FutureTask future_task(float_provider.Get());
  dispatcher_.Post(future_task);

  // DOCSTAG: [pw_async2-examples-func-task]
  pw::async2::FuncTask task(
      [&float_provider](pw::async2::Context& cx) -> pw::async2::Poll<> {
        while (true) {
          // Read a raw sensor sample when one is available.
          PW_TRY_READY_ASSIGN(int raw_sample, ReadSensor(cx));

          // Convert the raw values to standard units.
          float sensor_value = ConvertSensorReadingToValue(raw_sample);
          float_provider.Resolve(sensor_value);
        }
      });
  dispatcher_.Post(task);
  // DOCSTAG: [pw_async2-examples-func-task]

  EXPECT_EQ(future_task.Wait(), 2468.f);
}

TEST_F(TaskHelpersExampleTest, FuncTaskMemberFunction) {
  ExampleClass object;
  object.RunCalibrationTask(dispatcher_);
}

TEST_F(TaskHelpersExampleTest, FutureTaskOwnsTheFuture) {
  EXPECT_EQ(FutureTaskOwnsTheFuture(
                dispatcher_, pw::async2::ValueFuture<int>::Resolved(1234)),
            1234);
}

TEST_F(TaskHelpersExampleTest, FutureTaskReferencesTheFuture) {
  auto future = pw::async2::ValueFuture<int>::Resolved(1234);
  EXPECT_EQ(FutureTaskReferencesTheFuture(dispatcher_, future), 1234);
}

TEST_F(TaskHelpersExampleTest, RunOnceTask) {
  // DOCSTAG: [pw_async2-examples-run-once]
  // Calculate the 8th fibonacci number on the dispatcher thread.
  pw::async2::RunOnceTask task([] {
    int a = 0, b = 1;
    for (int i = 3; i <= 8; i++) {
      a = std::exchange(b, a + b);
    }
    return b;
  });
  dispatcher_.Post(task);

  int result = task.Wait();  // returns 13
  // DOCSTAG: [pw_async2-examples-run-once]
  EXPECT_EQ(result, 13);
}
