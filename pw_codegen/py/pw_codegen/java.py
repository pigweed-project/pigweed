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
"""Java source code generation utilities."""

from __future__ import annotations

from pw_codegen import _escape_string_literal

_STRING_ESCAPES: dict[str, str] = {
    "\\": r"\\",
    '"': r"\"",
    "\b": r"\b",
    "\f": r"\f",
    "\n": r"\n",
    "\r": r"\r",
    "\t": r"\t",
}


def string_literal(data: str) -> str:
    """Formats ``data`` as a quoted Java string literal."""
    return _escape_string_literal(data, _STRING_ESCAPES, r"\u{:04x}")


def byte_array(data: bytes) -> str:
    """Formats ``data`` as a Java ``byte[]`` literal."""
    elements = ", ".join(f"(byte) 0x{byte:02x}" for byte in data)
    return f"new byte[] {{{elements}}}"
