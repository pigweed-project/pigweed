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

// pw_perf_test main for pw_system targets. pw_system starts the scheduler
// before main() would run, so performance tests are run from a thread started
// by UserAppInit() instead. Results are logged through pw_system's log
// pipeline.

#include "pw_perf_test/event_handler.h"
#include "pw_perf_test/logging_event_handler.h"
#include "pw_perf_test/perf_test.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_system/config.h"
#include "pw_system/log.h"
#include "pw_system/target_hooks.h"
#include "pw_thread/attrs.h"
#include "pw_thread/context.h"
#include "pw_thread/priority.h"
#include "pw_thread/thread.h"

namespace pw::system {
namespace {

// Wraps LoggingEventHandler to flush pw_system's log thread before tests start
// and between test cases (outside timed iterations) so the lower-priority
// LogThread drains its log buffer without dropping results on large benchmark
// suites or interfering with measurements.
class DrainingLoggingEventHandler final : public perf_test::EventHandler {
 public:
  void RunAllTestsStart(const perf_test::TestRunInfo& summary) override {
    handler_.RunAllTestsStart(summary);
    GetLogThread().PollUntilDrained();
  }

  void RunAllTestsEnd() override {
    handler_.RunAllTestsEnd();
    GetLogThread().PollUntilDrained();
  }

  void TestCaseStart(const perf_test::TestCase& info) override {
    handler_.TestCaseStart(info);
  }

  void TestCaseIteration(const perf_test::TestIteration& iteration) override {
    handler_.TestCaseIteration(iteration);
  }

  void TestCaseEnd(const perf_test::TestCase& info,
                   const perf_test::TestMeasurement& measurement) override {
    handler_.TestCaseEnd(info, measurement);
    GetLogThread().PollUntilDrained();
  }

 private:
  perf_test::LoggingEventHandler handler_;
};

// Run at VeryHigh priority (below Highest, where e.g. an RTOS timer daemon may
// run) so that lower-priority system threads do not preempt measurements,
// while system timer commands are still processed promptly.
constexpr ThreadAttrs kPerfTestThread =
    ThreadAttrs()
        .set_name("PerfTestThread")
        .set_stack_size_bytes(kPerfTestThreadStackSizeBytes)
        .set_priority(ThreadPriority::VeryHigh());

PW_CONSTINIT ThreadContextFor<kPerfTestThread> perf_test_thread_context;

}  // namespace

void UserAppInit() {
  Thread(perf_test_thread_context, [] {
    DrainingLoggingEventHandler handler;
    perf_test::RunAllTests(handler);
  }).detach();
}

}  // namespace pw::system
