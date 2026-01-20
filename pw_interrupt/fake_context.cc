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

#include "pw_interrupt/fake_context.h"

#include <utility>

#include "pw_function/function.h"

namespace pw::interrupt {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
Function<bool()> in_interrupt_context_callback;

}  // namespace

bool InInterruptContext() {
  return in_interrupt_context_callback != nullptr
             ? in_interrupt_context_callback()
             : false;
}

namespace fake {

void SetInInterruptContextCallback(Function<bool()>&& callback) {
  in_interrupt_context_callback = std::move(callback);
}

}  // namespace fake

}  // namespace pw::interrupt
