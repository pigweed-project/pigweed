# Copyright 2024 The Pigweed Authors
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
"""Checks for phrases intended to block submission."""

import re

from pw_presubmit.v2 import step, Context
from pw_presubmit import inclusive_language

WORDS_REGEX = re.compile(
    r"do[\s_-]*not[\s_-]*submit|don'?t[\s_-]*submit", re.IGNORECASE
)
DISABLE_TAG = 'block-submission: disable'
ENABLE_TAG = 'block-submission: enable'
IGNORE_TAG = 'block-submission: ignore'
ISSUE_TYPE = 'submission-blocking phrase'


@step(name='block_submission')
def presubmit_check(ctx: Context) -> None:
    inclusive_language.generic_presubmit_check(
        ctx,
        words_regex=WORDS_REGEX,
        disable_tag=DISABLE_TAG,
        enable_tag=ENABLE_TAG,
        ignore_tag=IGNORE_TAG,
        issue_type=ISSUE_TYPE,
    )
