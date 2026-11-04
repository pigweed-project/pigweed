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
"""Tests pw_py_test macro expansion from an external root workspace."""

import unittest

from pw_status import Status


class NonRootPyTest(unittest.TestCase):
    """Verifies @pigweed Python libraries work from a non-root workspace."""

    def test_pw_status_from_external_workspace(self) -> None:
        self.assertTrue(Status.OK.ok())
        self.assertFalse(Status.NOT_FOUND.ok())


if __name__ == '__main__':
    unittest.main()
