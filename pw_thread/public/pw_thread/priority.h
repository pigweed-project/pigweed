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
#pragma once

#include "pw_thread/internal/priority.h"  // IWYU pragma: export
#include "pw_thread_backend/priority.h"   // IWYU pragma: export

namespace pw {

/// @module{pw_thread}

/// Represents a thread's priority. Schedulers favor higher priority tasks when
/// determining which thread to run. Scheduling policies differ, but a common
/// default policy for real-time operating systems is preemptive scheduling,
/// where the highest priority ready thread is always selected to run. If there
/// are multiple tasks with the same priority, RTOSes may use round-robin
/// scheduling, where each ready thread at a given priority is given equal
/// opportunity to run.
///
/// Priorities may be compared with `==`, `!=`, `<`, etc. Comparisons occur in
/// terms of the native priorities they represent, but are translated such that
/// logically lower priorities always compare as less than higher priorities,
/// even if the underlying RTOS uses lower numbers for higher priorities.
using ThreadPriority =
    thread::internal::Priority<thread::backend::PriorityType,
                               thread::backend::kLowestPriority,
                               thread::backend::kHighestPriority,
                               thread::backend::kDefaultPriority>;

}  // namespace pw
