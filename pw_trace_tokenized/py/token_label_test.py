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
"""Tests for token_label."""

import json
import struct
import unittest

from pw_tokenizer import tokens
from pw_trace import trace
from pw_trace_tokenized import token_label


class TestTokenLabel(unittest.TestCase):
    """Tests for token_label."""

    def test_generate_json_custom_fmt_token_label(self) -> None:
        """Tests custom data format handler."""

        db = tokens.Database(
            [tokens.TokenizedStringEntry(0x12345678, "custom_label")]
        )
        token_label.register_handler(db)

        event = trace.TraceEvent(
            event_type=trace.TraceType.INSTANTANEOUS,
            module="module",
            label="label",  # should be replaced
            timestamp_us=10,
            has_data=True,
            data_fmt="@pw_trace_tokenized_token_label",
            data=struct.pack("<I", 0x12345678),
        )
        json_lines = trace.generate_trace_json([event])
        self.assertEqual(1, len(json_lines))
        self.assertEqual(
            json.loads(json_lines[0]),
            {
                "ph": "I",
                "pid": "module",
                "name": "custom_label",
                "ts": 10,
                "s": "p",
            },
        )

    def test_token_not_found_in_db(self) -> None:
        """Tests when token is not present in the tokenizer database."""
        db = tokens.Database([])
        token_label.register_handler(db)

        event = trace.TraceEvent(
            event_type=trace.TraceType.INSTANTANEOUS,
            module="module",
            label="label",
            timestamp_us=10,
            has_data=True,
            data_fmt="@pw_trace_tokenized_token_label",
            data=struct.pack("<I", 0x12345678),
        )
        json_lines = trace.generate_trace_json([event])

        self.assertEqual(1, len(json_lines))
        self.assertEqual(
            json.loads(json_lines[0]),
            {
                "ph": "I",
                "pid": "module",
                "name": "label",
                "ts": 10,
                "s": "p",
                "args": {
                    "error": (
                        "Plugin handler "
                        "@pw_trace_tokenized_token_label failed: "
                        "Token 0x12345678 not found in database"
                    )
                },
                "data": "78563412",
            },
        )

    def test_token_conflict_multiple_entries(self) -> None:
        """Tests when token has multiple entries (conflict) in the database."""
        db = tokens.Database(
            [
                tokens.TokenizedStringEntry(0x12345678, "label_one"),
                tokens.TokenizedStringEntry(0x12345678, "label_two"),
            ]
        )
        token_label.register_handler(db)

        event = trace.TraceEvent(
            event_type=trace.TraceType.INSTANTANEOUS,
            module="module",
            label="label",
            timestamp_us=10,
            has_data=True,
            data_fmt="@pw_trace_tokenized_token_label",
            data=struct.pack("<I", 0x12345678),
        )
        json_lines = trace.generate_trace_json([event])

        self.assertEqual(1, len(json_lines))
        self.assertEqual(
            json.loads(json_lines[0]),
            {
                "ph": "I",
                "pid": "module",
                "name": "label",
                "ts": 10,
                "s": "p",
                "args": {
                    "error": (
                        "Plugin handler "
                        "@pw_trace_tokenized_token_label failed: "
                        "Token conflict: 0x12345678 has "
                        "multiple entries in database"
                    )
                },
                "data": "78563412",
            },
        )

    def test_data_less_than_four_bytes(self) -> None:
        """Tests when the trace event data is less than 4 bytes."""
        db = tokens.Database(
            [tokens.TokenizedStringEntry(0x12345678, "custom_label")]
        )
        token_label.register_handler(db)

        event = trace.TraceEvent(
            event_type=trace.TraceType.INSTANTANEOUS,
            module="module",
            label="label",
            timestamp_us=10,
            has_data=True,
            data_fmt="@pw_trace_tokenized_token_label",
            data=struct.pack("<H", 0x1234),  # 2 bytes
        )
        json_lines = trace.generate_trace_json([event])

        self.assertEqual(1, len(json_lines))
        self.assertEqual(
            json.loads(json_lines[0]),
            {
                "ph": "I",
                "pid": "module",
                "name": "label",
                "ts": 10,
                "s": "p",
                "args": {
                    "error": (
                        "Plugin handler "
                        "@pw_trace_tokenized_token_label failed: "
                        "Mismatched data format "
                        "@pw_trace_tokenized_token_label: "
                        "expected 4 bytes, got 2 bytes"
                    )
                },
                "data": "3412",
            },
        )


if __name__ == '__main__':
    unittest.main()
