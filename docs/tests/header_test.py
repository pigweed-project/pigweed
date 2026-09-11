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
"""Header tests for pigweed.dev."""

import unittest

from helpers import (
    get_docs_dir,
    get_html_files,
)


class HeaderTest(unittest.TestCase):
    """Header tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.html_files = get_html_files(cls.docs_dir)

    def test_header_present(self):
        """Verifies that <pw-header id="pw-header"> is present on every HTML
        page."""
        self.assertGreater(
            len(self.html_files),
            0,
            f"No HTML files found under {self.docs_dir}",
        )

        missing_header: list[str] = []

        for html_file in self.html_files:
            rel_path = html_file.relative_to(self.docs_dir)
            content = html_file.read_text(encoding="utf-8", errors="ignore")
            if '<pw-header id="pw-header">' not in content:
                missing_header.append(str(rel_path))

        self.assertEqual(
            len(missing_header),
            0,
            f"Found {len(missing_header)} pages missing "
            f'<pw-header id="pw-header">:\n' + "\n".join(missing_header[:20]),
        )


if __name__ == "__main__":
    unittest.main()
