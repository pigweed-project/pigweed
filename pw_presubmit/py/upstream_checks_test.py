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
"""Tests for upstream_checks."""

import datetime
from pathlib import Path
import unittest
from unittest.mock import MagicMock, mock_open, patch

from pw_presubmit import upstream_checks
from pw_presubmit.private.step import Context


class TestCopyrightNotice(unittest.TestCase):
    """Tests for copyright notice check and fix."""

    def setUp(self) -> None:
        self.ctx = MagicMock(spec=Context)
        self.ctx.fail = MagicMock()
        self.ctx.paths = []

    def test_valid_notice(self) -> None:
        year = datetime.date.today().year
        contents = f"""# Copyright {year} The Pigweed Authors
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
"""
        path = MagicMock(spec=Path)
        path.suffix = '.py'
        path.stat.return_value.st_size = len(contents)
        path.open = mock_open(read_data=contents)

        self.ctx.paths = [path]
        upstream_checks.copyright_notice.run(self.ctx)
        self.ctx.fail.assert_not_called()

    def test_missing_notice_python(self) -> None:
        contents = "print('hello')\n"
        path = MagicMock(spec=Path)
        path.suffix = '.py'
        path.stat.return_value.st_size = len(contents)
        m = mock_open(read_data=contents)
        path.open = m

        self.ctx.paths = [path]

        upstream_checks.copyright_notice.fix(self.ctx)

        # Verify write was called
        handle = m()
        handle.writelines.assert_called_once()
        args, _ = handle.writelines.call_args
        written_lines = args[0]
        self.assertTrue(written_lines[0].startswith('# Copyright'))
        self.assertEqual(written_lines[-1], "print('hello')\n")

    def test_missing_notice_cpp(self) -> None:
        contents = "int main() {}\n"
        path = MagicMock(spec=Path)
        path.suffix = '.cc'
        path.stat.return_value.st_size = len(contents)
        m = mock_open(read_data=contents)
        path.open = m

        self.ctx.paths = [path]

        upstream_checks.copyright_notice.fix(self.ctx)

        handle = m()
        handle.writelines.assert_called_once()
        args, _ = handle.writelines.call_args
        written_lines = args[0]
        self.assertTrue(written_lines[0].startswith('// Copyright'))

    def test_typo_detection(self) -> None:
        year = datetime.date.today().year
        contents = f"""# Copyrite {year} The Pigweed Authors
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
"""
        path = MagicMock(spec=Path)
        path.suffix = '.py'
        path.stat.return_value.st_size = len(contents)
        path.open = mock_open(read_data=contents)

        self.ctx.paths = [path]

        with patch('pw_presubmit.upstream_checks._LOG') as mock_log:
            upstream_checks.copyright_notice.fix(self.ctx)
            self.ctx.fail.assert_called_once()
            mock_log.warning.assert_called_with(
                '%s: Copyright notice appears to be present but is malformed. '
                'Please fix manually.',
                path,
            )

    def test_skip_prefixes(self) -> None:
        contents = """#!/usr/bin/env python3
print('hello')
"""
        path = MagicMock(spec=Path)
        path.suffix = '.py'
        path.stat.return_value.st_size = len(contents)
        m = mock_open(read_data=contents)
        path.open = m

        self.ctx.paths = [path]

        upstream_checks.copyright_notice.fix(self.ctx)

        handle = m()
        handle.writelines.assert_called_once()
        args, _ = handle.writelines.call_args
        written_lines = args[0]
        # Notice should be inserted AFTER shebang
        self.assertEqual(written_lines[0], "#!/usr/bin/env python3\n")
        self.assertTrue(written_lines[1].startswith('# Copyright'))

    def test_typo_detection_ratio(self) -> None:
        year = datetime.date.today().year
        contents = f"""# Copyright {year} The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may NOT
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
"""
        path = MagicMock(spec=Path)
        path.suffix = '.py'
        path.stat.return_value.st_size = len(contents)
        path.open = mock_open(read_data=contents)

        self.ctx.paths = [path]

        with patch('pw_presubmit.upstream_checks._LOG') as mock_log:
            upstream_checks.copyright_notice.fix(self.ctx)
            self.ctx.fail.assert_called_once()
            mock_log.warning.assert_called_with(
                '%s: Copyright notice appears to be present but is malformed. '
                'Please fix manually.',
                path,
            )


if __name__ == '__main__':
    unittest.main()
