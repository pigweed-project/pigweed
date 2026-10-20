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
#pragma once

#include "pw_span/span.h"
#include "pw_unit_test/event_handler.h"
#include "pw_unit_test/framework.h"

namespace pw::unit_test {

class UnitTestThread;

namespace internal {

// GoogleTest event handler that streams test events through an RPC service.
class RpcEventHandler : public testing::EmptyTestEventListener {
 public:
  RpcEventHandler(UnitTestThread& thread);
  void ExecuteTests(span<const TestCaseFilter> test_filters);

  void OnTestProgramStart(const testing::UnitTest& unit_test) override;
  void OnTestProgramEnd(const testing::UnitTest& unit_test) override;
  void OnTestStart(const testing::TestInfo& test_info) override;
  void OnTestEnd(const testing::TestInfo& test_info) override;
  void OnTestPartResult(
      const testing::TestPartResult& test_part_result) override;
  void OnTestDisabled(const testing::TestInfo& test_info) override;

 private:
  UnitTestThread& thread_;
};

}  // namespace internal
}  // namespace pw::unit_test
