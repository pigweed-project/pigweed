# Copyright 2025 The Pigweed Authors
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
"""Tests for the pw_build.project_builder module."""

from graphlib import CycleError
import logging
from pathlib import Path
import re
import unittest
from unittest.mock import patch, MagicMock

from parameterized import parameterized  # type: ignore

from pw_build.build_recipe import BuildCommand, BuildRecipe
from pw_build.project_builder import (
    ProjectBuilder,
    check_ansi_codes,
    execute_command_with_logging,
)


class TestCheckAnsiCodes(unittest.TestCase):
    """Tests for project_builder.check_ansi_codes."""

    def test_empty(self):
        """Tests setting and getting the GN target name and type."""
        result = check_ansi_codes('')
        self.assertEqual(result, None)

    def test_code_reset(self):
        result = check_ansi_codes('\x1b[11mHello\x1b[0m')
        self.assertEqual(result, None)

    def test_code_not_reset(self):
        result = check_ansi_codes('\x1b[11mHello')
        self.assertEqual(result, ['\x1b[11m'])

    def test_code_code_no_reset(self):
        result = check_ansi_codes('\x1b[30;47mHello\x1b[1;31mWorld')
        self.assertEqual(result, ['\x1b[30;47m', '\x1b[1;31m'])

    def test_code_code_reset(self):
        result = check_ansi_codes('\x1b[30;47mHello\x1b[1;31mWorld\x1b[0m')
        self.assertEqual(result, None)

    def test_reset_only(self):
        result = check_ansi_codes('\x1b[30;47mHello World\x1b[9999m\x1b[m')
        self.assertEqual(result, None)
        result = check_ansi_codes('Hello World\x1b[0m')
        self.assertEqual(result, None)
        result = check_ansi_codes('Hello World\x1b[m')
        self.assertEqual(result, None)

    def test_code_reset_code(self):
        result = check_ansi_codes('\x1b[30;47mHello\x1b[0m\x1b[1;31mWorld')
        self.assertEqual(result, ['\x1b[1;31m'])

    def test_code_reset_code_reset(self):
        result = check_ansi_codes(
            '\x1b[30;47mHello\x1b[0m\x1b[1;31mWorld\x1b[0m'
        )
        self.assertEqual(result, None)


_TEST_RECIPE_SET_GN = [
    BuildRecipe(
        build_dir=Path('out/gn'),
        title='default_gn',
        steps=[
            BuildCommand(
                command=[
                    'gn',
                    'gen',
                    '{build_dir}',
                    '--export-compile-commands',
                ],
            ),
            BuildCommand(
                build_system_command='ninja',
                targets=['default'],
            ),
        ],
    ),
]

_TEST_RECIPE_SET_BAZEL = [
    BuildRecipe(
        build_dir=Path('out/bazel'),
        title='bazel_compile_commands',
        steps=[
            BuildCommand(
                build_system_command='bazel',
                build_system_extra_args=[
                    'run',
                    '--verbose_failures',
                    '--worker_verbose',
                ],
                targets=['//:refresh_compile_commands'],
            ),
        ],
    ),
    BuildRecipe(
        build_dir=Path('out/bazel'),
        title='bazel_build',
        steps=[
            BuildCommand(
                build_system_command='bazel',
                build_system_extra_args=[
                    'build',
                    '--verbose_failures',
                    '--worker_verbose',
                ],
                targets=['//...'],
            ),
        ],
        dependencies=['bazel_compile_commands'],
    ),
    BuildRecipe(
        build_dir=Path('out/bazel'),
        title='bazel_test',
        steps=[
            BuildCommand(
                build_system_command='bazel',
                build_system_extra_args=[
                    'test',
                    '--test_output=errors',
                ],
                targets=['//...'],
            ),
        ],
        dependencies=['bazel_build'],
    ),
]

_TEST_RECIPE_SET_CMAKE = [
    BuildRecipe(
        build_dir=Path('out/cmake'),
        title='default_cmake',
        steps=[
            BuildCommand(
                command=[
                    'cmake',
                    '--fresh',
                    '-S',
                    'repo_root',
                    '-B',
                    '{build_dir}',
                    '-G',
                    'Ninja',
                ],
            ),
            BuildCommand(
                build_system_command='ninja',
                targets=['default'],
            ),
        ],
    ),
]

_TEST_RECIPE_SET_WITH_CYCLE = [
    BuildRecipe(
        build_dir=Path('out/makefile'),
        title='makefile1',
        steps=[
            BuildCommand(build_system_command='make'),
        ],
        dependencies=['cmake_gen_makefile'],
    ),
    BuildRecipe(
        build_dir=Path('out/makefile'),
        title='cmake_gen_makefile',
        steps=[
            BuildCommand(build_system_command='make'),
        ],
        dependencies=['makefile1'],
    ),
]

_COMMAND_PWD = ['pwd']
_COMMAND_DATE = ['date']


def _create_mock_build_command_process(return_code=0):
    mock_process = MagicMock()
    mock_process.returncode = return_code
    mock_enter = MagicMock()
    mock_enter.poll.return_value = return_code
    mock_enter.stdout = []
    mock_enter.stderr = []
    mock_enter.returncode = return_code
    mock_process.__enter__.return_value = mock_enter
    return mock_process


class TestProjectBuilder(unittest.TestCase):
    """Tests for ProjectBuilder."""

    maxDiff = None

    def test_build_recipe_dep_cycles(self) -> None:
        """Test BuildRecipe dependency cycles."""
        mock_abort = MagicMock()
        pb = ProjectBuilder(
            build_recipes=_TEST_RECIPE_SET_WITH_CYCLE,
            source_path=Path.cwd(),  # Needed for running under bazel test
            abort_callback=mock_abort,
        )
        mock_abort.assert_called_once_with(
            'Build recipe dependency cycle found: '
            'makefile1 -> cmake_gen_makefile -> makefile1'
        )
        # Printing the graph should raise a CycleError
        with self.assertRaises(CycleError):
            pb.recipe_graph()

    @parameterized.expand(
        [
            (
                # test title
                'recipe graph order 1',
                # recipes
                (
                    _TEST_RECIPE_SET_GN
                    + _TEST_RECIPE_SET_BAZEL
                    + _TEST_RECIPE_SET_CMAKE
                ),
                # expected graph output text
                [
                    '├── default_gn',
                    '├── bazel_compile_commands',
                    '│   └── bazel_build',
                    '│       └── bazel_test',
                    '└── default_cmake',
                ],
                # expected_static_order
                [
                    'default_gn',
                    'bazel_compile_commands',
                    'default_cmake',
                    'bazel_build',
                    'bazel_test',
                ],
            ),
            (
                # test title
                'recipe graph order 2',
                # recipes
                _TEST_RECIPE_SET_BAZEL
                + _TEST_RECIPE_SET_GN
                + _TEST_RECIPE_SET_CMAKE,
                # expected graph output text
                [
                    '├── bazel_compile_commands',
                    '│   └── bazel_build',
                    '│       └── bazel_test',
                    '├── default_gn',
                    '└── default_cmake',
                ],
                # expected_static_order
                [
                    'bazel_compile_commands',
                    'default_gn',
                    'default_cmake',
                    'bazel_build',
                    'bazel_test',
                ],
            ),
            (
                # test title
                'recipe graph multiple dependencies 1',
                # recipes
                (
                    _TEST_RECIPE_SET_GN
                    + _TEST_RECIPE_SET_BAZEL
                    + _TEST_RECIPE_SET_CMAKE
                    + [
                        BuildRecipe(
                            build_dir=Path('out/premake1'),
                            title='premake1',
                            steps=[
                                BuildCommand(build_system_command='make'),
                            ],
                            dependencies=[],
                        ),
                        BuildRecipe(
                            build_dir=Path('out/premake2'),
                            title='premake2',
                            steps=[
                                BuildCommand(build_system_command='make'),
                            ],
                            dependencies=[],
                        ),
                        BuildRecipe(
                            build_dir=Path('out/actualbuild'),
                            title='actualbuild',
                            steps=[
                                BuildCommand(build_system_command='make'),
                            ],
                            dependencies=['premake1', 'premake2'],
                        ),
                    ]
                ),
                # expected graph output text
                [
                    '├── default_gn',
                    '├── bazel_compile_commands',
                    '│   └── bazel_build',
                    '│       └── bazel_test',
                    '├── default_cmake',
                    '├── premake1',
                    '│   └── actualbuild',
                    '└── premake2',
                    '    └── actualbuild',
                ],
                # expected_static_order
                [
                    'default_gn',
                    'bazel_compile_commands',
                    'default_cmake',
                    'premake1',
                    'premake2',
                    'bazel_build',
                    'actualbuild',
                    'bazel_test',
                ],
            ),
        ]
    )
    def test_recipe_graph(
        self,
        _test_name,
        recipes,
        expected_output,
        expected_static_order,
    ) -> None:
        """Test BuildRecipe dependency cycles."""
        pb = ProjectBuilder(
            build_recipes=recipes,
            source_path=Path.cwd(),  # Needed for running under bazel test
        )
        self.assertEqual(expected_output, pb.recipe_graph())

        if expected_static_order:
            self.assertEqual(
                # pylint: disable=protected-access
                expected_static_order,
                pb._get_recipe_execution_order(),
                # pylint: enable=protected-access
            )

    # pylint: disable=no-self-use
    def test_check_unique_recipe_names(self) -> None:
        """Test _check_unique_recipe_names with duplicate names."""
        recipe1 = BuildRecipe(
            build_dir=Path('out1'),
            title='recipe1',
            steps=[BuildCommand(build_system_command='ninja')],
        )
        recipe2 = BuildRecipe(
            build_dir=Path('out2'),
            title='recipe1',
            steps=[BuildCommand(build_system_command='ninja')],
        )

        mock_exit = MagicMock()
        ProjectBuilder(
            build_recipes=[recipe1, recipe2],
            source_path=Path.cwd(),  # Needed for running under bazel test
            abort_callback=mock_exit,
        )
        mock_exit.assert_called_once_with(
            'Duplicate build recipe names found: recipe1'
        )

    def test_check_unique_recipe_names_no_duplicates(self) -> None:
        """Test _check_unique_recipe_names with no duplicate names."""
        recipe1 = BuildRecipe(
            build_dir=Path('out1'),
            title='recipe1',
            steps=[BuildCommand(build_system_command='ninja')],
        )
        recipe2 = BuildRecipe(
            build_dir=Path('out2'),
            title='recipe2',
            steps=[BuildCommand(build_system_command='ninja')],
        )

        mock_exit = MagicMock()
        ProjectBuilder(
            build_recipes=[recipe1, recipe2],
            source_path=Path.cwd(),  # Needed for running under bazel test
            abort_callback=mock_exit,
        )
        mock_exit.assert_not_called()

    @patch('subprocess.Popen')
    def test_serial_running(self, mock_popen) -> None:
        """Test dependency running in serial."""
        builds_with_one_dep = [
            BuildRecipe(
                build_dir=Path('out/builddir1'),
                title='build1-dep1',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
                dependencies=['build1'],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir1'),
                title='build1',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
            ),
        ]

        log_prefix = 'INFO:pbrun.serial:'
        build_logger = logging.getLogger('pbrun.serial')
        build_logger.propagate = False
        mock_popen.return_value = _create_mock_build_command_process()

        mock_abort = MagicMock()
        pb = ProjectBuilder(
            build_recipes=builds_with_one_dep,
            source_path=Path.cwd(),  # Needed for running under bazel test
            abort_callback=mock_abort,
            root_logger=build_logger,
            execute_command=execute_command_with_logging,
            colors=False,
        )
        mock_abort.assert_not_called()

        with self.assertLogs(build_logger, level='DEBUG') as logs:
            pb.run_builds(workers=1)
        # pylint: disable=line-too-long
        expected_logs = [
            'Starting build with 2 recipes',
            '[1/2] Starting ==> Recipe: build1',
            '[1/2] Run ==> pwd',
            '[1/2] Run ==> date',
            '[1/2] Finished ==> Recipe: build1 (OK)',
            '[2/2] Starting ==> Recipe: build1-dep1',
            '[2/2] Run ==> pwd',
            '[2/2] Run ==> date',
            '[2/2] Finished ==> Recipe: build1-dep1 (OK)',
        ]
        # pylint: enable=line-too-long
        result_logs = [
            line.replace(log_prefix, '').rstrip() for line in logs.output
        ]
        for log in expected_logs:
            self.assertIn(log, result_logs)

    @patch('subprocess.Popen')
    def test_parallel_running(self, mock_popen) -> None:
        """Test dependency running in parallel."""
        builds_with_one_dep = [
            BuildRecipe(
                build_dir=Path('out/builddir1'),
                title='build1-dep1',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
                dependencies=['build1'],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir1'),
                title='build1-dep2',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
                dependencies=['build1'],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir1'),
                title='build1',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir2'),
                title='build2-dep1',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
                dependencies=['build2'],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir2'),
                title='build2',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
            ),
            BuildRecipe(
                build_dir=Path('out/builddir3'),
                title='build3',
                steps=[
                    BuildCommand(command=_COMMAND_PWD),
                    BuildCommand(command=_COMMAND_DATE),
                ],
                dependencies=['build1', 'build2'],
            ),
        ]

        log_prefix = 'INFO:pbrun.parallel:'
        build_logger = logging.getLogger('pbrun.parallel')
        build_logger.propagate = False
        mock_popen.return_value = _create_mock_build_command_process()

        mock_abort = MagicMock()
        pb = ProjectBuilder(
            build_recipes=builds_with_one_dep,
            source_path=Path.cwd(),  # Needed for running under bazel test
            abort_callback=mock_abort,
            root_logger=build_logger,
            execute_command=execute_command_with_logging,
            colors=False,
        )
        mock_abort.assert_not_called()

        with self.assertLogs(build_logger, level='DEBUG') as logs:
            pb.run_builds(workers=2)
        # pylint: disable=line-too-long
        expected_logs = [
            'Starting build with 6 recipes',
            ' ╔════════════════════════════════════',
            ' ║',
            ' ║   #1  ...   build1',
            ' ║   #2  ...   build2',
            ' ║   #3  ...   build1-dep1',
            ' ║   #4  ...   build1-dep2',
            ' ║   #5  ...   build2-dep1',
            ' ║   #6  ...   build3',
            ' ║',
            ' ╚════════════════════════════════════',
            'Dependency overview:',
            '├── build1',
            '│   ├── build1-dep1',
            '│   ├── build1-dep2',
            '│   └── build3',
            '└── build2',
            '    ├── build2-dep1',
            '    └── build3',
            '[1/6] Starting ==> Recipe: build1',
            '[1/6] Run ==> pwd',
            '[1/6] Run ==> date',
            '[1/6] Finished ==> Recipe: build1 (OK)',
            '[2/6] Starting ==> Recipe: build2',
            '[2/6] Run ==> pwd',
            '[2/6] Run ==> date',
            '[2/6] Finished ==> Recipe: build2 (OK)',
            '[3/6] Starting ==> Recipe: build1-dep1',
            '[3/6] Run ==> pwd',
            '[3/6] Run ==> date',
            '[3/6] Finished ==> Recipe: build1-dep1 (OK)',
            '[4/6] Starting ==> Recipe: build1-dep2',
            '[4/6] Run ==> pwd',
            '[4/6] Run ==> date',
            '[4/6] Finished ==> Recipe: build1-dep2 (OK)',
            '[5/6] Starting ==> Recipe: build2-dep1',
            '[5/6] Run ==> pwd',
            '[5/6] Run ==> date',
            '[5/6] Finished ==> Recipe: build2-dep1 (OK)',
            '[6/6] Starting ==> Recipe: build3',
            '[6/6] Run ==> pwd',
            '[6/6] Run ==> date',
            '[6/6] Finished ==> Recipe: build3 (OK)',
        ]
        # pylint: enable=line-too-long
        result_logs = [
            line.replace(log_prefix, '').rstrip() for line in logs.output
        ]
        for log in expected_logs:
            self.assertIn(log, result_logs)


class TestProjectBuilderFailurePropagation(unittest.TestCase):
    """Tests for ProjectBuilder failures."""

    @patch('subprocess.Popen')
    def test_serial_failure_propagation(self, mock_popen) -> None:
        """Test dependency failure propagation in serial."""
        # Recipe 1 fails
        recipe1 = BuildRecipe(
            build_dir=Path('out/builddir1'),
            title='build1',
            steps=[
                BuildCommand(command=_COMMAND_PWD),
            ],
        )
        # Recipe 2 depends on Recipe 1
        recipe2 = BuildRecipe(
            build_dir=Path('out/builddir1'),
            title='build1-dep1',
            steps=[
                BuildCommand(command=_COMMAND_DATE),
            ],
            dependencies=['build1'],
        )

        build_recipes = [
            recipe2,
            recipe1,
        ]

        build_logger = logging.getLogger('pbrun.serial_fail')
        build_logger.propagate = False

        # Mock Popen to fail for the first run recipe (build1)
        mock_popen.return_value = _create_mock_build_command_process(
            return_code=1
        )

        mock_abort = MagicMock()
        pb = ProjectBuilder(
            build_recipes=build_recipes,
            source_path=Path.cwd(),
            abort_callback=mock_abort,
            root_logger=build_logger,
            execute_command=execute_command_with_logging,
            colors=False,
        )

        with self.assertLogs(build_logger, level='DEBUG') as logs:
            pb.run_builds(workers=1)

        prefix = re.compile(r"^(INFO|ERROR):pbrun.serial_fail:")
        result_logs = [prefix.sub('', line).rstrip() for line in logs.output]

        # Check that build1 failed
        self.assertTrue(recipe1.status.failed())
        # Check that build1-dep1 failed (propagated)
        self.assertTrue(recipe2.status.failed())

        # Verify build1 ran
        self.assertIn('[1/2] Starting ==> Recipe: build1', result_logs)
        self.assertIn('[1/2] Finished ==> Recipe: build1 (FAIL)', result_logs)

        # Verify build1-dep1 did NOT run (no starting log)
        self.assertNotIn('[2/2] Starting ==> Recipe: build1-dep1', result_logs)

    @patch('subprocess.Popen')
    def test_parallel_failure_propagation(self, mock_popen) -> None:
        """Test dependency failure propagation in parallel."""
        # Recipe 1 fails
        recipe1 = BuildRecipe(
            build_dir=Path('out/builddir1'),
            title='build1',
            steps=[
                BuildCommand(command=_COMMAND_PWD),
            ],
        )
        # Recipe 2 depends on Recipe 1
        recipe2 = BuildRecipe(
            build_dir=Path('out/builddir1'),
            title='build1-dep1',
            steps=[
                BuildCommand(command=_COMMAND_DATE),
            ],
            dependencies=['build1'],
        )

        build_recipes = [recipe2, recipe1]

        build_logger = logging.getLogger('pbrun.parallel_fail')
        build_logger.propagate = False

        # Mock Popen to fail for the first run recipe (build1)
        mock_popen.return_value = _create_mock_build_command_process(
            return_code=1
        )

        mock_abort = MagicMock()
        pb = ProjectBuilder(
            build_recipes=build_recipes,
            source_path=Path.cwd(),
            abort_callback=mock_abort,
            root_logger=build_logger,
            execute_command=execute_command_with_logging,
            colors=False,
        )

        with self.assertLogs(build_logger, level='DEBUG') as logs:
            pb.run_builds(workers=2)

        prefix = re.compile(r"^(INFO|ERROR):pbrun.parallel_fail:")
        result_logs = [prefix.sub('', line).rstrip() for line in logs.output]

        # Check that build1 failed
        self.assertTrue(recipe1.status.failed())
        # Check that build1-dep1 failed (propagated)
        self.assertTrue(recipe2.status.failed())

        # Verify build1 ran
        self.assertIn('[1/2] Starting ==> Recipe: build1', result_logs)
        self.assertIn('[1/2] Finished ==> Recipe: build1 (FAIL)', result_logs)

        # Verify build1-dep1 did NOT run
        self.assertNotIn('[2/2] Starting ==> Recipe: build1-dep1', result_logs)


if __name__ == '__main__':
    unittest.main()
