# Copyright 2023 The Pigweed Authors
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
"""JSON validity check."""

import json

from pw_presubmit.v2 import step, Context


@step(name='json_check', endswith=('.json',))
def presubmit_check(ctx: Context):
    """Presubmit check that ensures JSON files are valid."""

    for path in ctx.paths:
        with path.open('r') as ins:
            try:
                json.load(ins)
            except json.decoder.JSONDecodeError as exc:
                intro_line = f'failed to parse {path.relative_to(ctx.root)}'

                ctx.fail(intro_line)
                ctx.fail(str(exc))
