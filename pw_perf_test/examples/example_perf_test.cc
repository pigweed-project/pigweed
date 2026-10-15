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

#include <cstddef>

#include "pw_perf_test/perf_test.h"

namespace pw::perf_test {
namespace {

// DOCSTAG: [pw_perf_test_examples-simulate_work]
void SimulateWork(size_t a, size_t b) {
  volatile size_t i = 0;
  while (i < a * b * 100000) {
    i = i + 1;
  }
}
// DOCSTAG: [pw_perf_test_examples-simulate_work]

// DOCSTAG: [pw_perf_test_examples-simple_example]
PW_PERF_TEST_SIMPLE(SimpleFunction,
                    SimulateWork,
                    static_cast<size_t>(2),
                    static_cast<size_t>(4));
// DOCSTAG: [pw_perf_test_examples-simple_example]

// DOCSTAG: [pw_perf_test_examples-full_example]
void TestFunction(pw::perf_test::State& state, size_t a, size_t b) {
  while (state.KeepRunning()) {
    SimulateWork(a, b);
  }
}
PW_PERF_TEST(FunctionWithArgs, TestFunction, 2, 4);
// DOCSTAG: [pw_perf_test_examples-full_example]

// DOCSTAG: [pw_perf_test_examples-lambda_example]
PW_PERF_TEST_SIMPLE(
    SimpleLambda,
    [](size_t a, size_t b) { SimulateWork(a, b); },
    static_cast<size_t>(2),
    static_cast<size_t>(4));

PW_PERF_TEST(
    LambdaFunction,
    [](pw::perf_test::State& state, size_t a, size_t b) {
      while (state.KeepRunning()) {
        SimulateWork(a, b);
      }
    },
    2,
    4);
// DOCSTAG: [pw_perf_test_examples-lambda_example]

}  // namespace
}  // namespace pw::perf_test
