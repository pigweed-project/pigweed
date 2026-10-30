#!/usr/bin/env python3
# Copyright 2022 The Pigweed Authors
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
"""Fixes identifiers that would cause compiler errors in generated C++ code."""

from __future__ import annotations

from pw_codegen import cpp

# Identifiers that conflict with pw_protobuf codegen internals when used in
# certain contexts:
_EXTRA_RESERVED_WORDS = ("Fields", "Message")


def fix_cc_identifier(proto_identifier: str) -> str:
    """Adjusts the identifier for use in generated C++ code."""
    return cpp.fix_identifier(proto_identifier, _EXTRA_RESERVED_WORDS)


def fix_cc_enum_value_name(proto_enum_entry: str) -> str:
    """Adjusts the enum-value name for use in generated C++ code."""
    return cpp.fix_enum_value_name(proto_enum_entry, _EXTRA_RESERVED_WORDS)
