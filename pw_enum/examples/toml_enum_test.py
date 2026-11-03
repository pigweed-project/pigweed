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
"""Example of using Python enums generated from TOML."""

import unittest

# DOCSTAG: [pw_enum-examples-toml-py]
# Generated from pw_enum/examples/enum_example/sensor.toml.
from pw_enum.examples.enum_example.sensor import SensorState, SensorStatus

# DOCSTAG: [pw_enum-examples-toml-py]


class TomlEnumExampleTest(unittest.TestCase):
    """Tests the TOML enum example."""

    def test_usage(self) -> None:
        # DOCSTAG: [pw_enum-examples-toml-py-usage]
        state = SensorState(0x80)
        self.assertIs(state, SensorState.FAULT)
        self.assertEqual(str(state), "FAULT")  # Matches pw::EnumToString
        self.assertFalse(SensorState.is_valid(4))
        self.assertEqual(SensorStatus.NOT_READY, 1)  # IntEnum: compares to int
        self.assertFalse(SensorStatus.NOT_READY.ok())
        # DOCSTAG: [pw_enum-examples-toml-py-usage]


if __name__ == "__main__":
    unittest.main()
