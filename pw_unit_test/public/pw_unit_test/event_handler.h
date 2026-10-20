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

#include <string_view>

namespace pw {

namespace unit_test {

/// @submodule{pw_unit_test,events}

/// The result of a complete test run.
enum class TestResult {
  kSuccess = 0,
  kFailure = 1,
  // Test skipped at runtime. This is neither a success nor a failure.
  kSkipped = 2,
};

struct TestCase {
  /// Name of the test suite to which this test case belongs.
  const char* suite_name;

  /// Name of the test case.
  const char* test_name;

  /// Path to the file in which the test case (`TEST` macro) is defined.
  const char* file_name;
};

struct TestCaseFilter {
  /// Name of the test suite(s) to run.
  std::string_view suite_name;

  /// Name of the test case(s) to run.
  std::string_view test_name;
};

struct TestExpectation {
  /// The source code for the expression which was run.
  const char* expression;

  /// The expression with arguments evaluated.
  const char* evaluated_expression;

  /// Path to the file where the expectation is located. This file may be
  /// different from the `file_name` in `TestCase`.
  const char* file_name;

  /// Line number at which the expectation is located.
  int line_number;

  /// Whether the expectation succeeded.
  bool success;
};

struct RunTestsSummary {
  /// The number of passed tests among the run tests.
  int passed_tests;

  /// The number of passed tests among the run tests.
  int failed_tests;

  /// The number of tests skipped or filtered out.
  int skipped_tests;

  /// The number of disabled tests encountered.
  int disabled_tests;
};

struct ProgramSummary {
  /// The total number of tests to run in the program.
  int tests_to_run;

  /// The number of test suites included in the program.
  int test_suites;

  /// Test summary for the program once complete.
  RunTestsSummary tests_summary;
};

struct TestSuite {
  /// Name of the test suite.
  const char* name;

  /// Total number of tests in suite to run.
  int test_to_run_count;
};

/// Collects and processes the results of a unit test run. Its interface is
/// called by the unit test framework as tests are executed and various test
/// events occur.
///
/// A program wanting to process test events must define a class implementing
/// the ``pw::unit_test::EventHandler`` interface and register it with the
/// framework. When ``RUN_ALL_TESTS()`` is called, ``pw_unit_test`` notifies
/// the handler of various events which occur in the test process.
/// For example, consider a file containing the following test definitions:
///
/// @code{.cpp}
///   TEST(MyTestSuite, MyFirstCase) {
///     EXPECT_TRUE(true);
///   }
///
///   TEST(MyTestSuite, MySecondCase) {
///     EXPECT_TRUE(false);
///   }
/// @endcode
///
/// There's one test suite consisting of two test cases. When ``pw_unit_test``
/// starts running the first test case (``MyFirstCase``), it dispatches a
/// ``TestCaseStart`` event to the event handler. It then runs the body of the
/// test, sequentially checking each expectation within. After each expectation,
/// a
/// ``TestCaseExpect`` event is sent to the event handler with the expectation's
/// result. In this case, there's only one, which passes successfully. Finally,
/// after the test is finished, a ``TestCaseEnd`` event is dispatched with the
/// overall result of the test case. ``pw_unit_test`` then runs ``MySecondCase``
/// in the same way.
class EventHandler {
 public:
  virtual ~EventHandler() = default;

  /// Called before any test activity starts.
  virtual void TestProgramStart(const ProgramSummary& program_summary) = 0;

  /// Called after environment setup for each iteration of tests ends.
  virtual void EnvironmentsSetUpEnd() = 0;

  /// Called before the test suite starts.
  virtual void TestSuiteStart(const TestSuite& test_suite) = 0;

  /// Called after the test suite ends.
  virtual void TestSuiteEnd(const TestSuite& test_suite) = 0;

  /// Called after environment teardown for each iteration of tests ends.
  virtual void EnvironmentsTearDownEnd() = 0;

  /// Called after all test activities have ended.
  virtual void TestProgramEnd(const ProgramSummary& program_summary) = 0;

  /// Called before all tests are run.
  virtual void RunAllTestsStart() = 0;

  /// Called after all tests are run.
  virtual void RunAllTestsEnd(const RunTestsSummary& run_tests_summary) = 0;

  /// Called when a new test case is started.
  virtual void TestCaseStart(const TestCase& test_case) = 0;

  /// Called when a test case completes. The overall result of the test case is
  /// provided.
  virtual void TestCaseEnd(const TestCase& test_case, TestResult result) = 0;

  /// Called when a disabled test case is encountered.
  virtual void TestCaseDisabled(const TestCase&) {}

  /// Called after each expect or assert statement within a test case with the
  /// result.
  virtual void TestCaseExpect(const TestCase& test_case,
                              const TestExpectation& expectation) = 0;
};

/// Sets the event handler for a test run. Must be called before
/// `RUN_ALL_TESTS()` to receive test output. Set `event_handler` to null
/// to disable event handling.
///
/// @warning This method is not thread-safe.
void RegisterEventHandler(EventHandler* event_handler);

/// @}

}  // namespace unit_test

}  // namespace pw
