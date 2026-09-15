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
"""Tests for the FreshnessTriager class in pw_fortifier."""
# pylint: disable=protected-access
import asyncio
from datetime import date
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.freshness_triager import FreshnessTriager
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestFreshnessTriager(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the FreshnessTriager class."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    @patch(
        'pw_fortifier.triager.CoreOwnerFinder.find',
        return_value='assignee@google.com',
    )
    async def test_triage_freshness_result_success(self, _mock_find) -> None:
        """Tests that FreshnessTriager correctly modifies a FreshnessResult."""
        triager = FreshnessTriager()

        next_stage = PipelineSink()
        triager.connect(next_stage)

        await configure_stage_for_test(
            triager,
            working_dir=str(self.working_dir),
        )

        # Create input freshness result (tier 0)
        result = FreshnessResult(
            package='test-package',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=0,
        )
        test_file = AsyncPath(self.working_dir, 'result-1.json')
        await result.save(test_file)

        # Run triager
        run_task = asyncio.create_task(triager.run())
        await triager.input_queue.put(test_file)
        await triager.input_queue.put(None)
        await run_task

        # Verify output
        self.assertEqual(next_stage.input_queue.qsize(), 2)
        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None

        triaged_result = await FreshnessResult.load(forwarded_path)
        self.assertEqual(triaged_result.priority, 1)  # tier 0 // 2 + 1 = 1
        self.assertEqual(triaged_result.severity, 2)  # tier 0 // 2 + 2 = 2
        self.assertEqual(triaged_result.assignee, 'assignee@google.com')

    async def test_triage_tier2_priority_and_severity(self) -> None:
        """Tests priority and severity computation for tier 2 dependencies."""
        triager = FreshnessTriager()

        result = FreshnessResult(
            package='dev-package',
            location=CodeSnippet(file='requirements.txt'),
            pkg_type='pip',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.0', date(2026, 6, 1)),
            tier=2,
        )
        await triager._triage(result)

        self.assertEqual(result.priority, 2)  # tier 2 // 2 + 1 = 2
        self.assertEqual(result.severity, 3)  # tier 2 // 2 + 2 = 3


if __name__ == '__main__':
    unittest.main()
