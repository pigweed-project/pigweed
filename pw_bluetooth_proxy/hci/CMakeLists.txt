# Copyright 2025 The Pigweed Authors
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

###############################################################################
##          Everything below here is intended to be emboss only              ##
##          and will be skipped if emboss isn't enabled.                     ##
###############################################################################
if("${dir_pw_third_party_emboss}" STREQUAL "")
  # Skip emboss defs if it's not configured
  return()
endif()

pw_add_library(pw_bluetooth_proxy.hci.command_multiplexer STATIC
# LINT.IfChange
  HEADERS
    public/pw_bluetooth_proxy/hci/command_multiplexer.h
    public/pw_bluetooth_proxy/hci/internal/type_safe_ids.h
  PUBLIC_DEPS
    pw_bluetooth.emboss_hci_common
    pw_bluetooth.emboss_hci_commands
    pw_bluetooth.emboss_hci_events
    pw_bluetooth.emboss_hci_h4
    pw_containers.flat_map
    pw_function
    pw_result
    pw_result.expected
    pw_span
    pw_status
    pw_sync.mutex
  PRIVATE_DEPS
    pw_assert.check
  SOURCES
    command_multiplexer.cc
# LINT.ThenChange(Android.bp, BUILD.bazel, BUILD.gn)
)

pw_add_test(pw_bluetooth_proxy.hci.command_multiplexer_test
# LINT.IfChange
  PRIVATE_DEPS
    pw_bluetooth_proxy.hci.command_multiplexer
    pw_allocator.testing
    pw_async2.func_task
    pw_async2.simulated_time_provider
    pw_async2.testing
    pw_containers.vector
    pw_unit_test
  SOURCES
    command_multiplexer_test.cc
# LINT.ThenChange(Android.bp, BUILD.bazel, BUILD.gn)
)
