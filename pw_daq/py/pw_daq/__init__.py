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
"""Pigweed Data Acquisition (DAQ) package and CLI."""

# The `pw_daq` package is split across multiple Bazel `pw_py_library` targets
# (`common`, `backend`, `pw_daq`, and backend subpackages), so
# `pkgutil.extend_path()` merges their package directories across `sys.path`
# before submodule imports are resolved.
__path__ = __import__("pkgutil").extend_path(__path__, __name__)

from pw_daq.common import (
    ActionResult,
    BlockType,
    DaqError,
    DaqTimeoutError,
    DigitalDirection,
    DigitalPull,
    DigitalState,
    EnumValue,
    EventStream,
    UartParity,
    Value,
    ValueType,
)
from pw_daq.lib import (
    SUPPORTED_BLOCK_TYPES,
    Action,
    Attribute,
    AttributeMutability,
    Block,
    Daq,
    DigitalBlock,
    Event,
    Signal,
    UartBlock,
)

__all__ = [
    "SUPPORTED_BLOCK_TYPES",
    "Action",
    "ActionResult",
    "Attribute",
    "AttributeMutability",
    "Block",
    "BlockType",
    "Daq",
    "DaqError",
    "DaqTimeoutError",
    "DigitalBlock",
    "DigitalDirection",
    "DigitalPull",
    "DigitalState",
    "EnumValue",
    "Event",
    "EventStream",
    "Signal",
    "UartBlock",
    "UartParity",
    "Value",
    "ValueType",
]
