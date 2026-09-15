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
"""Tests for the CodeAnalyzer class in pw_fortifier."""

# pylint: disable=protected-access

import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_analyzer import CodeAnalyzerStub
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.defect import Defect
from pw_fortifier.scanner import configure_stage_for_test


class TestCodeAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for CodeAnalyzer using stubs and a temp directory."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_run_executes_scan_and_forwards_findings(self) -> None:
        """Tests that run() scans queued files and forwards reports."""
        analyzer = CodeAnalyzerStub()

        next_stage = PipelineSink()
        analyzer.connect(next_stage)

        await configure_stage_for_test(
            analyzer,
            working_dir=str(self.working_dir),
        )

        # Simulate Emitter by putting source files in the input queue
        source_file1 = self.working_dir / 'foo.cc'
        await source_file1.write_text('// foo')
        source_file2 = self.working_dir / 'bar.cc'
        await source_file2.write_text('// bar')

        await analyzer.input_queue.put(source_file1)
        await analyzer.input_queue.put(source_file2)
        await analyzer.input_queue.put(None)

        # Run the analyzer
        await analyzer.run()

        # It should have scanned both files
        self.assertEqual(len(analyzer.scanned_files), 2)
        self.assertEqual(analyzer.scanned_files[0].name, source_file1.name)
        self.assertEqual(analyzer.scanned_files[1].name, source_file2.name)

        self.assertEqual(next_stage.input_queue.qsize(), 3)

        path1 = await next_stage.input_queue.get()
        path2 = await next_stage.input_queue.get()
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)

        assert path1 is not None
        assert path2 is not None

        self.assertTrue(path1.name.endswith('.json'))
        self.assertTrue(path1.name.endswith('.json'))
        self.assertTrue(await path1.exists())
        report1 = await Defect.load(path1)
        assert report1.description is not None
        self.assertIn('# Stub Defect', report1.description)
        self.assertEqual(report1.affected_files, ['foo.cc'])

        self.assertTrue(path2.name.endswith('.json'))
        self.assertTrue(await path2.exists())
        report2 = await Defect.load(path2)
        assert report2.description is not None
        self.assertIn('# Stub Defect', report2.description)
        self.assertIn('bar.cc', report2.description)
        self.assertEqual(report2.affected_files, ['bar.cc'])

    async def test_extracts_affected_files_with_cpp_and_dots(self) -> None:
        """Tests extracting affected files with .cpp and dots."""

        analyzer = CodeAnalyzerStub()
        next_stage = PipelineSink()
        analyzer.connect(next_stage)

        await configure_stage_for_test(
            analyzer,
            working_dir=str(self.working_dir),
        )

        source_file = self.working_dir / 'my.pkg' / 'test.cpp'
        await source_file.parent.mkdir(parents=True, exist_ok=True)
        await source_file.write_text('// test')

        await analyzer.input_queue.put(source_file)
        await analyzer.input_queue.put(None)

        await analyzer.run()

        path = await next_stage.input_queue.get()
        assert path is not None
        report = await Defect.load(path)
        self.assertEqual(report.affected_files, ['test.cpp'])


if __name__ == '__main__':
    unittest.main()
