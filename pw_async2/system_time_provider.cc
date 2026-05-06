// Copyright 2024 The Pigweed Authors
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

#include "pw_async2/system_time_provider.h"

#include "pw_chrono/system_timer.h"
#include "pw_memory/no_destructor.h"

namespace pw::async2 {
namespace {

using ::pw::chrono::SystemClock;

class SystemTimeProvider final : public TimeProvider<SystemClock> {
 public:
  SystemTimeProvider()
      : timer_(
            [this](SystemClock::time_point expired) { RunExpired(expired); }) {}

 private:
  SystemClock::time_point now() final { return SystemClock::now(); }

  void DoInvokeAt(SystemClock::time_point time_point) final {
    timer_.InvokeAt(time_point);
  }

  void DoCancel() final { timer_.Cancel(); }

  pw::chrono::SystemTimer timer_;
};

}  // namespace

TimeProvider<SystemClock>& GetSystemTimeProvider() {
  static pw::NoDestructor<SystemTimeProvider> time_provider;
  return *time_provider;
}

}  // namespace pw::async2
