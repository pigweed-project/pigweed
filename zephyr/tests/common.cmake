# Copyright 2026 The Pigweed Authors
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

# Shared setup for the Zephyr integration test applications in this directory.
# Include it after find_package(Zephyr).

# Zephyr adds the -Wno-volatile flag in cmake/compiler/gcc/compiler_flags.cmake
# this is a gcc flag which is added in the clang builds too. Pigweed's clang
# doesn't seem to support this flag so we'll remove it here.
if("${COMPILER}" STREQUAL "clang")
  foreach(dialect IN ITEMS cpp2a cpp20 cpp2b)
    get_property(flags TARGET compiler-cpp PROPERTY "dialect_${dialect}")
    if(flags)
      list(REMOVE_ITEM flags "-Wno-volatile")
      set_property(TARGET compiler-cpp PROPERTY "dialect_${dialect}" ${flags})
    endif()
  endforeach()
endif()
