# Copyright 2024 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include("$ENV{PW_ROOT}/pw_build/pigweed.cmake")
include("$ENV{PW_ROOT}/pw_perf_test/backend.cmake")
include("$ENV{PW_ROOT}/pw_sync/backend.cmake")
include("$ENV{PW_ROOT}/pw_thread/backend.cmake")

# Libraries

pw_add_library(pw_allocator.examples.named_u32 INTERFACE
  HEADERS
    public/examples/named_u32.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_string
)

pw_add_library(pw_allocator.examples.custom_allocator STATIC
  HEADERS
    public/examples/custom_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
  SOURCES
    custom_allocator.cc
  PRIVATE_DEPS
    pw_log
    pw_result
)

pw_add_library(pw_allocator.examples.custom_allocator_test_harness INTERFACE
  HEADERS
    public/examples/custom_allocator_test_harness.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.examples.custom_allocator
    pw_allocator.test_harness
    pw_allocator.testing
)

    # Examples

pw_add_test(pw_allocator.examples.basic
  PRIVATE_DEPS
    pw_allocator
    pw_allocator.examples.named_u32
    pw_allocator.testing
  SOURCES
    basic.cc
)

pw_add_test(pw_allocator.examples.block_allocator
  PRIVATE_DEPS
    pw_allocator.examples.named_u32
    pw_allocator.first_fit
  SOURCES
    block_allocator.cc
)

if((NOT "${pw_perf_test.timer_interface_BACKEND}" STREQUAL ""))
  pw_add_test(pw_allocator.examples.custom_allocator_perf_test
    PRIVATE_DEPS
      pw_allocator.examples.custom_allocator_test_harness
      pw_perf_test
      pw_perf_test.state
      pw_random
    SOURCES
      custom_allocator_perf_test.cc
  )
endif()

pw_add_test(pw_allocator.examples.custom_allocator_test
  PRIVATE_DEPS
    pw_allocator.examples.custom_allocator
    pw_allocator.examples.custom_allocator_test_harness
    pw_allocator.examples.named_u32
    pw_allocator.fuzzing
    pw_allocator.testing
    pw_containers.vector
    pw_fuzzer.fuzztest
  SOURCES
    custom_allocator_test.cc
)

pw_add_test(pw_allocator.examples.linker_sections
  PRIVATE_DEPS
    pw_allocator
    pw_allocator.examples.named_u32
    pw_allocator.block_allocator
    pw_allocator.block.detailed_block
  SOURCES
    linker_sections.cc
)

pw_add_test(pw_allocator.examples.metrics
  PRIVATE_DEPS
    pw_allocator.examples.named_u32
    pw_allocator.metrics
    pw_allocator.testing
    pw_allocator.tracking_allocator
    pw_metric
  SOURCES
    metrics.cc
)

pw_add_test(pw_allocator.examples.pmr
  PRIVATE_DEPS
    pw_allocator
    pw_allocator.pmr_allocator
    pw_allocator.testing
    pw_tokenizer
  SOURCES
    pmr.cc
)

if((NOT "${pw_sync.interrupt_spin_lock_BACKEND}" STREQUAL "") AND
   (NOT "${pw_thread.test_thread_context_BACKEND}" STREQUAL ""))
  pw_add_test(pw_allocator.examples.spin_lock
    PRIVATE_DEPS
      pw_allocator.examples.named_u32
      pw_allocator.synchronized_allocator
      pw_allocator.testing
      pw_sync.interrupt_spin_lock
      pw_thread.test_thread_context
      pw_thread.thread
      pw_thread.thread_core
      pw_assert
    SOURCES
      spin_lock.cc
  )
endif()
