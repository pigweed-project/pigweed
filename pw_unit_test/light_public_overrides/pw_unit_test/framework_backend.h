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

// IWYU pragma: private, include "pw_unit_test/framework.h"

#if defined(GTEST_TEST)
#error \
    "GTEST_TEST is already defined. Make sure googletest headers are not " \
       "included when using the pw_unit_test light backend."
#endif  // GTEST_TEST

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

#include "pw_bytes/alignment.h"
#include "pw_polyfill/standard.h"
#include "pw_preprocessor/compiler.h"
#include "pw_preprocessor/util.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_string/string_builder.h"
#include "pw_unit_test/config.h"
#include "pw_unit_test/event_handler.h"

/// @submodule{pw_unit_test,declaration}

/// @def GTEST_TEST
/// Alias for `TEST`.
#define GTEST_TEST(test_suite_name, test_name)                           \
  _PW_TEST_SUITE_NAMES_MUST_BE_UNIQUE(void /* TEST */, test_suite_name); \
  _PW_TEST(test_suite_name, test_name, ::pw::unit_test::internal::Test)

/// @def TEST
/// Defines a test given the suite name and test case name.
///
/// If `TEST` is conflicting with other code, set `GTEST_DONT_DEFINE_TEST` to
/// 1 and use `GTEST_TEST` instead.
///
/// @param[in] test_suite_name The name of the test suite or collection of
/// tests.
/// @param[in] test_name The name of the test case.
#if !(defined(GTEST_DONT_DEFINE_TEST) && GTEST_DONT_DEFINE_TEST)
#define TEST(test_suite_name, test_name) GTEST_TEST(test_suite_name, test_name)
#endif  // !GTEST_DONT_DEFINE_TEST

/// @def TEST_F
/// Defines a test case using a test fixture.
///
/// @note `TEST_F` may allocate fixtures separately from the stack. Large
/// variables should be stored in test fixture fields, rather than stack
/// variables. This allows the test framework to statically ensure that enough
/// space is available to store these variables.
///
/// @param[in] test_fixture The name of the test fixture class to use.
/// @param[in] test_name The name of the test case.
#define TEST_F(test_fixture, test_name)                                \
  _PW_TEST_SUITE_NAMES_MUST_BE_UNIQUE(int /* TEST_F */, test_fixture); \
  _PW_TEST(test_fixture, test_name, test_fixture)

/// @def FRIEND_TEST
/// Defines a test case from a test suite as a friend class of an implementation
/// class.
///
/// @warning Use of `FRIEND_TEST` is discouraged, because it induces coupling
/// between testing and implementation code. Consider this a last resort only.
///
/// @param[in] test_suite_name The name of the test suite to befriend.
/// @param[in] test_name The name of the test case to befriend.
#define FRIEND_TEST(test_suite_name, test_name) \
  friend class test_suite_name##_##test_name##_Test

/// @}

/// @submodule{pw_unit_test,expectations}

/// @def EXPECT_TRUE
/// Verifies that @p expr evaluates to true.
///
/// @param[in] expr The expression to evaluate.
#define EXPECT_TRUE(expr) _PW_TEST_EXPECT(_PW_TEST_BOOL(expr, true))

/// @def EXPECT_FALSE
/// Verifies that @p expr evaluates to false.
///
/// @param[in] expr The expression to evaluate.
#define EXPECT_FALSE(expr) _PW_TEST_EXPECT(_PW_TEST_BOOL(expr, false))

/// @def EXPECT_EQ
/// Verifies that `lhs == rhs`.
///
/// Does pointer equality on pointers. If used on two C strings, `EXPECT_EQ`
/// tests if they are in the same memory location, not if they have the same
/// value. Use `EXPECT_STREQ` to compare C strings (e.g. `const char*`) by
/// value.
///
/// When comparing a pointer to `NULL` use `EXPECT_EQ(ptr, nullptr)` instead of
/// `EXPECT_EQ(ptr, NULL)`.
///
/// @param[in] lhs The left side of the equality comparison.
/// @param[in] rhs The right side of the equality comparison.
#define EXPECT_EQ(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, ==))

/// @def EXPECT_NE
/// Verifies that `lhs != rhs`.
///
/// Does pointer equality on pointers. If used on two C strings, it tests if
/// they are in different memory locations, not if they have different values.
/// Use `EXPECT_STRNE` to compare C strings (e.g. `const char*`) by value.
///
/// When comparing a pointer to `NULL`, use `EXPECT_NE(ptr, nullptr)` instead
/// of `EXPECT_NE(ptr, NULL)`.

/// @param[in] lhs The left side of the inequality comparison.
/// @param[in] rhs The right side of the inequality comparison.
#define EXPECT_NE(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, !=))

/// @def EXPECT_GT
/// Verifies that `lhs > rhs`.
///
/// @param[in] lhs The left side of the comparison.
/// @param[in] rhs The right side of the comparison.
#define EXPECT_GT(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, >))

/// @def EXPECT_GE
/// Verifies that `lhs >= rhs`.
///
/// @param[in] lhs The left side of the comparison.
/// @param[in] rhs The right side of the comparison.
#define EXPECT_GE(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, >=))

/// @def EXPECT_LT
/// Verifies that `lhs < rhs`.
///
/// @param[in] lhs The left side of the comparison.
/// @param[in] rhs The right side of the comparison.
#define EXPECT_LT(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, <))

/// @def EXPECT_LE
/// Verifies that `lhs <= rhs`.
///
/// @param[in] lhs The left side of the comparison.
/// @param[in] rhs The right side of the comparison.
#define EXPECT_LE(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_OP(lhs, rhs, <=))

/// @def EXPECT_NEAR
/// Verifies that the difference between `lhs` and `rhs` does not exceed the
/// absolute error bound `epsilon`.
///
/// @param[in] lhs The left side of the comparison.
/// @param[in] rhs The right side of the comparison.
/// @param[in] epsilon The maximum difference between `lhs` and `rhs`.
#define EXPECT_NEAR(lhs, rhs, epsilon) \
  _PW_TEST_EXPECT(_PW_TEST_NEAR(lhs, rhs, epsilon))

/// @def EXPECT_FLOAT_EQ
/// Verifies that the two float values `rhs` and `lhs` are approximately
/// equal, to within 4 units in the last place (ULPs) from each other.
///
/// @param[in] lhs The left side of the equality comparison.
/// @param[in] rhs The right side of the equality comparison.
#define EXPECT_FLOAT_EQ(lhs, rhs) \
  _PW_TEST_EXPECT(                \
      _PW_TEST_NEAR(lhs, rhs, 4 * std::numeric_limits<float>::epsilon()))

/// @def EXPECT_DOUBLE_EQ
/// Verifies that the two double values `rhs` and `lhs` are approximately
/// equal, to within 4 units in the last place (ULPs) from each other.
///
/// @param[in] lhs The left side of the equality comparison.
/// @param[in] rhs The right side of the equality comparison.
#define EXPECT_DOUBLE_EQ(lhs, rhs) \
  _PW_TEST_EXPECT(                 \
      _PW_TEST_NEAR(lhs, rhs, 4 * std::numeric_limits<double>::epsilon()))

/// @def EXPECT_STREQ
/// Verifies that the two C strings `lhs` and `rhs` have the same contents.
///
/// @param[in] lhs The left side of the equality comparison.
/// @param[] rhs The right side of the equality comparison.
#define EXPECT_STREQ(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_C_STR(lhs, rhs, ==))

/// @def EXPECT_STRNE
/// Verifies that the two C strings `lhs` and `rhs` have different content.
///
/// @param[in] lhs The left side of the inequality comparison.
/// @param[in] rhs The right side of the inequality comparison.
#define EXPECT_STRNE(lhs, rhs) _PW_TEST_EXPECT(_PW_TEST_C_STR(lhs, rhs, !=))

/// @}

/// @submodule{pw_unit_test,assertions}

/// @def ASSERT_TRUE
/// See `EXPECT_TRUE`.
#define ASSERT_TRUE(expr) _PW_TEST_ASSERT(_PW_TEST_BOOL(expr, true))

/// @def ASSERT_FALSE
/// See `EXPECT_FALSE`.
#define ASSERT_FALSE(expr) _PW_TEST_ASSERT(_PW_TEST_BOOL(expr, false))

/// @def ASSERT_EQ
/// See `EXPECT_EQ`.
#define ASSERT_EQ(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, ==))

/// @def ASSERT_NE
/// See `EXPECT_NE`.
#define ASSERT_NE(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, !=))

/// @def ASSERT_GT
/// See `EXPECT_GT`.
#define ASSERT_GT(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, >))

/// @def ASSERT_GE
/// See `EXPECT_GE`.
#define ASSERT_GE(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, >=))

/// @def ASSERT_LT
/// See `EXPECT_LT`.
#define ASSERT_LT(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, <))

/// @def ASSERT_LE
/// See `EXPECT_LE`.
#define ASSERT_LE(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_OP(lhs, rhs, <=))

/// @def ASSERT_NEAR
/// See `EXPECT_NEAR`.
#define ASSERT_NEAR(lhs, rhs, epsilon) \
  _PW_TEST_ASSERT(_PW_TEST_NEAR(lhs, rhs, epsilon))

/// @def ASSERT_FLOAT_EQ
/// See `EXPECT_FLOAT_EQ`.
#define ASSERT_FLOAT_EQ(lhs, rhs) \
  _PW_TEST_ASSERT(                \
      _PW_TEST_NEAR(lhs, rhs, 4 * std::numeric_limits<float>::epsilon()))

/// @def ASSERT_DOUBLE_EQ
/// See `EXPECT_DOUBLE_EQ`.
#define ASSERT_DOUBLE_EQ(lhs, rhs) \
  _PW_TEST_ASSERT(                 \
      _PW_TEST_NEAR(lhs, rhs, 4 * std::numeric_limits<double>::epsilon()))

/// @def ASSERT_STREQ
/// See `EXPECT_STREQ`.
#define ASSERT_STREQ(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_C_STR(lhs, rhs, ==))

/// @def ASSERT_STRNE
/// See `EXPECT_STRNE`.
#define ASSERT_STRNE(lhs, rhs) _PW_TEST_ASSERT(_PW_TEST_C_STR(lhs, rhs, !=))

/// @}

/// @submodule{pw_unit_test,control}

/// @def ADD_FAILURE
/// Generates a non-fatal failure with a generic message.
#define ADD_FAILURE()                                                      \
  ::pw::unit_test::internal::ReturnHelper() =                              \
      ::pw::unit_test::internal::Framework::Get().CurrentTestExpectSimple( \
          "(line is not executed)",                                        \
          "(line was executed)",                                           \
          __FILE__,                                                        \
          __LINE__,                                                        \
          false)

/// @def GTEST_FAIL
///
/// Alias of `FAIL`.
#define GTEST_FAIL() return ADD_FAILURE()

/// @def GTEST_SKIP
/// Skips test at runtime. Skips are neither successful nor failed. They
/// abort the current function.
#define GTEST_SKIP()                                                      \
  return ::pw::unit_test::internal::ReturnHelper() =                      \
             ::pw::unit_test::internal::Framework::Get().CurrentTestSkip( \
                 __FILE__, __LINE__)

/// @def FAIL
/// Generates a fatal failure with a generic message.
///
/// If this generic name is clashing with other code, set
/// `GTEST_DONT_DEFINE_FAIL` to 1 and use `GTEST_FAIL` instead.
#if !(defined(GTEST_DONT_DEFINE_FAIL) && GTEST_DONT_DEFINE_FAIL)
#define FAIL() GTEST_FAIL()
#endif  // !GTEST_DONT_DEFINE_FAIL

/// @def GTEST_SUCCEED
///
/// Alias of `SUCCEED`.
#define GTEST_SUCCEED()                                                \
  ::pw::unit_test::internal::Framework::Get().CurrentTestExpectSimple( \
      "(success)", "(success)", __FILE__, __LINE__, true)

/// @def SUCCEED
///
/// Generates success with a generic message.
///
/// If this generic name is conflicting with other code, set
/// `GTEST_DONT_DEFINE_SUCCEED` to 1 and use `GTEST_SUCCEED` instead.
#if !(defined(GTEST_DONT_DEFINE_SUCCEED) && GTEST_DONT_DEFINE_SUCCEED)
#define SUCCEED() GTEST_SUCCEED()
#endif  // !GTEST_DONT_DEFINE_SUCCEED

/// @def SCOPED_TRACE
/// SCOPED_TRACE is not supported. This macro does nothing.
// TODO: https://pwbug.dev/441096262 - Add SCOPED_TRACE support.
#define SCOPED_TRACE(message)

/// The `pw_unit_test` framework entrypoint. Runs every registered test case
/// and dispatches the results through the event handler.
///
/// @pre An event handler has been registered before calling `RUN_ALL_TESTS`.
///
/// @returns A status of 0 if all tests passed, or non-zero if there were any
/// failures. This is compatible with GoogleTest.
int RUN_ALL_TESTS();

/// @def GTEST_HAS_DEATH_TEST
/// Death tests are not supported. The `*_DEATH_IF_SUPPORTED` macros do nothing.
#define GTEST_HAS_DEATH_TEST 0

/// @def EXPECT_DEATH_IF_SUPPORTED
/// See `GTEST_HAS_DEATH_TEST`.
#define EXPECT_DEATH_IF_SUPPORTED(statement, regex) \
  if (0) {                                          \
    statement;                                      \
    static_cast<void>(regex);                       \
  }                                                 \
  static_assert(true, "Macros must be terminated with a semicolon")

/// @def ASSERT_DEATH_IF_SUPPORTED
/// See `GTEST_HAS_DEATH_TEST`.
#define ASSERT_DEATH_IF_SUPPORTED(statement, regex) \
  EXPECT_DEATH_IF_SUPPORTED(statement, regex)

/// @}

namespace pw {
namespace string {

/// @submodule{pw_unit_test,helpers}

// This function is used to print unknown types that are used in EXPECT or
// ASSERT statements in tests.
//
// You can add support for displaying custom types by defining a ToString
// template specialization. For example:
//
//   namespace pw {
//
//   template <>
//   StatusWithSize ToString<MyType>(const MyType& value,
//                                   span<char> buffer) {
//     return string::Format("<MyType|%d>", value.id);
//   }
//
//   }  // namespace pw
//
// See the documentation in pw_string/string_builder.h for more information.
template <typename T>
StatusWithSize UnknownTypeToString(const T& value, span<char> buffer) {
  StringBuilder sb(buffer);
  sb << '<' << sizeof(value) << "-byte object at 0x" << &value;

  // How many bytes of the object to print.
  //
  // WARNING: Printing the contents of an object may be undefined behavior!
  // Accessing uninitialized memory is undefined behavior, and objects
  // sometimes contain uninitialized regions, such as padding bytes or
  // unallocated storage (e.g. std::optional). kPrintMaybeUninitializedBytes
  // MUST stay at 0, except when changed locally to help with debugging.
  constexpr size_t kPrintMaybeUnintializedBytes = 0;

  constexpr size_t kBytesToPrint =
      std::min(sizeof(value), kPrintMaybeUnintializedBytes);

  if (kBytesToPrint != 0u) {
    sb << " |";

    // reinterpret_cast to std::byte is permitted by C++'s type aliasing
    // rules.
    const std::byte* bytes = reinterpret_cast<const std::byte*>(&value);

    for (size_t i = 0; i < kBytesToPrint; ++i) {
      sb << ' ' << bytes[i];
    }

    // If there's just one more byte, output it. Otherwise, output ellipsis.
    if (sizeof(value) == kBytesToPrint + 1) {
      sb << ' ' << bytes[sizeof(value) - 1];
    } else if (sizeof(value) > kBytesToPrint) {
      sb << " …";
    }
  }

  sb << '>';
  return sb.status_with_size();
}

/// @}

}  // namespace string

namespace unit_test {
namespace internal {

class Test;
class TestInfo;

// Types of SetUpTestSuite() and TearDownTestSuite() functions.
using SetUpTestSuiteFunc = void (*)();
using TearDownTestSuiteFunc = void (*)();

// Used to tag arguments to EXPECT_STREQ/EXPECT_STRNE so they are treated like
// C strings rather than pointers.
struct CStringArg {
  const char* const c_str;
};

constexpr size_t MaxPaddingNeededToRaiseAlignment(size_t current_align,
                                                  size_t new_align) {
  if (new_align < current_align) {
    return 0;
  }
  return new_align - current_align;
}

// GoogleTest supports stream-style messages, but pw_unit_test does not. This
// class accepts and ignores C++ <<-style logs.
class FailureMessageAdapter {
 public:
  constexpr FailureMessageAdapter() = default;

  template <typename T>
  constexpr const FailureMessageAdapter& operator<<(const T&) const {
    return *this;
  }
};

// Used to ignore a stream-style message in an assert, which returns. This
// uses a similar approach as upstream GoogleTest, but drops any messages.
class ReturnHelper {
 public:
  constexpr ReturnHelper() = default;

  // Return void so that assigning to ReturnHelper converts the log expression
  // to void without blocking the stream-style log with a closing parenthesis.
  // NOLINTNEXTLINE(misc-unconventional-assign-operator)
  constexpr void operator=(const FailureMessageAdapter&) const {}
};

// Singleton test framework class responsible for managing and running test
// cases. This implementation is internal to Pigweed test; free functions
// wrapping its functionality are exposed as the public interface.
class Framework {
 public:
  constexpr Framework()
      : current_test_(nullptr),
        current_result_(TestResult::kSuccess),
        run_tests_summary_{.passed_tests = 0,
                           .failed_tests = 0,
                           .skipped_tests = 0,
                           .disabled_tests = 0},
        exit_status_(0),
        event_handler_(nullptr),
        memory_pool_() {}

  static Framework& Get() { return framework_; }

  // Registers a single test case with the framework. The framework owns the
  // registered unit test. Called during static initialization.
  void RegisterTest(TestInfo* test) const;

  // Sets the handler to which the framework dispatches test events. During a
  // test run, the framework owns the event handler.
  inline void RegisterEventHandler(EventHandler* event_handler) {
    event_handler_ = event_handler;
  }

  // Runs all registered test cases, returning a status of 0 if all succeeded
  // or nonzero if there were any failures. Test events that occur during the
  // run are sent to the registered event handler, if any.
  int RunAllTests();

  // Only run test suites whose names are included in the provided list during
  // the next test run.
  void SetTestSuitesToRun(span<std::string_view> test_suites) {
    test_suites_to_run_ = test_suites;
  }

  bool ShouldRunTest(const TestInfo& test_info) const;

  // Whether the current test is skipped.
  bool IsSkipped() const { return current_result_ == TestResult::kSkipped; }

  // Whether the current test has failed.
  bool HasFailure() const { return current_result_ == TestResult::kFailure; }

  // Constructs an instance of a unit test class and runs the test.
  //
  // Tests are constructed within a static memory pool at run time instead of
  // being statically allocated to avoid blowing up the size of the test
  // binary in cases where users have large test fixtures (e.g. containing
  // buffers) reused many times. Instead, only a small, fixed-size TestInfo
  // struct is statically allocated per test case, with a run() function that
  // references this method instantiated for its test class.
  template <typename TestInstance>
  static void CreateAndRunTest(const TestInfo& test_info) {
    static_assert(
        sizeof(TestInstance) +
                MaxPaddingNeededToRaiseAlignment(
                    alignof(decltype(memory_pool_)), alignof(TestInstance)) <=
            sizeof(memory_pool_),
        "The test memory pool is too small for this test. Either increase "
        "PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE or decrease the size of your "
        "test fixture.");

    Framework& framework = Get();
    framework.StartTest(test_info);

    // Reset the memory pool to a marker value to help detect use of
    // uninitialized memory.
    std::memset(&framework.memory_pool_, 0xa5, sizeof(framework.memory_pool_));

    framework.SetUpTestSuiteIfNeeded(TestInstance::SetUpTestSuite);

    // Construct the test object within the static memory pool. The StartTest
    // function has already been called by the TestInfo at this point.
    void* aligned_pool =
        AlignUp(&framework.memory_pool_, alignof(TestInstance));
    TestInstance* test_instance = new (aligned_pool) TestInstance();
    test_instance->PigweedTestRun();

    // Manually call the destructor as it is not called automatically for
    // objects constructed using placement new.
    test_instance->~TestInstance();

    framework.TearDownTestSuiteIfNeeded(TestInstance::TearDownTestSuite);

    framework.EndCurrentTest();
  }

  template <typename Expectation, typename Lhs, typename Rhs, typename Epsilon>
  [[nodiscard]] bool CurrentTestExpectWithEpsilon(Expectation expectation,
                                                  const Lhs& lhs,
                                                  const Rhs& rhs,
                                                  const Epsilon& epsilon,
                                                  const char* expression,
                                                  const char* file,
                                                  int line) {
    const bool success = expectation(lhs, rhs, epsilon);
    if (!success) {
      CurrentTestExpectSimple(expression,
                              MakeString<config::kExpectationBufferSizeBytes>(
                                  ConvertForPrint(lhs),
                                  " within ",
                                  ConvertForPrint(epsilon),
                                  " of ",
                                  ConvertForPrint(rhs))
                                  .c_str(),
                              file,
                              line,
                              success);
    }
    return success;
  }

  // Runs an expectation function for the currently active test case.
  template <typename Expectation, typename Lhs, typename Rhs>
  [[nodiscard]] bool CurrentTestExpect(Expectation expectation,
                                       const Lhs& lhs,
                                       const Rhs& rhs,
                                       const char* expectation_string,
                                       const char* expression,
                                       const char* file,
                                       int line) {
    const bool success = expectation(lhs, rhs);
    if (!success) {
      CurrentTestExpectSimple(
          expression,
          MakeString<config::kExpectationBufferSizeBytes>(ConvertForPrint(lhs),
                                                          ' ',
                                                          expectation_string,
                                                          ' ',
                                                          ConvertForPrint(rhs))
              .c_str(),
          file,
          line,
          success);
    }
    return success;
  }

  // Skips the current test and dispatches an event for it.
  ::pw::unit_test::internal::FailureMessageAdapter CurrentTestSkip(
      const char* file, int line);

  // Dispatches an event indicating the result of an expectation.
  ::pw::unit_test::internal::FailureMessageAdapter CurrentTestExpectSimple(
      const char* expression,
      const char* evaluated_expression,
      const char* file,
      int line,
      bool success);

 private:
  // Convert char* to void* so that they are printed as pointers instead of
  // strings in EXPECT_EQ and other macros. EXPECT_STREQ wraps its pointers in
  // a CStringArg so its pointers are treated like C strings.
  static constexpr const void* ConvertForPrint(const char* str) { return str; }

  static constexpr const void* ConvertForPrint(char* str) { return str; }

  static constexpr const char* ConvertForPrint(CStringArg value) {
    return value.c_str;
  }

  template <typename T>
  static constexpr T ConvertForPrint(T&& value) {
    return std::forward<T>(value);
  }

  // If current_test_ will be first of its suite, call set_up_ts
  void SetUpTestSuiteIfNeeded(SetUpTestSuiteFunc set_up_ts) const;

  // If current_test_ was the last of its suite, call tear_down_ts
  void TearDownTestSuiteIfNeeded(TearDownTestSuiteFunc tear_down_ts) const;

  // Sets current_test_ and dispatches an event indicating that a test
  // started.
  void StartTest(const TestInfo& test);

  // Dispatches event indicating that a test finished and clears
  // current_test_.
  void EndCurrentTest();

  // Singleton instance of the framework class.
  static Framework framework_;

  // Linked list of all registered test cases. This is static as it tests are
  // registered using static initialization.
  static TestInfo* tests_;

  // The current test case which is running.
  const TestInfo* current_test_;

  // Overall result of the current test case (pass/fail/skip).
  TestResult current_result_;

  // Overall result of the ongoing test run, which covers multiple tests.
  RunTestsSummary run_tests_summary_;

  // Program exit status returned by RunAllTests for GoogleTest compatibility.
  int exit_status_;

  // Handler to which to dispatch test events.
  EventHandler* event_handler_;

  span<std::string_view> test_suites_to_run_;

  alignas(std::max_align_t) std::byte memory_pool_[config::kMemoryPoolSize];
};

// Information about a single test case, including a pointer to a function
// which constructs and runs the test class. These are statically allocated
// instead of the test classes, as test classes can be very large.
class TestInfo {
 public:
  TestInfo(const char* const test_suite_name,
           const char* const test_name,
           const char* const file_name,
           void (*run_func)(const TestInfo&))
      : test_case_{
            .suite_name = test_suite_name,
            .test_name = test_name,
            .file_name = file_name,
        },
        run_(run_func) {
    Framework::Get().RegisterTest(this);
  }

  // The name of the suite to which the test case belongs, the name of the
  // test case itself, and the path to the file in which the test case is
  // located.
  const TestCase& test_case() const { return test_case_; }

  bool enabled() const;

  void run() const { run_(*this); }

  TestInfo* next() const { return next_; }
  void set_next(TestInfo* next) { next_ = next; }

 private:
  TestCase test_case_;

  // Function which runs the test case. Refers to Framework::CreateAndRunTest
  // instantiated for the test case's class.
  void (*run_)(const TestInfo&);

  // TestInfo structs are registered with the test framework and stored as a
  // linked list.
  TestInfo* next_ = nullptr;
};

// Base class for all test cases or custom test fixtures.
// Every unit test created using the TEST or TEST_F macro defines a class that
// inherits from this (or a subclass of this).
//
// For example, given the following test definition:
//
//   TEST(MyTest, SaysHello) {
//     ASSERT_STREQ(SayHello(), "Hello, world!");
//   }
//
// A new class is defined for the test, e.g. MyTest_SaysHello_Test. This class
// inherits from the Test class and implements its PigweedTestBody function
// with the block provided to the TEST macro.
class Test {
 public:
  Test(const Test&) = delete;
  Test& operator=(const Test&) = delete;

  virtual ~Test() = default;

  static void SetUpTestSuite() {}
  static void TearDownTestSuite() {}

  static bool HasFailure() { return Framework::Get().HasFailure(); }

  // Runs the unit test.
  void PigweedTestRun() {
    SetUp();
    if (!HasFailure() && !Framework::Get().IsSkipped()) {
      PigweedTestBody();
    }
    TearDown();
  }

 protected:
  Test() = default;

  // Called immediately before executing the test body.
  //
  // Setup and cleanup can typically be done in the test fixture's constructor
  // and destructor, but there are cases where SetUp/TearDown must be used
  // instead. See the Google Test documentation for more information.
  virtual void SetUp() {}

  // Called immediately after executing the test body.
  virtual void TearDown() {}

 private:
  friend class internal::Framework;

  // The user-provided body of the test case. Populated by the TEST macro.
  virtual void PigweedTestBody() = 0;
};

// Checks that a test suite name is valid.
constexpr bool HasNoUnderscores(const char* suite) {
  const char* disabled_prefix = "DISABLED_";

  for (; *suite != '\0'; ++suite) {
    if (*suite == *disabled_prefix) {
      disabled_prefix += 1;
    } else {
      disabled_prefix = "";
      if (*suite == '_') {
        return false;
      }
    }
  }
  return true;
}

}  // namespace internal

inline void SetTestSuitesToRun(span<std::string_view> test_suites) {
  internal::Framework::Get().SetTestSuitesToRun(test_suites);
}

}  // namespace unit_test
}  // namespace pw

inline int RUN_ALL_TESTS() {
  return ::pw::unit_test::internal::Framework::Get().RunAllTests();
}

#define _PW_TEST(test_suite_name, test_name, parent_class)                     \
  static_assert(sizeof(#test_suite_name) > 1,                                  \
                "The test suite name must not be empty");                      \
  static_assert(::pw::unit_test::internal::HasNoUnderscores(#test_suite_name), \
                "The test suite name (" #test_suite_name                       \
                ") cannot contain underscores");                               \
  static_assert(sizeof(#test_name) > 1, "The test name must not be empty");    \
                                                                               \
  _PW_TEST_CLASS(test_suite_name,                                              \
                 test_name,                                                    \
                 test_suite_name##_##test_name##_Test,                         \
                 parent_class)

#define _PW_TEST_CLASS(suite, name, class_name, parent_class)               \
  class class_name final : public parent_class {                            \
   private:                                                                 \
    void PigweedTestBody() override;                                        \
  };                                                                        \
                                                                            \
  extern "C" {                                                              \
                                                                            \
  /* Silence ASAN to avoid errors in the initialization order checker */    \
  /* caused by the intentional use of dynamic initializers which modify */  \
  /* other globals */                                                       \
  PW_NO_SANITIZE("address")                                                 \
  /* Declare the TestInfo as non-const since const variables do not work */ \
  /* with the PW_UNIT_TEST_LINK_FILE_CONTAINING_TEST macro. */              \
  /* NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) */  \
  ::pw::unit_test::internal::TestInfo _pw_unit_test_Info_##suite##_##name(  \
      #suite,                                                               \
      #name,                                                                \
      __FILE__,                                                             \
      ::pw::unit_test::internal::Framework::CreateAndRunTest<class_name>);  \
                                                                            \
  } /* extern "C" */                                                        \
                                                                            \
  void class_name::PigweedTestBody()

#define _PW_TEST_ASSERT(expectation)                 \
  if (!(expectation))                                \
  return ::pw::unit_test::internal::ReturnHelper() = \
             ::pw::unit_test::internal::FailureMessageAdapter()

#define _PW_TEST_EXPECT(expectation) \
  if (!(expectation))                \
  ::pw::unit_test::internal::FailureMessageAdapter()

#define _PW_TEST_BOOL(expr, value)                                   \
  ::pw::unit_test::internal::Framework::Get().CurrentTestExpect(     \
      [](bool _pw_lhs, bool _pw_rhs) { return _pw_lhs == _pw_rhs; }, \
      static_cast<bool>(expr),                                       \
      value,                                                         \
      "is",                                                          \
      #expr " is " #value,                                           \
      __FILE__,                                                      \
      __LINE__)

#define _PW_TEST_OP(lhs, rhs, op)                                \
  ::pw::unit_test::internal::Framework::Get().CurrentTestExpect( \
      [](const auto& _pw_lhs, const auto& _pw_rhs) {             \
        return _pw_lhs op _pw_rhs;                               \
      },                                                         \
      (lhs),                                                     \
      (rhs),                                                     \
      #op,                                                       \
      #lhs " " #op " " #rhs,                                     \
      __FILE__,                                                  \
      __LINE__)

#define _PW_TEST_NEAR(lhs, rhs, epsilon)                                      \
  ::pw::unit_test::internal::Framework::Get().CurrentTestExpectWithEpsilon(   \
      [](const auto& _pw_lhs, const auto& _pw_rhs, const auto& _pw_epsilon) { \
        return std::abs(_pw_lhs - _pw_rhs) <= _pw_epsilon;                    \
      },                                                                      \
      (lhs),                                                                  \
      (rhs),                                                                  \
      (epsilon),                                                              \
      #lhs " within " #epsilon " of " #rhs,                                   \
      __FILE__,                                                               \
      __LINE__)

#define _PW_TEST_C_STR(lhs, rhs, op)                             \
  ::pw::unit_test::internal::Framework::Get().CurrentTestExpect( \
      [](const auto& _pw_lhs, const auto& _pw_rhs) {             \
        auto cmp = [](const char* l, const char* r) -> int {     \
          if (!l || !r) {                                        \
            return l != r;                                       \
          }                                                      \
          return std::strcmp(l, r);                              \
        };                                                       \
        return cmp(_pw_lhs.c_str, _pw_rhs.c_str) op 0;           \
      },                                                         \
      ::pw::unit_test::internal::CStringArg{lhs},                \
      ::pw::unit_test::internal::CStringArg{rhs},                \
      #op,                                                       \
      #lhs " " #op " " #rhs,                                     \
      __FILE__,                                                  \
      __LINE__)

// Checks that test suite names between TEST and TEST_F declarations are unique.
// This works by declaring a function named for the test suite. The function
// takes no arguments but is declared with different return types in the TEST
// and TEST_F macros. If a TEST and TEST_F use the same test suite name, the
// function declarations conflict, resulting in a compilation error.
//
// This catches most conflicts, but a runtime check is ultimately needed since
// tests may be declared in different translation units.
#if !defined(__clang__) && !defined(__GNUC___) && __GNUC__ <= 8
// For some reason GCC8 is unable to ignore -Wredundant-decls here.
#define _PW_TEST_SUITE_NAMES_MUST_BE_UNIQUE(return_type, test_suite)
#else  // All other compilers.
#define _PW_TEST_SUITE_NAMES_MUST_BE_UNIQUE(return_type, test_suite)       \
  PW_MODIFY_DIAGNOSTICS_PUSH();                                            \
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wredundant-decls");                      \
  extern "C" return_type /* use extern "C" to escape namespacing */        \
  PwUnitTestSuiteNamesMustBeUniqueBetweenTESTandTEST_F_##test_suite(void); \
  PW_MODIFY_DIAGNOSTICS_POP()
#endif  // GCC8 or older.

namespace testing {

// Alias Test as ::testing::Test for GoogleTest compatibility.
using Test = ::pw::unit_test::internal::Test;

// Provide a no-op init routine for GoogleTest compatibility.
inline void InitGoogleTest(int*, char**) {}

}  // namespace testing
