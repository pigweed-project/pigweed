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

#include "context_usage.h"

#include "pw_interrupt/context.h"

// This is a trivial library that uses //pw_interrupt:context, and that
// can be tested by a unit test using the /pw_interrupt:fake_context backend.

namespace pw::interrupt::examples {

bool ExampleInInterruptContext() { return pw::interrupt::InInterruptContext(); }

}  // namespace pw::interrupt::examples
