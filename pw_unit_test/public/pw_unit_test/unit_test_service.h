// Copyright 2020 The Pigweed Authors
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

#include <mutex>
#include <span>

#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_string/string.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/thread_core.h"
#include "pw_unit_test/config.h"
#include "pw_unit_test/event_handler.h"
#include "pw_unit_test/internal/rpc_event_handler.h"
#include "pw_unit_test_proto/unit_test.pwpb.h"
#include "pw_unit_test_proto/unit_test.raw_rpc.pb.h"

namespace pw::unit_test {

class UnitTestThread : public thread::ThreadCore {
 public:
  UnitTestThread()
      : service_(*this), handler_(*this), running_(false), verbose_(false) {}

  class Service final : public pw_rpc::raw::UnitTest::Service<Service> {
   public:
    constexpr Service(UnitTestThread& thread) : thread_(thread) {}

    void Run(ConstByteSpan request, RawServerWriter& writer);

   private:
    UnitTestThread& thread_;
  };

  constexpr Service& service() { return service_; }

 private:
  friend class internal::RpcEventHandler;

  static constexpr size_t kMaxTestSuiteNameLength = 64;
  static constexpr size_t kMaxTestNameLength = 64;
  static constexpr size_t kMaxTestFilters = 16;

  struct TestCaseFilterStorage {
    InlineString<kMaxTestSuiteNameLength> suite_name;
    InlineString<kMaxTestNameLength> test_name;
  };

  bool running() {
    std::lock_guard lock(mutex_);
    return running_;
  }

  // Returns all configured test run options to their default values.
  void Reset();
  void ResetLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void set_verbose(bool verbose) { verbose_ = verbose; }

  Status ScheduleTestRun(rpc::RawServerWriter&& writer,
                         span<const TestCaseFilter> test_filters_to_run);

  void Run() final;

  template <typename WriteFunction>
  void WriteEvent(WriteFunction event_writer) {
    pwpb::Event::MemoryEncoder event(encoding_buffer_);
    event_writer(event);
    if (event.status().ok()) {
      writer_.Write(event).IgnoreError();
    }
  }

  void WriteTestRunStart();
  void WriteTestRunEnd(const RunTestsSummary& summary);
  void WriteTestCaseStart(const TestCase& test_case);
  void WriteTestCaseEnd(TestResult result);
  void WriteTestCaseDisabled(const TestCase& test_case);
  void WriteTestCaseExpectation(const TestExpectation& expectation);

  Service service_;
  internal::RpcEventHandler handler_;
  Vector<TestCaseFilterStorage, kMaxTestFilters> test_filters_to_run_;
  std::array<std::byte, config::kEventBufferSize> encoding_buffer_ = {};
  pw::sync::Mutex mutex_;
  pw::sync::ThreadNotification notification_;

  rpc::RawServerWriter writer_;
  bool running_ PW_GUARDED_BY(mutex_);
  bool verbose_;
};

}  // namespace pw::unit_test
