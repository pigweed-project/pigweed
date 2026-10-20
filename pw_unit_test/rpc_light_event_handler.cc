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

#include "pw_unit_test/internal/rpc_event_handler.h"
#include "pw_unit_test/unit_test_service.h"

namespace pw::unit_test::internal {

RpcEventHandler::RpcEventHandler(UnitTestThread& thread) : thread_(thread) {}

void RpcEventHandler::ExecuteTests(span<const TestCaseFilter> test_filters) {
  RegisterEventHandler(this);
  SetTestFilters(test_filters);

  PW_LOG_DEBUG("%u test filters applied",
               static_cast<unsigned>(test_filters.size()));

  RUN_ALL_TESTS();

  RegisterEventHandler(nullptr);
  SetTestFilters({});
}

void RpcEventHandler::RunAllTestsStart() { thread_.WriteTestRunStart(); }

void RpcEventHandler::RunAllTestsEnd(const RunTestsSummary& run_tests_summary) {
  thread_.WriteTestRunEnd(run_tests_summary);
}

void RpcEventHandler::TestCaseStart(const TestCase& test_case) {
  thread_.WriteTestCaseStart(test_case);
}

void RpcEventHandler::TestCaseEnd(const TestCase&, TestResult result) {
  thread_.WriteTestCaseEnd(result);
}

void RpcEventHandler::TestCaseExpect(const TestCase&,
                                     const TestExpectation& expectation) {
  thread_.WriteTestCaseExpectation(expectation);
}

void RpcEventHandler::TestCaseDisabled(const TestCase& test_case) {
  thread_.WriteTestCaseDisabled(test_case);
}

}  // namespace pw::unit_test::internal
