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

extern "C" void pw_unit_test_RunRustTests(void);

namespace pw::unit_test::internal {
namespace {

struct RustTestRunnerRegistrar {
  RustTestRunnerRegistrar() {
    pw_unit_test_RegisterFfiTestRunner(pw_unit_test_RunRustTests);
  }
};

RustTestRunnerRegistrar g_rust_test_runner_registrar;

}  // namespace
}  // namespace pw::unit_test::internal
