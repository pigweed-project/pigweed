# Copyright 2021 The Pigweed Authors
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
"""Tests for pw_console.log_view"""

import logging
import re
import unittest
from parameterized import parameterized  # type: ignore

from prompt_toolkit.document import Document
from prompt_toolkit.validation import ValidationError

from pw_console.log_line import LogLine
from pw_console.log_filter import (
    LogFilter,
    RegexValidator,
    SearchMatcher,
    preprocess_search_regex,
)


class TestLogFilter(unittest.TestCase):
    """Tests for LogFilter."""

    maxDiff = None

    # pylint: disable=anomalous-backslash-in-string
    @parameterized.expand(
        [
            (
                'raw string',
                SearchMatcher.STRING,
                'f(x)',
                r'f\(x\)',
                re.IGNORECASE,
            ),
            (
                'simple regex',
                SearchMatcher.REGEX,
                'f(x)',
                'f(x)',
                re.IGNORECASE,
            ),
            (
                'regex with case sensitivity',
                SearchMatcher.REGEX,
                'f(X)',
                'f(X)',
                re.RegexFlag(0),
            ),
            (
                'regex with error',
                SearchMatcher.REGEX,
                'f of (x',  # Un-terminated open paren
                'f of (x',
                re.IGNORECASE,
                True,  # fails_validation
            ),
            (
                'simple fuzzy',
                SearchMatcher.FUZZY,
                'f x y',
                '(f)(.*?)(x)(.*?)(y)',
                re.IGNORECASE,
            ),
            (
                'fuzzy with case sensitivity',
                SearchMatcher.FUZZY,
                'f X y',
                '(f)(.*?)(X)(.*?)(y)',
                re.RegexFlag(0),
            ),
        ]
    )
    def test_preprocess_search_regex(
        self,
        _name,
        input_matcher,
        input_text,
        expected_regex,
        expected_re_flag,
        should_fail_validation=False,
    ) -> None:
        """Test preprocess_search_regex returns the expected regex settings."""
        result_text, re_flag = preprocess_search_regex(
            input_text, input_matcher
        )
        self.assertEqual(expected_regex, result_text)
        self.assertEqual(expected_re_flag, re_flag)

        if should_fail_validation:
            document = Document(text=input_text)
            with self.assertRaisesRegex(
                ValidationError, r'Regex Error.*at position [0-9]+'
            ):
                RegexValidator().validate(document)

    def _create_logs(self, log_messages):
        test_logger = logging.getLogger('log_filter.test')
        with self.assertLogs(test_logger, level='DEBUG') as log_context:
            for log, extra_arg in log_messages:
                test_logger.debug('%s', log, extra=extra_arg)

        return log_context

    @parameterized.expand(
        [
            (
                'simple fuzzy',
                SearchMatcher.FUZZY,
                'log item',  # input_text
                [  # input_lines
                    ('1 Log some item', {'planet': 'Jupiter'}),
                    ('1 Log another item', {'planet': 'Earth'}),
                    ('1 Some exception', {'planet': 'Earth'}),
                ],
                [  # expected_matched_lines
                    '1 Log some item',
                    '1 Log another item',
                ],
                None,  # field
                False,  # invert
            ),
            (
                'simple fuzzy inverted',
                SearchMatcher.FUZZY,
                'log item',  # input_text
                [  # input_lines
                    ('2 Log some item', dict()),
                    ('2 Log another item', dict()),
                    ('2 Some exception', dict()),
                ],
                [  # expected_matched_lines
                    '2 Some exception',
                ],
                None,  # field
                True,  # invert
            ),
            (
                'regex with field',
                SearchMatcher.REGEX,
                # lowercase input text should use re.IGNORECASE
                'earth',  # input_text
                [  # input_lines
                    (
                        '3 Log some item',
                        dict(extra_metadata_fields={'planet': 'Jupiter'}),
                    ),
                    (
                        '3 Log another item',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                    (
                        '3 Some exception',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                    (
                        '3 A line with earth in the message',
                        # Set planet to None, this log should not match.
                        dict(extra_metadata_fields={'planet': None}),
                    ),
                ],
                [  # expected_matched_lines
                    '3 Log another item',
                    '3 Some exception',
                ],
                'planet',  # field
                False,  # invert
            ),
            (
                'regex with field inverted',
                SearchMatcher.REGEX,
                'earth',  # input_text
                [  # input_lines
                    (
                        '4 Log some item',
                        dict(extra_metadata_fields={'planet': 'Jupiter'}),
                    ),
                    (
                        '4 Log another item',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                    (
                        '4 Some exception',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                ],
                [  # expected_matched_lines
                    '4 Log some item',
                ],
                'planet',  # field
                True,  # invert
            ),
            (
                'regex with field and no matches',
                SearchMatcher.REGEX,
                'mars',  # input_text
                [  # input_lines
                    (
                        '5 Log another item',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                    (
                        '5 Some exception',
                        dict(extra_metadata_fields={'planet': 'Earth'}),
                    ),
                    (
                        '5 Yet another exception for mars',
                        dict(extra_metadata_fields={'planet': None}),
                    ),
                ],
                # expected_matched_lines
                [],
                'planet',  # field
                False,  # invert
            ),
            (
                'inverted regex with field with Nones',
                SearchMatcher.REGEX,
                'earth',  # input_text
                [  # input_lines
                    (
                        '6 log1 mercury',
                        dict(extra_metadata_fields={'planet': 'mercury'}),
                    ),
                    (
                        '6 log2 earth',
                        dict(extra_metadata_fields={'planet': 'earth'}),
                    ),
                    (
                        '6 log3 mars',
                        dict(extra_metadata_fields={'planet': 'mars'}),
                    ),
                    (
                        '6 log4 None',
                        dict(extra_metadata_fields={'planet': None}),
                    ),
                ],
                # expected_matched_lines
                [
                    '6 log1 mercury',
                    '6 log3 mars',
                    '6 log4 None',
                ],
                'planet',  # field
                True,  # invert
            ),
        ]
    )
    def test_log_filter_matches(
        self,
        _name,
        input_matcher,
        input_text,
        input_lines,
        expected_matched_lines,
        field=None,
        invert=False,
    ) -> None:
        """Test log filter matches expected lines."""
        result_text, re_flag = preprocess_search_regex(
            input_text, input_matcher
        )
        log_filter = LogFilter(
            regex=re.compile(result_text, re_flag),
            input_text=input_text,
            invert=invert,
            field=field,
        )

        matched_lines = []
        logs = self._create_logs(input_lines)

        for record in logs.records:
            log_line = LogLine(
                record=record,
                formatted_log=record.message,
                ansi_stripped_log=record.message,
            )
            # Populate the extra metadata
            log_line.update_metadata()
            if log_filter.matches(log_line):
                matched_lines.append(record.message)

        self.assertEqual(expected_matched_lines, matched_lines)


if __name__ == '__main__':
    unittest.main()
