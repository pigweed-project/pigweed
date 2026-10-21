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

/// Interrupt context interface
namespace pw::interrupt {

/// @module{pw_interrupt}

/// @brief Checks if the currently executing code is within an interrupt service
/// routine handling an interrupt request (IRQ) or non-maskable interrupt (NMI).
///
/// @returns `true` if the currently executing code is in an interrupt
/// context. `false` if not.
bool InInterruptContext();

}  // namespace pw::interrupt

// The backend can opt to include an inlined implementation of the following:
//   bool InInterruptContext();
#if __has_include("pw_interrupt_backend/context_inline.h")
#include "pw_interrupt_backend/context_inline.h"  // IWYU pragma: export
#endif  // __has_include("pw_interrupt_backend/context_inline.h")
