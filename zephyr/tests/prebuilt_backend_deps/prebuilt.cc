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

#include <chrono>

#include "pw_chrono/system_clock.h"
#include "pw_chrono/system_timer.h"
#include "pw_thread/sleep.h"

// Uses the backends' out-of-line functions, which come from the libraries
// app.prebuilt declares in CMakeLists.txt.
void PrebuiltRun() {
  pw::chrono::SystemTimer timer([](pw::chrono::SystemClock::time_point) {});
  timer.InvokeAfter(
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(1)));
  pw::this_thread::sleep_for(
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(2)));
  timer.Cancel();
}
