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
"""Tools for running presubmit checks in a Git repository.

This is the public v2 API, which supports features like automatic fixes and
running individually on stacked commits.
"""

from pw_presubmit.private.result import Failure, PresubmitFailure
from pw_presubmit.private.step import Context, Step, step
from pw_presubmit.private.v2_cli import main

__all__ = [
    'Context',
    'Failure',
    'main',
    'Step',
    'step',
    'PresubmitFailure',
]
