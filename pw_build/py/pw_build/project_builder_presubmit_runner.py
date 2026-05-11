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
"""pw_build.project_builder_presubmit_runner"""

from __future__ import annotations

import argparse
import fnmatch
import logging
from pathlib import Path

from pw_cli.arguments import (
    print_completions_for_option,
    add_tab_complete_arguments,
)
from pw_cli.collect_files import file_summary
import pw_cli.env
from pw_cli.git_repo import fetch_file_lists
import pw_cli.log

from pw_presubmit import (
    Check,
    PresubmitResult,
    PresubmitFailure,
    Program,
    Programs,
    PresubmitContext,
)
from pw_presubmit.build import gn_args
from pw_presubmit.build_with_pw_package import GnGenNinja
import pw_presubmit.pigweed_presubmit
from pw_presubmit.presubmit_context import (
    get_check_traces,
)
from pw_presubmit.private.check import PresubmitCheckTrace

# pw_watch is not required by pw_build, this is an optional feature.
try:
    from pw_watch.argparser import (  # type: ignore
        add_parser_arguments as add_watch_arguments,
    )
    from pw_watch.watch import run_watch, watch_setup  # type: ignore
    from pw_watch.watch_app import WatchAppPrefs  # type: ignore

    PW_WATCH_AVAILABLE = True
except ImportError:
    PW_WATCH_AVAILABLE = False

from pw_build.project_builder import (
    ProjectBuilder,
    run_builds,
    ASCII_CHARSET,
    EMOJI_CHARSET,
)
from pw_build.build_recipe import (
    BuildCommand,
    BuildRecipe,
    UnknownBuildSystem,
    create_build_recipes,
    should_gn_gen,
)
from pw_build.project_builder_argparse import add_project_builder_arguments
from pw_build.project_builder_prefs import ProjectBuilderPrefs


_COLOR = pw_cli.color.colors()
_LOG = logging.getLogger('pw_build')


class PresubmitTraceAnnotationError(Exception):
    """Exception for malformed PresubmitCheckTrace annotations."""


def _pw_package_install_command(package_name: str) -> BuildCommand:
    return BuildCommand(
        command=[
            'pw',
            '--no-banner',
            'package',
            'install',
            package_name,
        ],
    )


def _pw_package_install_to_build_command(
    trace: PresubmitCheckTrace,
) -> BuildCommand:
    """Returns a BuildCommand from a PresubmitCheckTrace."""
    package_name = trace.call_annotation.get('pw_package_install', None)
    if package_name is None:
        raise PresubmitTraceAnnotationError(
            'Missing "pw_package_install" value.'
        )

    return _pw_package_install_command(package_name)


def _bazel_command_args_to_build_commands(
    trace: PresubmitCheckTrace,
) -> list[BuildCommand]:
    """Returns a list of BuildCommands based on a bazel PresubmitCheckTrace."""
    build_steps: list[BuildCommand] = []

    if 'bazel' not in trace.args:
        return build_steps

    bazel_command = list(arg for arg in trace.args if not arg.startswith('--'))
    bazel_options = list(
        arg for arg in trace.args if arg.startswith('--') and arg != '--'
    )
    # Check for bazel build, info or test subcommands.
    if not (
        bazel_command[0].endswith('bazel')
        and bazel_command[1] in ['build', 'info', 'test']
    ):
        raise UnknownBuildSystem(
            f'Unable to parse bazel command:\n  {trace.args}'
        )

    bazel_subcommand = bazel_command[1]
    bazel_targets = bazel_command[2:]
    build_steps.append(
        BuildCommand(
            build_system_command='bazel',
            build_system_extra_args=[bazel_subcommand] + bazel_options,
            targets=bazel_targets,
        )
    )
    return build_steps


def _presubmit_trace_to_build_commands(
    ctx: PresubmitContext,
    presubmit_step: Check,
) -> list[BuildCommand]:
    """Convert a presubmit step to a list of BuildCommands.

    Specifically, this handles the following types of PresubmitCheckTraces:

    - pw package installs
    - gn gen followed by ninja
    - bazel commands

    If none of the specific scenarios listed above are found the command args
    are passed along to BuildCommand as is.

    Returns:
      List of BuildCommands representing each command found in the
      presubmit_step traces.
    """
    build_steps: list[BuildCommand] = []

    result = presubmit_step(ctx)
    if result == PresubmitResult.FAIL:
        raise PresubmitFailure(
            '\n\nERROR: This presubmit cannot be run with "pw build". '
            'Please run with:\n\n'
            f'  pw presubmit --step {presubmit_step}'
        )

    step_traces = get_check_traces(ctx)

    for trace in step_traces:
        trace_args = list(trace.args)
        # Check for ninja -t graph command and skip it
        if trace_args[0].endswith('ninja'):
            try:
                dash_t_index = trace_args.index('-t')
                graph_index = trace_args.index('graph')
                if graph_index == dash_t_index + 1:
                    # This trace has -t graph, skip it.
                    continue
            except ValueError:
                # '-t graph' was not found
                pass

        if 'pw_package_install' in trace.call_annotation:
            build_steps.append(_pw_package_install_to_build_command(trace))
            continue

        if 'bazel' in trace.args:
            build_steps.extend(_bazel_command_args_to_build_commands(trace))
            continue

        # Check for gn gen or pw-wrap-ninja
        transformed_args = []
        pw_wrap_ninja_found = False
        gn_found = False
        gn_gen_found = False

        for arg in trace.args:
            # Check for a 'gn gen' command
            if arg == 'gn':
                gn_found = True
            if arg == 'gen' and gn_found:
                gn_gen_found = True

            # Check for pw-wrap-ninja, pw build doesn't use this.
            if arg == 'pw-wrap-ninja':
                # Use ninja instead
                transformed_args.append('ninja')
                pw_wrap_ninja_found = True
                continue
            # Remove --log-actions if pw-wrap-ninja was found. This is a
            # non-standard ninja arg.
            if pw_wrap_ninja_found and arg == '--log-actions':
                continue
            transformed_args.append(str(arg))

        if gn_gen_found:
            # Run the command with run_if=should_gn_gen
            build_steps.append(
                BuildCommand(run_if=should_gn_gen, command=transformed_args)
            )
        else:
            # Run the command as is.
            build_steps.append(BuildCommand(command=transformed_args))

    return build_steps


def presubmit_build_recipe(  # pylint: disable=too-many-locals
    repo_root: Path,
    presubmit_out_dir: Path,
    package_root: Path,
    presubmit_step: Check,
    all_files: list[Path],
    modified_files: list[Path],
) -> BuildRecipe | None:
    """Construct a BuildRecipe from a pw_presubmit step."""
    out_dir = presubmit_out_dir / presubmit_step.name

    ctx = PresubmitContext(
        root=repo_root,
        repos=(repo_root,),
        output_dir=out_dir,
        paths=tuple(modified_files),
        all_paths=tuple(all_files),
        package_root=package_root,
        luci=None,
        override_gn_args={},
        num_jobs=None,
        continue_after_build_error=True,
        format_options=pw_presubmit.presubmit.FormatOptions.load(),
        dry_run=True,
    )

    program = Program('', [presubmit_step])
    checks = list(program.filter(repo_root, modified_files))
    if not checks:
        _LOG.warning('')
        _LOG.warning(
            'Step "%s" is not required for the current set of modified files.',
            presubmit_step.name,
        )
        _LOG.warning('')
        return None

    try:
        ctx.paths = tuple(checks[0].paths)
    except IndexError:
        raise PresubmitTraceAnnotationError(
            'Missing pw_presubmit.presubmit.Check for presubmit step:\n'
            + repr(presubmit_step)
        )

    if isinstance(presubmit_step, GnGenNinja):
        # GnGenNinja is directly translatable to a BuildRecipe.
        selected_gn_args = {
            name: value(ctx) if callable(value) else value
            for name, value in presubmit_step.gn_args.items()
        }

        return BuildRecipe(
            build_dir=out_dir,
            title=presubmit_step.name,
            steps=[
                _pw_package_install_command(name)
                for name in presubmit_step._packages  # pylint: disable=protected-access
            ]
            + [
                BuildCommand(
                    run_if=should_gn_gen,
                    command=[
                        'gn',
                        'gen',
                        str(out_dir),
                        gn_args(**selected_gn_args),
                    ],
                ),
                BuildCommand(
                    build_system_command='ninja',
                    targets=presubmit_step.ninja_targets,
                ),
            ],
        )

    # Unknown type of presubmit, use dry-run to capture subprocess traces.
    build_steps = _presubmit_trace_to_build_commands(ctx, presubmit_step)

    out_dir.mkdir(parents=True, exist_ok=True)

    return BuildRecipe(
        build_dir=out_dir,
        title=presubmit_step.name,
        steps=build_steps,
    )


def get_parser(
    presubmit_programs: Programs | None = None,
    build_recipes: list[BuildRecipe] | None = None,
) -> argparse.ArgumentParser:
    """Setup argparse for pw_build.project_builder and optionally pw_watch."""
    parser = argparse.ArgumentParser(
        prog='pw build',
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    if PW_WATCH_AVAILABLE:
        parser = add_watch_arguments(parser)
    else:
        parser = add_project_builder_arguments(parser)

    if build_recipes is not None:

        def build_recipe_argparse_type(arg: str) -> list[BuildRecipe]:
            """Return a list of matching presubmit steps."""
            assert build_recipes
            all_recipe_names = list(
                recipe.display_name for recipe in build_recipes
            )
            filtered_names = fnmatch.filter(all_recipe_names, arg)

            if not filtered_names:
                recipe_name_str = '\n'.join(sorted(all_recipe_names))
                raise argparse.ArgumentTypeError(
                    f'"{arg}" does not match the name of a recipe.\n\n'
                    f'Valid Recipes:\n{recipe_name_str}'
                )

            return list(
                recipe
                for recipe in build_recipes
                if recipe.display_name in filtered_names
            )

        parser.add_argument(
            '-r',
            '--recipe',
            action='extend',
            default=[],
            help=(
                'Run a build recipe. Include an asterix to match more than one '
                "name. For example: --recipe 'gn_*'"
            ),
            type=build_recipe_argparse_type,
        )

    if presubmit_programs is not None:
        # Add presubmit step arguments.
        all_steps = presubmit_programs.all_steps()

        def presubmit_step_argparse_type(arg: str) -> list[Check]:
            """Return a list of matching presubmit steps."""
            filtered_step_names = fnmatch.filter(all_steps.keys(), arg)

            if not filtered_step_names:
                all_step_names = '\n'.join(sorted(all_steps.keys()))
                raise argparse.ArgumentTypeError(
                    f'"{arg}" does not match the name of a presubmit step.\n\n'
                    f'Valid Steps:\n{all_step_names}'
                )

            return list(all_steps[name] for name in filtered_step_names)

        parser.add_argument(
            '-s',
            '--step',
            action='extend',
            default=[],
            help=(
                'Run presubmit step. Include an asterix to match more than one '
                "step name. For example: --step '*_format'"
            ),
            type=presubmit_step_argparse_type,
        )

    if build_recipes or presubmit_programs:
        parser.add_argument(
            '-l',
            '--list',
            action='store_true',
            default=False,
            help=('List all known build recipes and presubmit steps.'),
        )

    if build_recipes:
        parser.add_argument(
            '--all',
            action='store_true',
            default=False,
            help=('Run all known build recipes.'),
        )

    parser.add_argument(
        '--progress-bars',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Show progress bars in the terminal.',
    )

    parser.add_argument(
        '--log-build-steps',
        action=argparse.BooleanOptionalAction,
        help='Show ninja build step log lines in output.',
    )

    if PW_WATCH_AVAILABLE:
        parser.add_argument(
            '-w',
            '--watch',
            action='store_true',
            help='Use pw_watch to monitor changes.',
            default=False,
        )

    parser.add_argument(
        '-b',
        '--base',
        help=(
            'Git revision to diff for changed files. This is used for '
            'presubmit steps.'
        ),
    )

    parser = add_tab_complete_arguments(parser)

    parser.add_argument(
        '--tab-complete-recipe',
        nargs='?',
        help='Print tab completions for the supplied recipe name.',
    )

    parser.add_argument(
        '--tab-complete-presubmit-step',
        nargs='?',
        help='Print tab completions for the supplied presubmit name.',
    )

    return parser


def _get_prefs(
    args: argparse.Namespace,
) -> ProjectBuilderPrefs | WatchAppPrefs:
    """Load either WatchAppPrefs or ProjectBuilderPrefs.

    Applies the command line args to the correct prefs class.

    Returns:
      A WatchAppPrefs instance if pw_watch is importable, ProjectBuilderPrefs
      otherwise.
    """
    prefs: ProjectBuilderPrefs | WatchAppPrefs
    if PW_WATCH_AVAILABLE:
        prefs = WatchAppPrefs(load_argparse_arguments=add_watch_arguments)
        prefs.apply_command_line_args(args)
    else:
        prefs = ProjectBuilderPrefs(
            load_argparse_arguments=add_project_builder_arguments,
        )
        prefs.apply_command_line_args(args)
    return prefs


def load_presubmit_build_recipes(
    presubmit_programs: Programs,
    presubmit_steps: list[Check],
    repo_root: Path,
    presubmit_out_dir: Path,
    package_root: Path,
    all_files: list[Path],
    modified_files: list[Path],
    default_presubmit_step_names: list[str] | None = None,
) -> list[BuildRecipe]:
    """Convert selected presubmit steps into a list of BuildRecipes."""
    # Use the default presubmit if no other steps or command line out
    # directories are provided.
    if len(presubmit_steps) == 0 and default_presubmit_step_names:
        default_steps = list(
            check
            for name, check in presubmit_programs.all_steps().items()
            if name in default_presubmit_step_names
        )
        presubmit_steps = default_steps

    presubmit_recipes: list[BuildRecipe] = []

    for step in presubmit_steps:
        build_recipe = presubmit_build_recipe(
            repo_root,
            presubmit_out_dir,
            package_root,
            step,
            all_files,
            modified_files,
        )
        if build_recipe:
            presubmit_recipes.append(build_recipe)

    return presubmit_recipes


def _tab_complete_recipe(
    build_recipes: list[BuildRecipe],
    text: str = '',
) -> None:
    for name in sorted(recipe.display_name for recipe in build_recipes):
        if name.startswith(text):
            print(name)


def _tab_complete_presubmit_step(
    presubmit_programs: Programs,
    text: str = '',
) -> None:
    for name in sorted(presubmit_programs.all_steps().keys()):
        if name.startswith(text):
            print(name)


def _list_steps_and_recipes(
    presubmit_programs: Programs | None = None,
    build_recipes: list[BuildRecipe] | None = None,
) -> None:
    if presubmit_programs:
        _LOG.info('Presubmit steps:')
        print()
        for name in sorted(presubmit_programs.all_steps().keys()):
            print(name)
        print()
    if build_recipes:
        _LOG.info('Build recipes:')
        print()
        for name in sorted(recipe.display_name for recipe in build_recipes):
            print(name)
        print()


def _print_usage_help(
    presubmit_programs: Programs | None = None,
    build_recipes: list[BuildRecipe] | None = None,
) -> None:
    """Print usage examples with known presubmits and build recipes."""

    def print_pw_build(
        option: str, arg: str | None = None, end: str = '\n'
    ) -> None:
        print(
            ' '.join(
                [
                    'pw build',
                    _COLOR.cyan(option),
                    _COLOR.yellow(arg) if arg else '',
                ]
            ),
            end=end,
        )

    if presubmit_programs:
        print(_COLOR.green('All presubmit steps:'))
        for name in sorted(presubmit_programs.all_steps().keys()):
            print_pw_build('--step', name)
    if build_recipes:
        if presubmit_programs:
            # Add a blank line separator
            print()
        print(_COLOR.green('All build recipes:'))
        for name in sorted(recipe.display_name for recipe in build_recipes):
            print_pw_build('--recipe', name)

        print()
        print(
            _COLOR.green(
                'Recipe and step names may use wildcards and be repeated:'
            )
        )
        print_pw_build('--recipe', '"default_*"', end=' ')
        print(
            _COLOR.cyan('--step'),
            _COLOR.yellow('step1'),
            _COLOR.cyan('--step'),
            _COLOR.yellow('step2'),
        )
        print()
        print(_COLOR.green('Run all build recipes:'))
        print_pw_build('--all')
        print()
        print(_COLOR.green('For more help please run:'))
        print_pw_build('--help')


def main(
    presubmit_programs: Programs | None = None,
    default_presubmit_step_names: list[str] | None = None,
    build_recipes: list[BuildRecipe] | None = None,
    default_build_recipe_names: list[str] | None = None,
    repo_root: Path | None = None,
    presubmit_out_dir: Path | None = None,
    package_root: Path | None = None,
    default_root_logfile: Path = Path('out/build.txt'),
    force_pw_watch: bool = False,
) -> int:
    """Build upstream Pigweed presubmit steps."""
    # pylint: disable=too-many-locals,too-many-branches
    parser = get_parser(presubmit_programs, build_recipes)
    args = parser.parse_args()

    if args.tab_complete_option is not None:
        print_completions_for_option(
            parser,
            text=args.tab_complete_option,
            tab_completion_format=args.tab_complete_format,
        )
        return 0

    log_level = logging.DEBUG if args.debug_logging else logging.INFO

    pw_cli.log.install(
        level=log_level,
        use_color=args.colors,
        # Hide the date from the timestamp
        time_format='%H:%M:%S',
    )

    pw_env = pw_cli.env.pigweed_environment()
    if pw_env.PW_EMOJI:
        charset = EMOJI_CHARSET
    else:
        charset = ASCII_CHARSET

    if args.tab_complete_recipe is not None:
        if build_recipes:
            _tab_complete_recipe(build_recipes, text=args.tab_complete_recipe)
        # Must exit if there are no build_recipes.
        return 0

    if args.tab_complete_presubmit_step is not None:
        if presubmit_programs:
            _tab_complete_presubmit_step(
                presubmit_programs, text=args.tab_complete_presubmit_step
            )
        # Must exit if there are no presubmit_programs.
        return 0

    # List valid steps + recipes.
    if hasattr(args, 'list') and args.list:
        _list_steps_and_recipes(presubmit_programs, build_recipes)
        return 0

    command_line_dash_c_recipes: list[BuildRecipe] = []
    # If -C out directories are provided add them to the recipes list.
    if args.build_directories:
        prefs = _get_prefs(args)
        command_line_dash_c_recipes = create_build_recipes(prefs)

    if repo_root is None:
        repo_root = pw_cli.env.project_root()
    if presubmit_out_dir is None:
        presubmit_out_dir = repo_root / 'out/presubmit'
    if package_root is None:
        package_root = pw_env.PW_PACKAGE_ROOT

    all_files: list[Path]
    modified_files: list[Path]

    all_files, modified_files = fetch_file_lists(
        root=repo_root,
        repo=repo_root,
        pathspecs=[],
        base=args.base,
    )

    # Log modified file summary just like pw_presubmit if using --base.
    if args.base:
        _LOG.info(
            'Running steps that apply to modified files since "%s":', args.base
        )
        _LOG.info('')
        for line in file_summary(
            mf.relative_to(repo_root) for mf in modified_files
        ):
            _LOG.info(line)
        _LOG.info('')

    selected_build_recipes: list[BuildRecipe] = []
    if build_recipes:
        if hasattr(args, 'recipe'):
            selected_build_recipes = args.recipe
        if not selected_build_recipes and default_build_recipe_names:
            selected_build_recipes = [
                recipe
                for recipe in build_recipes
                if recipe.display_name in default_build_recipe_names
            ]

    selected_presubmit_recipes: list[BuildRecipe] = []
    if presubmit_programs and hasattr(args, 'step'):
        selected_presubmit_recipes = load_presubmit_build_recipes(
            presubmit_programs,
            args.step,
            repo_root,
            presubmit_out_dir,
            package_root,
            all_files,
            modified_files,
            default_presubmit_step_names=default_presubmit_step_names,
        )

    # If no builds specifed on the command line print a useful help message:
    if (
        not selected_build_recipes
        and not command_line_dash_c_recipes
        and not selected_presubmit_recipes
        and not args.all
    ):
        _print_usage_help(presubmit_programs, build_recipes)
        return 1

    if build_recipes and args.all:
        selected_build_recipes = build_recipes

    # Run these builds in order:
    recipes_to_build = (
        # -C dirs
        command_line_dash_c_recipes
        # --step 'name'
        + selected_presubmit_recipes
        # --recipe 'name'
        + selected_build_recipes
    )

    # Always set separate build file logging.
    if not args.logfile:
        args.logfile = default_root_logfile
    if not args.separate_logfiles:
        args.separate_logfiles = True

    workers = 1
    if args.parallel:
        # If parallel is requested and parallel_workers is set to 0 run all
        # recipes in parallel. That is, use the number of recipes as the worker
        # count.
        if args.parallel_workers == 0:
            workers = len(recipes_to_build)
        else:
            workers = args.parallel_workers

    project_builder = ProjectBuilder(
        build_recipes=recipes_to_build,
        jobs=args.jobs,
        banners=args.banners,
        keep_going=args.keep_going,
        colors=args.colors,
        charset=charset,
        separate_build_file_logging=args.separate_logfiles,
        # If running builds in serial, send all sub build logs to the root log
        # window (or terminal).
        send_recipe_logs_to_root=(workers == 1),
        root_logfile=args.logfile,
        root_logger=_LOG,
        log_level=log_level,
        allow_progress_bars=args.progress_bars,
        log_build_steps=args.log_build_steps,
        source_path=args.source_path,
    )

    if project_builder.should_use_progress_bars():
        project_builder.use_stdout_proxy()

    if PW_WATCH_AVAILABLE and (
        force_pw_watch or (args.watch or args.fullscreen)
    ):
        event_handler, exclude_list = watch_setup(
            project_builder,
            banners=args.banners,
            parallel=args.parallel,
            parallel_workers=workers,
            fullscreen=args.fullscreen,
            logfile=args.logfile,
            separate_logfiles=args.separate_logfiles,
        )

        run_watch(
            event_handler,
            exclude_list,
            fullscreen=args.fullscreen,
        )
        return 0

    # One off build
    return run_builds(project_builder, workers)
