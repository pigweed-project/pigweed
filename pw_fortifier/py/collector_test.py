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
"""Tests for the generic Collector class."""

import asyncio
import io
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.collector import Collector
from pw_fortifier.scanner import configure_stage_for_test


class CollectorStub(Collector):
    """A stub collector for testing generic behavior."""

    def __init__(self) -> None:
        super().__init__()
        self._add_field('Col1', 5)
        self._add_field('Col2', -1)
        self._add_field('Col3', 10, hidden=True)

    async def _print_item(self, path: AsyncPath) -> None:
        content = await path.read_text()
        parts = content.split(',')
        col1 = parts[0] if len(parts) > 0 else ''
        col2 = parts[1] if len(parts) > 1 else ''
        col3 = parts[2] if len(parts) > 2 else ''
        self._report_fields(col1, col2, col3)


class TestCollector(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the generic Collector class."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_collector_formatting(self) -> None:
        """Tests that Collector correctly formats and prints generic items."""
        collector = CollectorStub()
        await configure_stage_for_test(
            collector,
            working_dir=str(self.working_dir),
        )

        # Create some files for testing
        file1 = self.working_dir / 'item1.txt'
        await file1.write_text('val1_long,val2_long_text,val3_hidden')

        file2 = self.working_dir / 'item2.txt'
        await file2.write_text('v1,v2,v3_hid')

        captured_stdout = io.StringIO()
        with patch('sys.stdout', new=captured_stdout):
            run_task = asyncio.create_task(collector.run())

            await collector.input_queue.put(file1)
            await collector.input_queue.put(file2)
            await collector.input_queue.put(None)
            await run_task

        output = captured_stdout.getvalue()

        # Expected header and separator (Col3 is hidden, so same as before)
        expected_header = '| Col1  | Col2' + ' ' * 105 + '|'
        expected_separator = '+-------+' + '-' * 110 + '+'

        self.assertIn(expected_header, output)
        self.assertIn(expected_separator, output)

        # Verify rows (Col1 is truncated to 'va...', Col3 is hidden)
        expected_row1 = '| va... | val2_long_text' + ' ' * 95 + '|'
        expected_row2 = '| v1    | v2' + ' ' * 107 + '|'

        self.assertIn(expected_row1, output)
        self.assertIn(expected_row2, output)

        # Verify summary
        self.assertIn('DONE! Processed 2 reports.', output)

    async def test_collector_csv_output(self) -> None:
        """Tests that Collector writes results to a CSV file."""
        csv_output_path = self.working_dir / 'output.csv'
        collector = CollectorStub()
        await configure_stage_for_test(
            collector,
            working_dir=str(self.working_dir),
            output=str(csv_output_path),
        )

        # Create some files for testing
        file1 = self.working_dir / 'item1.txt'
        await file1.write_text('val1,val2_long_text,val3_h')

        file2 = self.working_dir / 'item2.txt'
        await file2.write_text('v1,v2,v3_h')

        # Run the collector
        captured_stdout = io.StringIO()
        with patch('sys.stdout', new=captured_stdout):
            run_task = asyncio.create_task(collector.run())

            await collector.input_queue.put(file1)
            await collector.input_queue.put(file2)
            await collector.input_queue.put(None)
            await run_task

        # Verify CSV content (includes hidden Col3)
        self.assertTrue(await csv_output_path.is_file())
        csv_content = await csv_output_path.read_text()

        lines = csv_content.splitlines()
        self.assertEqual(len(lines), 3)
        self.assertEqual(lines[0], 'Col1,Col2,Col3')
        self.assertEqual(lines[1], 'val1,val2_long_text,val3_h')
        self.assertEqual(lines[2], 'v1,v2,v3_h')

    async def test_collector_left_truncation(self) -> None:
        """Tests that Collector correctly truncates fields from the left."""

        class LeftTruncatingCollectorStub(Collector):
            def __init__(self) -> None:
                super().__init__()
                self._add_field('Col1', 6, truncate_from_left=True)
                self._add_field('Col2', -1)

            async def _print_item(self, path: AsyncPath) -> None:
                content = await path.read_text()
                parts = content.split(',')
                col1 = parts[0] if len(parts) > 0 else ''
                col2 = parts[1] if len(parts) > 1 else ''
                self._report_fields(col1, col2)

        collector = LeftTruncatingCollectorStub()
        await configure_stage_for_test(
            collector,
            working_dir=str(self.working_dir),
        )

        file1 = self.working_dir / 'item_left.txt'
        await file1.write_text('some-long-val,rest_of_text')

        captured_stdout = io.StringIO()
        with patch('sys.stdout', new=captured_stdout):
            run_task = asyncio.create_task(collector.run())
            await collector.input_queue.put(file1)
            await collector.input_queue.put(None)
            await run_task

        output = captured_stdout.getvalue()
        # 'some-long-val' (length 13) with width 6 and truncate_from_left=True
        # should be '...' + 'val' = '...val'
        self.assertIn('| ...val | rest_of_text', output)


if __name__ == '__main__':
    unittest.main()
