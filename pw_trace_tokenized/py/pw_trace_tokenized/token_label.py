#!/usr/bin/env python3
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
"""Implement handler for @pw_trace_tokenized_token_label

The plugin format uses a token (little-endian, first 4 bytes of the data
field) to represent trace event label.
"""

import struct
from typing import Any
from pw_tokenizer import tokens
from pw_trace import trace


def register_handler(db: tokens.Database):
    """Registers a handler for @pw_trace_tokenized_token_label."""
    plugin_format = "@pw_trace_tokenized_token_label"

    def handler(event: trace.TraceEvent, line: dict[str, Any]):
        if len(event.data) != 4:
            raise ValueError(
                f"Mismatched data format {event.data_fmt}: expected 4 bytes, "
                f"got {len(event.data)} bytes"
            )

        token = struct.unpack('<I', event.data[:4])[0]
        entries = db.token_to_entries.get(token)
        if not entries:
            raise ValueError(f"Token 0x{token:08x} not found in database")
        if len(entries) > 1:
            raise ValueError(
                f"Token conflict: 0x{token:08x} has "
                "multiple entries in database"
            )
        line["name"] = str(entries[0])

    trace.register_plugin_data_format_handler(plugin_format, handler)
