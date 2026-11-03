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
include($ENV{PW_ROOT}/pw_enum/pw_enum.cmake)

pw_add_enum(pw_rpc2.rpc_code
  SOURCES
    rpc_code.toml
  STRIP_PREFIX
    .
  PREFIX
    pw_rpc2
)

pw_add_enum(pw_rpc2._packet_type_enum
  SOURCES
    internal/packet_type_enum.toml
  STRIP_PREFIX
    .
  PREFIX
    pw_rpc2
)

pw_add_enum(pw_rpc2._protocol_error_enum
  SOURCES
    internal/protocol_error_enum.toml
  STRIP_PREFIX
    .
  PREFIX
    pw_rpc2
)

add_subdirectory(cpp)
