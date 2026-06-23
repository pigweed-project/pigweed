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
"""Examples for the pw_presubmit v2 API."""

# pylint: disable=unused-import,reimported,ungrouped-imports
# pylint: disable=wrong-import-position,wrong-import-order

import tempfile
from pathlib import Path
import unittest

# DOCSTAG: [pw_presubmit-v2-decorator]
import pw_presubmit.v2


@pw_presubmit.v2.step(endswith='.py')
def check_python_files(ctx: pw_presubmit.v2.Context):
    """A simple step that checks Python files."""
    for _ in ctx.paths:
        # Check file
        pass


# DOCSTAG: [pw_presubmit-v2-decorator]

# DOCSTAG: [pw_presubmit-v2-class]
import pw_presubmit.v2
from pw_presubmit.v2 import Step


class MyStep(Step):
    """A step that supports automatic fixing."""

    def __init__(self):
        super().__init__(endswith=('.cc', '.h'))

    def run(self, ctx: pw_presubmit.v2.Context) -> None:
        """Runs the check."""
        for path in ctx.paths:
            # Check file
            if 'bad_word' in path.read_text():
                # Signal failure by calling ctx.fail()
                ctx.fail('Found bad word', path=path)

    def fix(self, ctx: pw_presubmit.v2.Context) -> None:
        """Applies the fix."""
        for path in ctx.paths:
            # Attempt to fix the file
            success = False  # Hypothetical result
            if not success:
                # Signal failure by raising PresubmitFailure
                raise pw_presubmit.v2.PresubmitFailure(f'Failed to fix {path}')


# DOCSTAG: [pw_presubmit-v2-class]

# DOCSTAG: [pw_presubmit-v2-programs]
import sys
import pw_presubmit.v2

EXCLUDE = (
    'third_party/',
    r'some_file\.cc',
)

PROGRAMS = {
    'quick': [check_python_files],
    'full': [check_python_files, MyStep()],
}


def main() -> int:
    """Main entry point."""
    return pw_presubmit.v2.main(PROGRAMS, 'quick', exclude=EXCLUDE)


# DOCSTAG: [pw_presubmit-v2-programs]


class TestExamples(unittest.TestCase):
    def test_load(self):
        """Verify that the examples can be loaded."""
        self.assertIsNotNone(PROGRAMS)

    def test_run_steps(self):
        """Instantiate and run steps with a fake context."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            test_file = tmp_path / 'file.py'
            test_file.write_text('nothing bad')
            ctx = pw_presubmit.v2.Context(
                root=tmp_path,
                output_dir=tmp_path / 'output',
                paths=(test_file,),
                all_paths=(test_file,),
                all_modified_paths=(test_file,),
            )
            ctx.output_dir.mkdir()

            # Run decorator step
            check_python_files.run(ctx)

            # Run class step
            step_instance = MyStep()
            step_instance.run(ctx)
            with self.assertRaises(pw_presubmit.v2.PresubmitFailure):
                step_instance.fix(ctx)


if __name__ == '__main__':
    unittest.main()
