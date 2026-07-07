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
#pragma once

#include <lib/stdcompat/memory.h>

#include "pw_containers/internal/pod_deque_internal.h"

namespace pw::containers {

// Simpler stand-in for pw::Deque, pw::InlineDeque, pw::DynamicDeque
template <typename ValueType, typename SizeType = size_t>
using PodDeque = internal::PodDeque<ValueType, SizeType>;

}  // namespace pw::containers
