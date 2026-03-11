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
"""Custom toolchain action names unique to Pigweed."""

PW_OBJDUMP_DISASSEMBLE = "objdump-disassemble"
PW_READELF = "readelf"
PW_NM = "nm"
PW_SIZE = "size"
PW_COV = "cov"
PW_GCOV = "gcov"

PW_ACTION_NAMES = struct(
    objdump_disassemble = PW_OBJDUMP_DISASSEMBLE,
    readelf = PW_READELF,
    nm = PW_NM,
    size = PW_SIZE,
    cov = PW_COV,
    gcov = PW_GCOV,
)
