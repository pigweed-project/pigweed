// Copyright 2023 The Pigweed Authors
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

#include <cstdint>
#include <limits>

#include "pw_assert/assert.h"
#include "pw_perf_test/event_handler.h"
#include "pw_perf_test/internal/timer.h"

namespace pw::perf_test {

// Forward declaration.
class State;

namespace internal {

// Allows access to the private State object constructor
State CreateState(int durations,
                  EventHandler& event_handler,
                  const char* test_name);

}  // namespace internal

/// Records the performance of a test case over many iterations.
class State {
 public:
  // KeepRunning() should be called in a while loop. Responsible for managing
  // iterations and timestamps.
  bool KeepRunning() {
    internal::Timestamp iteration_end = internal::GetCurrentTimestamp();
    const bool keep_running = KeepRunningInternal(iteration_end);
    iteration_start_ = internal::GetCurrentTimestamp();
    return keep_running;
  }

 private:
  // Allows the framework to create state objects and unit tests for the state
  // class
  friend State internal::CreateState(int durations,
                                     EventHandler& event_handler,
                                     const char* test_name);

  bool KeepRunningInternal(internal::Timestamp iteration_end);

  // Privated constructor to prevent unauthorized instances of the state class.
  constexpr State(int iterations,
                  EventHandler& event_handler,
                  const char* test_name)
      : test_iterations_(iterations),
        iteration_start_(),
        event_handler_(&event_handler),
        test_info{.name = test_name} {
    PW_ASSERT(test_iterations_ > 0);
  }

  int64_t mean_ = -1;

  // Stores the total number of iterations wanted
  const int test_iterations_;

  // Stores the total duration of the tests.
  int64_t total_duration_ = 0;

  // Smallest value of the iterations
  int64_t min_ = std::numeric_limits<int64_t>::max();

  // Largest value of the iterations
  int64_t max_ = std::numeric_limits<int64_t>::min();

  // Time at the start of the iteration
  internal::Timestamp iteration_start_;

  // Run the loop this many times before recording data.
  static constexpr int kWarmUpIterations = 1;

  // The current iteration.
  int current_iteration_ = -(kWarmUpIterations + 1);

  EventHandler* event_handler_;

  TestCase test_info;
};

}  // namespace pw::perf_test
