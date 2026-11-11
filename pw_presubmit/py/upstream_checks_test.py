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
import re
import tempfile
import unittest
from unittest.mock import MagicMock, mock_open, patch

from pw_cli.file_filter import FileFilter
from pw_presubmit import source_in_build, upstream_checks
from pw_presubmit.presubmit_context import FormatOptions, PresubmitContext
from pw_presubmit.private.events import PresubmitEvents
from pw_presubmit.private.orchestrator import Orchestrator
from pw_presubmit.private.result import PresubmitResult
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


class TestSourceFilesFilter(unittest.TestCase):
    """Tests for file filters in upstream_checks."""

    def test_gn_exclude_perf_test(self) -> None:
        self.assertFalse(
            upstream_checks.SOURCE_FILES_FILTER_GN_EXCLUDE.matches(
                'pw_checksum/crc32_perf_test.cc'
            )
        )
        self.assertFalse(
            upstream_checks.SOURCE_FILES_FILTER_GN_EXCLUDE.matches(
                'pw_protobuf/encoder_perf_test.cc'
            )
        )
        self.assertFalse(
            upstream_checks.SOURCE_FILES_FILTER_GN_EXCLUDE.matches(
                'pw_tokenizer/detokenize_perf_test.cc'
            )
        )
        self.assertTrue(
            upstream_checks.SOURCE_FILES_FILTER_GN_EXCLUDE.matches(
                'pw_perf_test/perf_test.cc'
            )
        )
        self.assertTrue(
            upstream_checks.SOURCE_FILES_FILTER_GN_EXCLUDE.matches(
                'pw_checksum/crc32.cc'
            )
        )

    def test_bazel_exclude_docs_tests_anchored_to_root(self) -> None:
        self.assertFalse(
            upstream_checks.SOURCE_FILES_FILTER_BAZEL_EXCLUDE.matches(
                'docs/tests/head_test.py'
            )
        )
        self.assertFalse(
            upstream_checks.SOURCE_FILES_FILTER_BAZEL_EXCLUDE.matches(
                '.agents/skills/docs/tests/foo.py'
            )
        )
        self.assertTrue(
            upstream_checks.SOURCE_FILES_FILTER_BAZEL_EXCLUDE.matches(
                'pw_foo/docs/tests/foo_test.py'
            )
        )


class TestSourceInBuild(unittest.TestCase):
    """Tests for source_in_build checks."""

    @patch('pw_presubmit.source_in_build.git_repo.find_git_repo')
    @patch('pw_presubmit.source_in_build.build.check_gn_build_for_files')
    def test_gn_checks_all_paths_via_orchestrator(
        self,
        mock_check_gn: MagicMock,
        mock_find_git_repo: MagicMock,
    ) -> None:
        """Checks that modifying BUILD.gn triggers step and checks all_paths."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            build_gn = root / 'pw_foo/BUILD.gn'
            included_hdr = root / 'pw_foo/public/pw_foo/bar.h'
            excluded_hdr = root / 'pw_kernel/bar.h'

            mock_find_git_repo.return_value.list_files.return_value = [build_gn]
            mock_check_gn.return_value = [included_hdr]

            orchestrator = Orchestrator(
                root=root,
                output_dir=root / 'out',
                paths=(build_gn,),
                all_paths=(build_gn, included_hdr, excluded_hdr),
                events=MagicMock(spec=PresubmitEvents),
            )
            result = orchestrator.run(
                'test', [upstream_checks.source_in_gn_build()]
            )

            self.assertFalse(result.success)
            self.assertEqual(result.failed, ['source_is_in_gn_build'])
            mock_check_gn.assert_called_once()
            checked_paths = mock_check_gn.call_args[0][1]
            self.assertIn(included_hdr, checked_paths)
            self.assertNotIn(excluded_hdr, checked_paths)

    @patch('pw_presubmit.source_in_build.git_repo.find_git_repo')
    @patch('pw_presubmit.source_in_build.build.check_gn_build_for_files')
    def test_step_concat_and_replace_filter(
        self,
        mock_check_gn: MagicMock,
        mock_find_git_repo: MagicMock,
    ) -> None:
        """Checks Step.concat_filter and Step.replace_filter on all_paths."""
        root = Path('/repo')
        build_gn = root / 'pw_foo/BUILD.gn'
        foo_hdr = root / 'pw_foo/public/pw_foo/bar.h'
        bar_hdr = root / 'pw_bar/public/pw_bar/bar.h'
        kernel_hdr = root / 'pw_kernel/bar.h'

        mock_find_git_repo.return_value.list_files.return_value = [build_gn]
        mock_check_gn.return_value = []

        ctx = Context(
            root=root,
            output_dir=root / 'out',
            paths=(build_gn,),
            all_paths=(build_gn, foo_hdr, bar_hdr, kernel_hdr),
            all_modified_paths=(build_gn,),
        )

        # concat_filter adds pw_bar/ exclusion while keeping pw_kernel/ excluded
        concat_step = upstream_checks.source_in_gn_build().concat_filter(
            exclude=(r'\bpw_bar/',)
        )
        concat_step.run(ctx)
        checked_paths = mock_check_gn.call_args[0][1]
        self.assertIn(foo_hdr, checked_paths)
        self.assertNotIn(bar_hdr, checked_paths)
        self.assertNotIn(kernel_hdr, checked_paths)

        # replace_filter replaces default exclusions (so pw_kernel/ is included)
        mock_check_gn.reset_mock()
        replace_step = upstream_checks.source_in_gn_build().replace_filter(
            suffix=('.h',)
        )
        replace_step.run(ctx)
        checked_paths = mock_check_gn.call_args[0][1]
        self.assertIn(foo_hdr, checked_paths)
        self.assertIn(bar_hdr, checked_paths)
        self.assertIn(kernel_hdr, checked_paths)

    @patch('pw_presubmit.source_in_build.git_repo.find_git_repo')
    @patch('pw_presubmit.source_in_build.build.check_gn_build_for_files')
    def test_check_with_file_filter_updates_all_paths_filter(
        self,
        mock_check_gn: MagicMock,
        mock_find_git_repo: MagicMock,
    ) -> None:
        """Checks that Check.with_file_filter applies to ctx.all_paths."""
        root = Path('/repo')
        build_gn = root / 'pw_foo/BUILD.gn'
        included_cc = root / 'pw_foo/foo.cc'
        excluded_cc = root / 'pw_bar/bar.cc'

        mock_find_git_repo.return_value.list_files.return_value = [build_gn]
        mock_check_gn.return_value = []

        ctx = Context(
            root=root,
            output_dir=root / 'out',
            paths=(build_gn,),
            all_paths=(build_gn, included_cc, excluded_cc),
            all_modified_paths=(build_gn,),
        )

        check = source_in_build.gn(
            upstream_checks.SOURCE_FILES_FILTER
        ).with_file_filter(FileFilter(exclude=(r'\bpw_bar/',)))
        check(ctx)

        self.assertFalse(ctx.failed)
        mock_check_gn.assert_called_once()
        checked_paths = mock_check_gn.call_args[0][1]
        self.assertIn(included_cc, checked_paths)
        self.assertNotIn(excluded_cc, checked_paths)

    @patch('pw_presubmit.source_in_build.build.check_gn_build_for_files')
    def test_format_options_exclude_and_empty_paths_early_return(
        self,
        mock_check_gn: MagicMock,
    ) -> None:
        """Checks FormatOptions exclusions and early return on empty paths."""
        root = Path('/repo')
        bar_cc = root / 'pw_bar/bar.cc'
        other_cc = root / 'other/other.cc'
        ctx = PresubmitContext.create_for_testing(
            root=root,
            repos=(root,),
            paths=(bar_cc, other_cc),
            all_paths=(bar_cc, other_cc),
            format_options=FormatOptions(exclude=(re.compile(r'\bother/'),)),
        )
        check = source_in_build.gn(
            upstream_checks.SOURCE_FILES_FILTER
        ).with_filter(exclude=(r'\bpw_bar/',))
        self.assertEqual(check(ctx), PresubmitResult.PASS)
        mock_check_gn.assert_not_called()

    @patch(
        'pw_presubmit.source_in_build.build.check_bazel_build_for_files',
        side_effect=upstream_checks.PresubmitFailure('bazel query failed'),
    )
    def test_bazel_step_propagates_presubmit_failure(
        self, _mock_check_bazel: MagicMock
    ) -> None:
        """Checks that _SourceInBuildStep propagates PresubmitFailure."""
        root = Path('/repo')
        build_bazel = root / 'pw_foo/BUILD.bazel'
        ctx = Context(
            root=root,
            output_dir=root / 'out',
            paths=(build_bazel,),
            all_paths=(build_bazel, root / 'pw_foo/foo.cc'),
            all_modified_paths=(build_bazel,),
        )
        with self.assertRaises(upstream_checks.PresubmitFailure):
            upstream_checks.source_in_bazel_build().run(ctx)

    def test_build_file_patterns_added_when_positive_filters_present(
        self,
    ) -> None:
        """Checks build-file patterns are added to positive source filters."""
        src_only = FileFilter(suffix=('.cc', '.h'), exclude=(r'\bexcluded/',))

        bazel_check = source_in_build.bazel(src_only)
        self.assertTrue(bazel_check.filter.matches('pw_foo/BUILD.bazel'))
        self.assertTrue(bazel_check.filter.matches('pw_foo/BUILD'))
        self.assertTrue(bazel_check.filter.matches('.bazelignore'))
        self.assertFalse(bazel_check.filter.matches('excluded/BUILD.bazel'))
        self.assertFalse(bazel_check.filter.matches('pw_foo/README.md'))

        gn_check = source_in_build.gn(src_only)
        self.assertTrue(gn_check.filter.matches('pw_foo/BUILD.gn'))
        self.assertTrue(gn_check.filter.matches('pw_foo/foo.gni'))
        self.assertFalse(gn_check.filter.matches('pw_foo/README.md'))

        cmake_check = source_in_build.cmake(src_only, lambda _: None)
        self.assertTrue(cmake_check.filter.matches('pw_foo/CMakeLists.txt'))
        self.assertTrue(cmake_check.filter.matches('pw_foo/foo.cmake'))
        self.assertFalse(cmake_check.filter.matches('pw_foo/README.md'))

        soong_check = source_in_build.soong(src_only)
        self.assertTrue(soong_check.filter.matches('pw_foo/Android.bp'))
        self.assertFalse(soong_check.filter.matches('pw_foo/README.md'))

        # Filter with no positive matchers keeps matching all non-excluded files
        exclude_only = FileFilter(exclude=(r'\bexcluded/',))
        gn_exclude_only = source_in_build.gn(exclude_only)
        self.assertTrue(gn_exclude_only.filter.matches('pw_foo/foo.cc'))
        self.assertFalse(gn_exclude_only.filter.matches('excluded/foo.cc'))


if __name__ == '__main__':
    unittest.main()
