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
"""Runs the local presubmit checks for the Pigweed repository."""

import os
import sys

import pw_presubmit.v2
from pw_presubmit.private.upstream_programs import EXCLUDES, programs


def main() -> int:
    """Run the presubmit for the Pigweed repository."""
    # Change to working directory if running from Bazel.
    if 'BUILD_WORKING_DIRECTORY' in os.environ:
        os.chdir(os.environ['BUILD_WORKING_DIRECTORY'])

    return pw_presubmit.v2.main(programs(), 'quick', exclude=EXCLUDES)


if __name__ == '__main__':
    sys.exit(main())
