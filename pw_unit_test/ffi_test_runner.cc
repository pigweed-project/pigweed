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

#include "pw_unit_test/ffi_test_runner.h"

#include "light_public_overrides/pw_unit_test/framework_backend.h"
#include "pw_assert/check.h"
#include "pw_unit_test/event_handler.h"

extern "C" {

static pw_unit_test_FfiTestRunner s_ffi_test_runner = nullptr;

bool pw_unit_test_HasFfiTestRunner(void) {
  return s_ffi_test_runner != nullptr;
}

void pw_unit_test_RegisterFfiTestRunner(pw_unit_test_FfiTestRunner runner) {
  s_ffi_test_runner = runner;
}

void pw_unit_test_RunFfiTests(void) {
  PW_CHECK_NOTNULL(
      s_ffi_test_runner,
      "pw_unit_test_RunFfiTests called without a registered runner");
  s_ffi_test_runner();
}

bool pw_unit_test_ShouldRunSuite(const char* suite_name) {
  if (suite_name == nullptr) {
    return false;
  }
  return pw::unit_test::internal::Framework::Get().ShouldRunSuite(suite_name);
}

static pw::unit_test::TestCase s_current_ffi_test_case;

void pw_unit_test_StartTest(pw_unit_test_FfiTestContext* handle,
                            const char* suite,
                            const char* name,
                            const char* file) {
  PW_CHECK_NOTNULL(handle);
  PW_CHECK_NOTNULL(suite);
  PW_CHECK_NOTNULL(name);

  handle->suite = suite;
  handle->name = name;
  handle->file = file != nullptr ? file : "ffi_source";

  s_current_ffi_test_case = {handle->suite, handle->name, handle->file};
  pw::unit_test::internal::Framework::Get().StartTest(s_current_ffi_test_case);
}

void pw_unit_test_Expect(pw_unit_test_FfiTestContext* handle,
                         const char* expression,
                         const char* evaluated_expression,
                         const char* file,
                         uint32_t line) {
  PW_CHECK_NOTNULL(handle);
  PW_CHECK_NOTNULL(expression);
  PW_CHECK_NOTNULL(evaluated_expression);
  PW_CHECK_NOTNULL(file);

  pw::unit_test::internal::Framework::Get().CurrentTestExpectSimple(
      expression, evaluated_expression, file, static_cast<int>(line), false);
}

void pw_unit_test_EndTest(pw_unit_test_FfiTestContext* handle) {
  PW_CHECK_NOTNULL(handle);
  pw::unit_test::internal::Framework::Get().EndCurrentTest();
}

}  // extern "C"
