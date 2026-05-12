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
"""Tests for pw_console.log_file_loader."""

from pathlib import Path
import unittest
from unittest.mock import MagicMock, patch, mock_open

from prompt_toolkit.application import create_app_session

# inclusive-language: ignore
from prompt_toolkit.output import DummyOutput as FakeOutput

from pw_console.log_file_loader import LogFileLoader


class TestLogFileLoader(unittest.TestCase):
    """Tests for LogFileLoader."""

    def setUp(self):
        self.mock_parser = MagicMock()
        self.patcher_get_parser = patch(
            'pw_console.log_file_loader.get_command_log_parser',
            return_value=self.mock_parser,
        )
        self.mock_get_parser = self.patcher_get_parser.start()

        # Mock ProgressBar
        self.mock_pb_instance = MagicMock()
        self.mock_pb_instance.__enter__.return_value = self.mock_pb_instance
        self.mock_pb_instance.__exit__.return_value = None

        self.mock_counter = MagicMock()
        self.mock_pb_instance.return_value = self.mock_counter

        self.patcher_pb = patch(
            'pw_console.log_file_loader.ProgressBar',
            return_value=self.mock_pb_instance,
        )
        self.mock_pb = self.patcher_pb.start()

    def tearDown(self):
        self.patcher_get_parser.stop()
        self.patcher_pb.stop()

    def test_initialization(self):
        """Test successful initialization."""
        loader = LogFileLoader(
            files=[Path('test.log')],
            log_parser='basic',
        )
        self.assertEqual(loader.files, [Path('test.log')])
        self.assertEqual(loader.log_parser_name, 'basic')
        self.assertEqual(loader.log_parser, self.mock_parser)

    def test_unknown_parser(self):
        """Test initialization with unknown parser raises ValueError."""
        self.mock_get_parser.return_value = None
        with self.assertRaises(ValueError):
            LogFileLoader(files=[Path('test.log')], log_parser='unknown')

    def test_load_file(self):
        """Test loading a plain text file."""
        loader = LogFileLoader(
            files=[Path('test.log')],
            log_parser='basic',
        )

        mock_file_content = b'line1\nline2\n'

        with patch('pw_console.log_file_loader.Path.stat') as mock_stat:
            mock_stat.return_value.st_size = 1000

            m_open = mock_open(read_data=mock_file_content)
            with patch('pw_console.log_file_loader.Path.open', m_open):
                with create_app_session(output=FakeOutput()):
                    loader.load_files()

                m_open.assert_called_once_with('rb')

        self.assertEqual(self.mock_parser.call_count, 2)
        expected_logger = loader.get_log_store(Path('test.log')).logger
        self.mock_parser.assert_any_call(expected_logger, 'line1\n')
        self.mock_parser.assert_any_call(expected_logger, 'line2\n')

    def test_load_zip_file(self):
        """Test loading a zip file."""
        loader = LogFileLoader(
            files=[Path('test.zip')],
            log_parser='basic',
        )

        mock_zip = MagicMock()
        mock_zip.__enter__.return_value = mock_zip
        mock_zip.namelist.return_value = ['file1.log', 'file2.txt']

        mock_info1 = MagicMock()
        mock_info1.file_size = 500
        mock_info2 = MagicMock()
        mock_info2.file_size = 500
        mock_zip.getinfo.side_effect = lambda name: (
            mock_info1 if name == 'file1.log' else mock_info2
        )

        m_open1 = mock_open(read_data=b'zipline1\nzipline2\n')
        m_open2 = mock_open(read_data=b'zipline3\n')

        mock_zip.open.side_effect = lambda name, mode: (
            m_open1() if name == 'file1.log' else m_open2()
        )

        with patch('zipfile.ZipFile', return_value=mock_zip):
            with create_app_session(output=FakeOutput()):
                loader.load_files()

        self.assertEqual(self.mock_parser.call_count, 3)

    def test_merge_files(self):
        """Test that logs are merged when merge_files is True."""
        loader = LogFileLoader(
            files=[Path('file1.log'), Path('file2.log')],
            log_parser='basic',
            merge_files=True,
        )

        fls1 = loader.get_log_store(Path('file1.log'))
        fls2 = loader.get_log_store(Path('file2.log'))

        self.assertEqual(fls1.logger, fls2.logger)
        self.assertEqual(fls1.logger.name, 'pw_console.open_files')


if __name__ == '__main__':
    unittest.main()
