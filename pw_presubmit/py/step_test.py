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
"""Tests for the Step class."""

from typing import Any
import unittest
from pathlib import Path
from pw_cli.file_filter import FileFilter
from pw_presubmit.private.step import (
    Step,
    Context,
    step as step_decorator,
    FilteredStep,
)
from pw_presubmit.private.check import Check
from pw_presubmit.private.result import PresubmitResult


class MockStep(Step):
    """Mock step for testing."""

    def __init__(self, file_filter=FileFilter()):
        super().__init__(file_filter=file_filter)
        self.run_called = False
        self.fix_called = False
        self.should_fail = False
        self.should_fix = False

    def run(self, ctx: Context) -> None:
        self.run_called = True
        if self.should_fail:
            ctx.fail("Failed")

    def fix(self, ctx: Context) -> None:
        self.fix_called = True
        if not self.should_fix:
            ctx.fail("Failed to fix")


class TestStep(unittest.TestCase):
    """Tests for Step class."""

    def test_default_filter(self):
        step = MockStep()
        self.assertEqual(step.filter.endswith, frozenset())
        self.assertEqual(step.filter.exclude, frozenset())

    def test_custom_filter(self):
        filt = FileFilter(endswith='.cc')
        step = MockStep(file_filter=filt)
        self.assertEqual(step.filter.endswith, filt.endswith)

    def test_replace_filter(self):
        step = MockStep(file_filter=FileFilter(endswith=('.cc',)))
        new_step = step.replace_filter(
            file_filter=FileFilter(endswith=('.py',))
        )
        self.assertEqual(
            tuple(new_step.filter.filter([Path('a.py')])), (Path('a.py'),)
        )
        self.assertEqual(tuple(new_step.filter.filter([Path('a.cc')])), ())

    def test_concat_filter(self):
        step = MockStep(file_filter=FileFilter(endswith=('.cc',)))
        new_step = step.concat_filter(file_filter=FileFilter(endswith=('.h',)))
        self.assertEqual(
            tuple(new_step.filter.filter([Path('a.cc')])), (Path('a.cc'),)
        )
        self.assertEqual(
            tuple(new_step.filter.filter([Path('a.h')])), (Path('a.h'),)
        )
        self.assertEqual(tuple(new_step.filter.filter([Path('a.py')])), ())

    def test_run(self):
        step = MockStep()
        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        step.run(ctx)
        self.assertTrue(step.run_called)
        self.assertFalse(ctx.failed)

    def test_run_failed(self):
        step = MockStep()
        step.should_fail = True
        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        step.run(ctx)
        self.assertTrue(step.run_called)
        self.assertTrue(ctx.failed)

    def test_fix_default(self):
        step = MockStep()
        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        step.fix(ctx)
        self.assertTrue(ctx.failed)
        self.assertTrue(step.fix_called)

    def test_fix_custom(self):
        step = MockStep()
        step.should_fix = True
        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        step.fix(ctx)
        self.assertFalse(ctx.failed)
        self.assertTrue(step.fix_called)

    def test_wrap_in_check(self):
        """Test wrapping a Step in a Check."""
        step = MockStep()

        def _adapter(ctx: Any) -> PresubmitResult:
            step.run(ctx)
            if ctx.failed:
                return PresubmitResult.FAIL
            return PresubmitResult.PASS

        check = Check(_adapter, path_filter=step.filter)

        class MockCtx:
            def __init__(self):
                self.failed = False
                self._failures = []
                self.paths = (Path('file.cc'),)

            def fail(self, desc):
                self.failed = True
                self._failures.append(desc)

        ctx = MockCtx()
        result = check.run(ctx)
        self.assertTrue(step.run_called)
        self.assertEqual(result, PresubmitResult.PASS)

        step.should_fail = True
        step.run_called = False
        result = check.run(ctx)
        self.assertTrue(step.run_called)
        self.assertEqual(result, PresubmitResult.FAIL)

    def test_filter(self):
        step = MockStep(file_filter=FileFilter(endswith=('.cc',)))
        paths = (Path('a.cc'), Path('b.py'))

        filtered_paths = tuple(step.filter.filter(paths))
        filtered_step = FilteredStep(step, filtered_paths)
        self.assertEqual(filtered_step.paths, (Path('a.cc'),))
        self.assertEqual(filtered_step.name, 'mock_step')

    def test_filter_none(self):
        step = MockStep(file_filter=FileFilter(endswith=('.cc',)))
        paths = (Path('b.py'),)

        filtered_paths = tuple(step.filter.filter(paths))
        self.assertEqual(filtered_paths, ())

    def test_default_filter_matches_all(self):
        step = MockStep()
        paths = (Path('a.cc'), Path('b.py'))

        filtered_paths = tuple(step.filter.filter(paths))
        self.assertEqual(filtered_paths, paths)


class TestStepDecorator(unittest.TestCase):
    """Tests for step decorator."""

    def test_basic_decorator(self):
        @step_decorator()
        def my_step(_ctx: Context) -> None:
            pass

        self.assertIsInstance(my_step, Step)
        self.assertEqual(my_step.name, 'my_step')

    def test_decorator_with_filters(self):
        @step_decorator(endswith=('.cc',))
        def my_step(_ctx: Context) -> None:
            pass

        self.assertEqual(
            tuple(my_step.filter.filter([Path('a.cc')])), (Path('a.cc'),)
        )
        self.assertEqual(tuple(my_step.filter.filter([Path('a.py')])), ())

    def test_decorator_with_fix(self):
        fix_called = False

        def my_fix(_ctx: Context) -> None:
            nonlocal fix_called
            fix_called = True

        @step_decorator(fix=my_fix)
        def my_step(_ctx: Context) -> None:
            pass

        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        self.assertIsNone(my_step.fix(ctx))
        self.assertTrue(fix_called)

    def test_decorator_no_fix_inherits_base(self):
        @step_decorator()
        def my_step(_ctx: Context) -> None:
            pass

        # Verify that 'fix' is not in the derived class dict, meaning it's
        # inherited.
        self.assertNotIn('fix', my_step.__class__.__dict__)

        # Verify it raises NotImplementedError
        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        with self.assertRaises(NotImplementedError):
            my_step.fix(ctx)

    def test_decorator_with_fix_overrides(self):
        def my_fix(_ctx: Context) -> None:
            pass

        @step_decorator(fix=my_fix)
        def my_step(_ctx: Context) -> None:
            pass

        # Verify that 'fix' IS in the derived class dict.
        self.assertIn('fix', my_step.__class__.__dict__)

        ctx = Context(
            root=Path('.'),
            output_dir=Path('.'),
            paths=(),
            all_paths=(),
            all_modified_paths=(),
        )
        self.assertIsNone(my_step.fix(ctx))


if __name__ == '__main__':
    unittest.main()
