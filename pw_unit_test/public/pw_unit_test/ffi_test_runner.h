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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
namespace pw::unit_test {
class EventHandler;
}  // namespace pw::unit_test

typedef pw::unit_test::EventHandler pw_unit_test_EventHandler;

extern "C" {
#else
typedef struct pw_unit_test_EventHandler pw_unit_test_EventHandler;
#endif

/// Test runner execution context passed into FFI test runners.
typedef struct pw_unit_test_FfiTestContext {
  pw_unit_test_EventHandler* event_handler;
  const char* suite;
  const char* name;
  const char* file;
} pw_unit_test_FfiTestContext;

/// Function pointer type for an on-device test function.
typedef void (*pw_unit_test_TestFn)(void);

/// Descriptor for a registered test case.
typedef struct pw_unit_test_TestDescriptor {
  const char* name;
  const char* suite;
  pw_unit_test_TestFn test_fn;
} pw_unit_test_TestDescriptor;

/// Notifies the C++ test framework that a test case is starting.
void pw_unit_test_StartTest(pw_unit_test_FfiTestContext* handle,
                            const char* suite,
                            const char* name,
                            const char* file);

/// Reports a test expectation failure (assert) to the C++ test framework.
void pw_unit_test_Expect(pw_unit_test_FfiTestContext* handle,
                         const char* expression,
                         const char* evaluated_expression,
                         const char* file,
                         uint32_t line);

/// Notifies the C++ test framework that a test case has ended.
void pw_unit_test_EndTest(pw_unit_test_FfiTestContext* handle);

/// Function pointer type for an FFI test runner.
typedef void (*pw_unit_test_FfiTestRunner)(void);

/// Checks whether a test suite should be executed according to active filters.
bool pw_unit_test_ShouldRunSuite(const char* suite_name);

/// Checks whether an FFI test runner callback has been registered.
bool pw_unit_test_HasFfiTestRunner(void);

/// Registers a test runner callback to be invoked by pw_unit_test_RunFfiTests.
void pw_unit_test_RegisterFfiTestRunner(pw_unit_test_FfiTestRunner runner);

/// Runs all registered FFI unit tests. Asserts that a runner has been
/// registered.
void pw_unit_test_RunFfiTests(void);

#ifdef __cplusplus
}
#endif
