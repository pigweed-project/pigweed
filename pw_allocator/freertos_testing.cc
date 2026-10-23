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

#include <cstddef>

#include "FreeRTOS.h"
#include "pw_allocator/testing.h"

// This file contains a mock implementations of the portion of the FreeRTOS API
// needed to test FreeRtosAllocator.

namespace {

pw::allocator::test::AllocatorForTest<1024> g_allocator;

}  // namespace

void* pvPortMalloc(size_t xWantedSize) {
  return g_allocator.Allocate({xWantedSize, portBYTE_ALIGNMENT});
}

void vPortFree(void* pv) { g_allocator.Deallocate(pv); }
