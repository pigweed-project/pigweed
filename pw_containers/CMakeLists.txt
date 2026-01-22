# Copyright 2020 The Pigweed Authors
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

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

# Module configuration

pw_add_module_config(pw_containers_CONFIG)

pw_add_library(pw_containers.config INTERFACE
  HEADERS
    public/pw_containers/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_containers_CONFIG}
)

# Libraries

# The pw_containers group is DEPRECATED. Do not add new containers to it.
pw_add_library(pw_containers INTERFACE
  PUBLIC_DEPS
    pw_containers.algorithm
    pw_containers.flat_map
    pw_containers.inline_deque
    pw_containers.inline_queue
    pw_containers.intrusive_list
    pw_containers.vector
)

pw_add_library(pw_containers.algorithm INTERFACE
  HEADERS
    public/pw_containers/algorithm.h
    public/pw_containers/internal/algorithm_internal.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers.bitset INTERFACE
  HEADERS
    public/pw_containers/bitset.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_third_party.fuchsia.stdcompat
)

pw_add_library(pw_containers.filtered_view INTERFACE
  HEADERS
    public/pw_containers/filtered_view.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_preprocessor
)

pw_add_library(pw_containers.flat_map INTERFACE
  HEADERS
    public/pw_containers/flat_map.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert.assert
)

pw_add_library(pw_containers.functional INTERFACE
  HEADERS
    public/pw_containers/functional.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers._common INTERFACE
  HEADERS
    public/pw_containers/internal/traits.h
    public/pw_containers/internal/wrap.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers._deque_common INTERFACE
  HEADERS
    public/pw_containers/internal/deque_iterator.h
    public/pw_containers/internal/generic_deque.h
    public/pw_containers/internal/count_and_capacity.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_containers._common
    pw_span
    pw_third_party.fuchsia.stdcompat
)

pw_add_library(pw_containers.async_count_and_capacity INTERFACE
  HEADERS
    public/pw_containers/internal/async_count_and_capacity.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_async2
    pw_async2.poll
    pw_containers._deque_common
)

pw_add_library(pw_containers.dynamic_deque INTERFACE
  HEADERS
    public/pw_containers/dynamic_deque.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers._deque_common
    pw_allocator.allocator
    pw_numeric.saturating_arithmetic
)

pw_add_library(pw_containers.inline_deque INTERFACE
  HEADERS
    public/pw_containers/inline_deque.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_containers._deque_common
    pw_containers._raw_storage
    pw_toolchain.constexpr_tag
)

pw_add_library(pw_containers.inline_async_deque INTERFACE
  HEADERS
    public/pw_containers/inline_async_deque.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.async_count_and_capacity
    pw_containers.inline_deque
    pw_preprocessor
)

pw_add_library(pw_containers._queue_common INTERFACE
  HEADERS
    public/pw_containers/internal/generic_queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers._deque_common
)

pw_add_library(pw_containers.dynamic_queue INTERFACE
  HEADERS
    public/pw_containers/dynamic_queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.dynamic_queue
    pw_containers._queue_common
)

pw_add_library(pw_containers.inline_queue INTERFACE
  HEADERS
    public/pw_containers/inline_queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.inline_deque
    pw_containers._queue_common
)

pw_add_library(pw_containers.inline_async_queue INTERFACE
  HEADERS
    public/pw_containers/inline_async_queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.async_count_and_capacity
    pw_containers.inline_async_deque
    pw_containers.inline_queue
    pw_containers._queue_common
)

pw_add_library(pw_containers.iterator INTERFACE
  HEADERS
    public/pw_containers/iterator.h
    public/pw_containers/ptr_iterator.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_containers.ptr_iterator_test
  SOURCES
    ptr_iterator_test.cc
  PRIVATE_DEPS
    pw_containers.iterator
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.bitset_test
  SOURCES
    bitset_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.bitset
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_library(pw_containers._raw_storage INTERFACE
  HEADERS
    public/pw_containers/internal/raw_storage.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers.storage INTERFACE
  HEADERS
    public/pw_containers/storage.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.algorithm
)

pw_add_library(pw_containers.deque INTERFACE
  HEADERS
    public/pw_containers/deque.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_allocator.allocator
    pw_containers._deque_common
    pw_containers.storage
)

pw_add_library(pw_containers.queue INTERFACE
  HEADERS
    public/pw_containers/queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.deque
    pw_containers._queue_common
)

pw_add_library(pw_containers._test_helpers STATIC
  HEADERS
    public/pw_containers/internal/container_tests.h
    public/pw_containers/internal/test_helpers.h
  SOURCES
    test_helpers.cc
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.algorithm
    pw_unit_test
  PRIVATE_DEPS
    pw_assert
)

pw_add_library(pw_containers.to_array INTERFACE
  HEADERS
    public/pw_containers/to_array.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers.generic_var_len_entry_queue STATIC
  HEADERS
    public/pw_containers/internal/generic_var_len_entry_queue.h
    public/pw_containers/internal/var_len_entry.h
    public/pw_containers/internal/var_len_entry_iterator.h
    public/pw_containers/internal/var_len_entry_queue_iterator.h
  PUBLIC_INCLUDES
    public
  SOURCES
    var_len_entry.cc
    var_len_entry_queue_iterator.cc
  PUBLIC_DEPS
    pw_assert.assert
    pw_bytes
    pw_containers._common
    pw_containers.algorithm
    pw_preprocessor
    pw_span
    pw_span.cast
    pw_varint
)

pw_add_library(pw_containers.generic_var_len_entry_queue_testing INTERFACE
  HEADERS
    public/pw_containers/test/generic_var_len_entry_queue_testing.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_unit_test
)

pw_add_library(pw_containers.inline_var_len_entry_queue STATIC
  HEADERS
    public/pw_containers/inline_var_len_entry_queue.h
  PUBLIC_INCLUDES
    public
  SOURCES
    inline_var_len_entry_queue.cc
  PUBLIC_DEPS
    pw_bytes
    pw_containers.generic_var_len_entry_queue
    pw_containers._raw_storage
    pw_preprocessor
    pw_span
    pw_toolchain.constexpr_tag
    pw_varint
)

pw_add_library(pw_containers.var_len_entry_queue INTERFACE
  HEADERS
    public/pw_containers/var_len_entry_queue.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.generic_var_len_entry_queue
    pw_containers._raw_storage
    pw_span
)

pw_add_library(pw_containers.vector INTERFACE
  HEADERS
    public/pw_containers/vector.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_containers.storage
    pw_preprocessor
    pw_toolchain.constexpr_tag
)

pw_add_library(pw_containers.dynamic_vector INTERFACE
  HEADERS
    public/pw_containers/dynamic_vector.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_containers.dynamic_deque
    pw_containers.iterator
)

pw_add_library(pw_containers.dynamic_ptr_vector INTERFACE
  HEADERS
    public/pw_containers/dynamic_ptr_vector.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_containers.dynamic_vector
)

pw_add_library(pw_containers.wrapped_iterator INTERFACE
  HEADERS
    public/pw_containers/wrapped_iterator.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_containers.intrusive_item STATIC
  HEADERS
    public/pw_containers/internal/intrusive_item.h
  PUBLIC_INCLUDES
    public
  SOURCES
    intrusive_item.cc
  PRIVATE_DEPS
    pw_assert
)

pw_add_library(pw_containers.intrusive_list_common INTERFACE
  HEADERS
    public/pw_containers/internal/intrusive_list.h
    public/pw_containers/internal/intrusive_list_item.h
    public/pw_containers/internal/intrusive_list_iterator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.intrusive_item
)

pw_add_library(pw_containers.intrusive_forward_list INTERFACE
  HEADERS
    public/pw_containers/intrusive_forward_list.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.config
    pw_containers.intrusive_list_common
)

pw_add_library(pw_containers.intrusive_list INTERFACE
  HEADERS
    public/pw_containers/intrusive_list.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.config
    pw_containers.intrusive_list_common
    pw_containers.legacy_intrusive_list
)

pw_add_library(pw_containers.legacy_intrusive_list INTERFACE
  HEADERS
    public/pw_containers/internal/legacy_intrusive_list.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.intrusive_forward_list
)

pw_add_library(pw_containers.aa_tree STATIC
  HEADERS
    public/pw_containers/internal/aa_tree.h
    public/pw_containers/internal/aa_tree_item.h
    public/pw_containers/internal/aa_tree_iterator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_bytes.packed_ptr
    pw_containers.intrusive_item
    pw_function
  SOURCES
    aa_tree.cc
    aa_tree_item.cc
)

pw_add_library(pw_containers.intrusive_map INTERFACE
  HEADERS
    public/pw_containers/intrusive_map.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.aa_tree
)

pw_add_library(pw_containers.intrusive_multimap INTERFACE
  HEADERS
    public/pw_containers/intrusive_multimap.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.aa_tree
)

pw_add_library(pw_containers.intrusive_multiset INTERFACE
  HEADERS
    public/pw_containers/intrusive_multiset.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.aa_tree
)

pw_add_library(pw_containers.intrusive_set INTERFACE
  HEADERS
    public/pw_containers/intrusive_set.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.aa_tree
)

pw_add_library(pw_containers.optional_tuple INTERFACE
  HEADERS
    public/pw_containers/optional_tuple.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.bitset
    pw_polyfill
    pw_third_party.fuchsia.stdcompat
)

pw_add_test(pw_containers.algorithm_test
  SOURCES
    algorithm_test.cc
  PRIVATE_DEPS
    pw_containers.algorithm
    pw_containers.flat_map
    pw_containers.intrusive_list
    pw_containers.vector
    pw_span
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.filtered_view_test
  SOURCES
    filtered_view_test.cc
  PRIVATE_DEPS
    pw_containers.algorithm
    pw_containers.filtered_view
    pw_containers.flat_map
    pw_containers.intrusive_list
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.flat_map_test
  SOURCES
    flat_map_test.cc
  PRIVATE_DEPS
    pw_containers.flat_map
    pw_polyfill
  GROUPS
    modules
    pw_containers
    pw_polyfill
)

pw_add_test(pw_containers.functional_test
  SOURCES
    functional_test.cc
  PRIVATE_DEPS
    pw_containers.functional
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.dynamic_deque_test
  SOURCES
    dynamic_deque_test.cc
  PRIVATE_DEPS
    pw_allocator.null_allocator
    pw_allocator.testing
    pw_containers.algorithm
    pw_containers.dynamic_deque
    pw_containers._test_helpers
    pw_polyfill
)

pw_add_test(pw_containers.inline_deque_test
  SOURCES
    inline_deque_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.inline_deque
    pw_containers._test_helpers
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.inline_async_deque_test
  SOURCES
    inline_async_deque_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_containers.inline_async_deque
    pw_containers._test_helpers
    pw_async2.testing
    pw_status
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.dynamic_queue_test
  SOURCES
    dynamic_queue_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_containers.dynamic_queue
    pw_containers._test_helpers
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.deque_test
  SOURCES
    deque_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.deque
    pw_containers._test_helpers
    pw_allocator.null_allocator
    pw_allocator.testing
    pw_assert
    pw_polyfill
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.fixed_deque_allocated_common_test
  SOURCES
    fixed_deque_allocated_common_test.cc
  PRIVATE_DEPS
    pw_containers.deque
    pw_containers._test_helpers
    pw_allocator.testing
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.fixed_deque_static_common_test
  SOURCES
    fixed_deque_static_common_test.cc
  PRIVATE_DEPS
    pw_containers.deque
    pw_containers._test_helpers
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.queue_test
  SOURCES
    queue_test.cc
  PRIVATE_DEPS
    pw_containers.queue
    pw_containers._test_helpers
    pw_allocator.testing
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.inline_queue_test
  SOURCES
    inline_queue_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.algorithm
    pw_containers.inline_queue
    pw_containers._test_helpers
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.inline_async_queue_test
  SOURCES
    inline_async_queue_test.cc
  PRIVATE_DEPS
    pw_containers.inline_async_queue
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.testing
    pw_preprocessor
    pw_status
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.raw_storage_test
  SOURCES
    raw_storage_test.cc
  PRIVATE_DEPS
    pw_containers._raw_storage
    pw_containers._test_helpers
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.storage_test
  SOURCES
    storage_test.cc
  PRIVATE_DEPS
    pw_containers.storage
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.to_array_test
  SOURCES
    to_array_test.cc
  PRIVATE_DEPS
    pw_containers.to_array
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.var_len_entry_test
  SOURCES
    var_len_entry_test.cc
  PRIVATE_DEPS
    pw_containers.generic_var_len_entry_queue
    pw_containers.var_len_entry_queue
    pw_bytes
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.var_len_entry_iterator_test
  SOURCES
    var_len_entry_iterator_test.cc
  PRIVATE_DEPS
    pw_containers.generic_var_len_entry_queue
    pw_containers.var_len_entry_queue
    pw_bytes
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.var_len_entry_queue_iterator_test
  SOURCES
    var_len_entry_queue_iterator_test.cc
  PRIVATE_DEPS
    pw_containers.generic_var_len_entry_queue
    pw_containers.var_len_entry_queue
    pw_bytes
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.inline_var_len_entry_queue_test
  SOURCES
    pw_containers_private/inline_var_len_entry_queue_test_oracle.h
    inline_var_len_entry_queue_test.cc
  PRIVATE_DEPS
    pw_assert
    pw_bytes
    pw_containers.generic_var_len_entry_queue_testing
    pw_containers.inline_var_len_entry_queue
    pw_span
)

pw_add_test(pw_containers.var_len_entry_queue_test
  SOURCES
    var_len_entry_queue_test.cc
  PRIVATE_DEPS
    pw_assert
    pw_containers.generic_var_len_entry_queue
    pw_containers.generic_var_len_entry_queue_testing
    pw_containers.var_len_entry_queue
    pw_span
)

pw_add_test(pw_containers.vector_test
  SOURCES
    vector_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers._test_helpers
    pw_containers.vector
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.dynamic_vector_test
  SOURCES
    dynamic_vector_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_containers._test_helpers
    pw_containers.dynamic_vector
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.dynamic_ptr_vector_test
  SOURCES
    dynamic_ptr_vector_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_allocator.testing
    pw_containers._test_helpers
    pw_containers.dynamic_ptr_vector
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.wrapped_iterator_test
  SOURCES
    wrapped_iterator_test.cc
  PRIVATE_DEPS
    pw_containers.wrapped_iterator
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_forward_list_test
  SOURCES
    intrusive_forward_list_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_forward_list
    pw_containers.vector
    pw_preprocessor
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_list_test
  SOURCES
    intrusive_list_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_list
    pw_containers.vector
    pw_preprocessor
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_map_test
  SOURCES
    intrusive_map_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_map
    pw_containers.intrusive_multimap
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_multimap_test
  SOURCES
    intrusive_multimap_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_map
    pw_containers.intrusive_multimap
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_set_test
  SOURCES
    intrusive_set_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_set
    pw_containers.intrusive_multiset
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_multiset_test
  SOURCES
    intrusive_multiset_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_set
    pw_containers.intrusive_multiset
    pw_span
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.intrusive_item_test
  SOURCES
    intrusive_item_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers.intrusive_forward_list
    pw_containers.intrusive_list
    pw_containers.intrusive_map
    pw_containers.intrusive_multimap
    pw_containers.intrusive_set
    pw_containers.intrusive_multiset
  GROUPS
    modules
    pw_containers
)

pw_add_test(pw_containers.optional_tuple_test
  SOURCES
    optional_tuple_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_containers._test_helpers
    pw_containers.optional_tuple
    pw_polyfill
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_containers
)

add_subdirectory(examples)
