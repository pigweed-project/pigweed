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

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

# This must be an OBJECT library so that overridden weak standard library
# symbols (like std::__libcpp_verbose_abort) are always pulled in during link.
pw_add_library(pw_libcxx.libcxx_hooks OBJECT
  SOURCES
    __cxa_deleted_virtual.cc
    __cxa_pure_virtual.cc
    operator_delete.cc
    operator_new.cc
    verbose_abort.cc
  PRIVATE_DEPS
    pw_assert.check
    pw_string
)
