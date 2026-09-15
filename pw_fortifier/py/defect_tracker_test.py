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
"""Tests for defect_tracker classes."""

# pylint: disable=protected-access

import asyncio
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.defect import Defect
from pw_fortifier.defect_tracker import (
    DefectReader,
    DefectSummarizer,
    DefectWriter,
)
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTrackerStub
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class StubDefectSummarizer(DefectSummarizer):
    """Stub implementation of DefectSummarizer for testing."""

    def summarize(self, defect: Defect) -> tuple[str, str]:
        """Returns stubbed vulnerability code and summary."""
        return ('PW-SEC-001', 'Potential buffer overflow')


class TestDefectSummarizer(unittest.TestCase):
    """Unit tests for DefectSummarizer."""

    def test_summarize(self) -> None:
        """Tests summarization of a Defect instance."""
        summarizer = StubDefectSummarizer()
        defect = Defect(location=CodeSnippet(file='pw_sync/mutex.cc'))
        code, summary = summarizer.summarize(defect)
        self.assertEqual(code, 'PW-SEC-001')
        self.assertEqual(summary, 'Potential buffer overflow')


class TestDefectReader(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for DefectReader."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_convert_valid_issue(self) -> None:
        """Tests converting a base Issue to a Defect."""
        issue = Issue(
            issue_id=123,
            title='PW-SEC-001 - pw_sync/mutex.cc: Buffer overflow',
            description='Found defect in pw_sync/mutex.cc and pw_sync/mutex.h',
            priority=1,
            severity=2,
            assignee='dev@google.com',
            cl_num=999,
        )
        converted = Defect.from_issue(issue)
        assert converted is not None
        assert isinstance(converted, Defect)
        self.assertEqual(converted.issue_id, 123)
        self.assertEqual(converted.filename, 'pw_sync/mutex.cc')
        self.assertEqual(
            converted.affected_files,
            ['pw_sync/mutex.cc', 'pw_sync/mutex.h'],
        )
        self.assertEqual(converted.priority, 1)
        self.assertEqual(converted.severity, 2)
        self.assertEqual(converted.assignee, 'dev@google.com')
        self.assertEqual(converted.cl_num, 999)

    def test_convert_missing_issue_id(self) -> None:
        """Tests that issues without issue_id return None and log a warning."""
        issue = Issue(
            issue_id=None,
            title='PW-SEC-001 - pw_sync/mutex.cc: Buffer overflow',
        )
        with self.assertLogs('pw_fortifier.defect', level='WARNING') as cm:
            self.assertIsNone(Defect.from_issue(issue))
        self.assertTrue(
            any('missing or invalid issue_id' in log for log in cm.output)
        )

    async def test_run_reads_and_forwards_defects(self) -> None:
        """Tests running DefectReader end-to-end."""
        tracker_stub = IssueTrackerStub()
        tracker_stub.add_issue(
            Issue(
                issue_id=100,
                title='PW-SEC-001 - pw_sync/mutex.cc: Buffer overflow',
                description='Affects pw_sync/mutex.cc',
            )
        )
        tracker_stub.add_issue(
            Issue(
                issue_id=200,
                title='PW-SEC-002 - pw_crypto/aes.cc: Key leak',
            ),
            hotlist_ids=[5],
        )

        reader = DefectReader(tracker_stub)
        next_stage = PipelineSink()
        reader.connect(next_stage)

        await configure_stage_for_test(
            reader,
            working_dir=str(self.working_dir),
            issues=[100],
            hotlists=[5],
        )

        await reader.run()

        results = []
        while not next_stage.input_queue.empty():
            path = await next_stage.input_queue.get()
            if path is None:
                break
            defect = await Defect.load(path)
            results.append((defect.issue_id, defect.filename))

        self.assertEqual(
            results,
            [(100, 'pw_sync/mutex.cc'), (200, 'pw_crypto/aes.cc')],
        )


class TestDefectWriter(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for DefectWriter."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_make_title(self) -> None:
        """Tests title generation using DefectSummarizer."""
        summarizer = StubDefectSummarizer()
        tracker_stub = IssueTrackerStub()
        writer = DefectWriter(tracker_stub, summarizer)
        defect = Defect(location=CodeSnippet(file='pw_sync/mutex.cc'))
        title = writer.make_title(defect)
        self.assertEqual(
            title, 'PW-SEC-001 - pw_sync/mutex.cc: Potential buffer overflow'
        )

    async def test_process_one_dry_run(self) -> None:
        """Tests DefectWriter processing in dry-run mode."""
        summarizer = StubDefectSummarizer()
        tracker_stub = IssueTrackerStub()
        writer = DefectWriter(tracker_stub, summarizer)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
        )

        defect = Defect(
            location=CodeSnippet(file='pw_sync/mutex.cc'),
            description='Test defect',
        )
        defect_file = AsyncPath(self.working_dir, 'defect.json')
        await defect.save(defect_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(defect_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_defect = await Defect.load(forwarded_path)
        self.assertEqual(
            updated_defect.title,
            'PW-SEC-001 - pw_sync/mutex.cc: Potential buffer overflow',
        )

    async def test_process_one_creates_issue(self) -> None:
        """Tests DefectWriter filing a new issue."""
        summarizer = StubDefectSummarizer()
        tracker_stub = IssueTrackerStub()
        writer = DefectWriter(tracker_stub, summarizer)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=True,
        )

        defect = Defect(
            location=CodeSnippet(file='pw_sync/mutex.cc'),
            description='Test defect',
        )
        defect_file = AsyncPath(self.working_dir, 'defect.json')
        await defect.save(defect_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(defect_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_defect = await Defect.load(forwarded_path)
        self.assertEqual(updated_defect.issue_id, 8675309)
        self.assertEqual(
            updated_defect.title,
            'PW-SEC-001 - pw_sync/mutex.cc: Potential buffer overflow',
        )


if __name__ == '__main__':
    unittest.main()
