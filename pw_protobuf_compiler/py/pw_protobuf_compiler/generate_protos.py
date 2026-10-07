# Copyright 2020 The Pigweed Authors
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
"""Script that invokes protoc to generate code for .proto files."""

import argparse
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Callable

# Make sure dependencies are optional, since this script may be run when
# installing Python package dependencies through GN.
try:
    from pw_cli.log import install as setup_logging
except ImportError:
    from logging import basicConfig as setup_logging  # type: ignore

_LOG = logging.getLogger(__name__)


def _argument_parser() -> argparse.ArgumentParser:
    """Registers the script's arguments on an argument parser."""

    parser = argparse.ArgumentParser(description=__doc__)

    parser.add_argument(
        '--language',
        required=True,
        choices=DEFAULT_PROTOC_ARGS,
        help='Output language',
    )
    parser.add_argument(
        '--plugin-path', type=Path, help='Path to the protoc plugin'
    )
    parser.add_argument(
        '--proto-path',
        type=Path,
        help='Additional protoc include paths',
        action='append',
    )
    parser.add_argument(
        '--include-file',
        type=argparse.FileType('r'),
        help='File containing additional protoc include paths',
    )
    parser.add_argument(
        '--out-dir',
        type=Path,
        required=True,
        help='Output directory for generated code',
    )
    parser.add_argument(
        '--compile-dir',
        type=Path,
        help='Root path for compilation',
    )
    parser.add_argument(
        '--sources', type=Path, nargs='+', help='Input protobuf files'
    )
    parser.add_argument(
        '--protoc', type=Path, default='protoc', help='Path to protoc'
    )
    parser.add_argument(
        '--no-experimental-proto3-optional',
        dest='experimental_proto3_optional',
        action='store_false',
        help='Do not invoke protoc with --experimental_allow_proto3_optional',
    )
    parser.add_argument(
        '--no-experimental-editions',
        dest='experimental_editions',
        action='store_false',
        help='Do not invoke protoc with --experimental_editions',
    )
    parser.add_argument(
        '--no-generate-type-hints',
        dest='generate_type_hints',
        action='store_false',
        help='Do not generate pyi files for python',
    )
    parser.add_argument(
        '--exclude-pwpb-legacy-snake-case-field-name-enums',
        dest='exclude_pwpb_legacy_snake_case_field_name_enums',
        action='store_true',
        help=(
            'If set, generates legacy SNAKE_CASE names for field name enums '
            'in PWPB.'
        ),
    )
    parser.add_argument(
        '--pwpb-no-generic-options-files',
        action='store_true',
        help=(
            'If set, requires the use of the `.pwpb_options` for pw_protobuf '
            'options files'
        ),
    )
    parser.add_argument(
        '--pwpb-no-oneof-callbacks',
        action='store_true',
        help='Generate legacy inline oneof members instead of callbacks',
    )
    parser.add_argument(
        '--nanopb-pb2-dir',
        type=Path,
        help='Directory containing the generated nanopb_pb2',
    )

    return parser


def protoc_common_args(args: argparse.Namespace) -> tuple[str, ...]:
    flags: tuple[str, ...] = ()
    if args.experimental_proto3_optional:
        flags += ('--experimental_allow_proto3_optional',)
    if args.experimental_editions:
        flags += ('--experimental_editions',)
    return flags


def protoc_pwpb_args(
    args: argparse.Namespace, include_paths: list[str]
) -> tuple[str, ...]:
    out_args = [
        '--plugin',
        f'protoc-gen-custom={args.plugin_path}',
        *[f'--custom_opt=-I{include_path}' for include_path in include_paths],
    ]

    if args.exclude_pwpb_legacy_snake_case_field_name_enums:
        out_args.append(
            '--custom_opt=--exclude-legacy-snake-case-field-name-enums'
        )
    if args.pwpb_no_generic_options_files:
        out_args.append('--custom_opt=--no-generic-options-files')
    if args.pwpb_no_oneof_callbacks:
        out_args.append('--custom_opt=--no-oneof-callbacks')

    out_args.extend(
        [
            '--custom_out',
            args.out_dir,
        ]
    )

    return tuple(out_args)


def protoc_pwpb_rpc_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    return (
        '--plugin',
        f'protoc-gen-custom={args.plugin_path}',
        '--custom_out',
        args.out_dir,
    )


def protoc_go_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    return (
        '--go_out',
        f'plugins=grpc:{args.out_dir}',
    )


def protoc_nanopb_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    # nanopb needs to know of the include path to parse *.options files
    return (
        '--plugin',
        f'protoc-gen-nanopb={args.plugin_path}',
        # nanopb_opt provides the flags to use for nanopb_out. Windows doesn't
        # like when you merge the two using the `flag,...:out` syntax. Use
        # Posix-style paths since backslashes on Windows are treated like
        # escape characters.
        f'--nanopb_opt=-I{args.compile_dir.as_posix()}',
        f'--nanopb_out={args.out_dir}',
    )


def protoc_nanopb_rpc_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    return (
        '--plugin',
        f'protoc-gen-custom={args.plugin_path}',
        '--custom_out',
        args.out_dir,
    )


def protoc_raw_rpc_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    return (
        '--plugin',
        f'protoc-gen-custom={args.plugin_path}',
        '--custom_out',
        args.out_dir,
    )


def protoc_python_args(
    args: argparse.Namespace, _include_paths: list[str]
) -> tuple[str, ...]:
    flags: tuple[str, ...] = (
        '--python_out',
        args.out_dir,
    )

    if args.generate_type_hints and shutil.which('protoc-gen-mypy'):
        flags += (
            '--mypy_out',
            args.out_dir,
        )

    return flags


_DefaultArgsFunction = Callable[
    [argparse.Namespace, list[str]], tuple[str, ...]
]

# Default additional protoc arguments for each supported language.
# TODO(frolv): Make these overridable with a command-line argument.
DEFAULT_PROTOC_ARGS: dict[str, _DefaultArgsFunction] = {
    'go': protoc_go_args,
    'nanopb': protoc_nanopb_args,
    'nanopb_rpc': protoc_nanopb_rpc_args,
    'pwpb': protoc_pwpb_args,
    'pwpb_rpc': protoc_pwpb_rpc_args,
    'pwpb_rpc2': protoc_pwpb_rpc_args,
    'python': protoc_python_args,
    'raw_rpc': protoc_raw_rpc_args,
    'raw_rpc2': protoc_raw_rpc_args,
}

# Languages that protoc internally supports.
BUILTIN_PROTOC_LANGS = ('go', 'python')


def main(input_args) -> int:
    """Runs protoc as configured by command-line arguments."""
    parser = _argument_parser()
    args = parser.parse_args(input_args)

    if args.language == 'nanopb' and args.compile_dir is None:
        parser.error("--compile-dir is required when --language is nanopb.")

    if args.plugin_path is None and args.language not in BUILTIN_PROTOC_LANGS:
        parser.error(
            f'--plugin-path is required for --language {args.language}'
        )

    args.out_dir.mkdir(parents=True, exist_ok=True)

    include_paths: list[str] = []
    if args.compile_dir:
        include_paths.append(args.compile_dir)
    if args.include_file:
        include_paths.extend(line.strip() for line in args.include_file)
    if args.proto_path:
        include_paths.extend(str(path) for path in args.proto_path)

    wrapper_script: Path | None = None

    # On Windows, use a .bat version of the plugin if it exists or create a .bat
    # wrapper to use if none exists.
    if os.name == 'nt' and args.plugin_path:
        if args.plugin_path.with_suffix('.bat').exists():
            args.plugin_path = args.plugin_path.with_suffix('.bat')
            _LOG.debug('Using Batch plugin %s', args.plugin_path)
        else:
            with tempfile.NamedTemporaryFile(
                'w', suffix='.bat', delete=False
            ) as file:
                file.write(f'@echo off\npython {args.plugin_path.resolve()}\n')

            args.plugin_path = wrapper_script = Path(file.name)
            _LOG.debug('Using generated plugin wrapper %s', args.plugin_path)

    cmd: tuple[str | Path, ...] = (
        args.protoc,
        *[f'-I{include_path}' for include_path in include_paths],
        *protoc_common_args(args),
        *DEFAULT_PROTOC_ARGS[args.language](args, include_paths),
        *args.sources,
    )

    env = os.environ.copy()
    if args.nanopb_pb2_dir:
        env['NANOPB_PB2_NO_REBUILD'] = '1'
        pythonpath = env.get('PYTHONPATH', '').split(os.pathsep)
        pythonpath.insert(0, str(args.nanopb_pb2_dir))
        env['PYTHONPATH'] = os.pathsep.join(pythonpath)

    try:
        process = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
        )
    finally:
        if wrapper_script:
            wrapper_script.unlink()

    if process.returncode != 0:
        _LOG.error(
            'Protocol buffer compilation failed!\n%s',
            ' '.join(str(c) for c in cmd),
        )
        sys.stderr.buffer.write(process.stdout)
        sys.stderr.flush()

    return process.returncode


if __name__ == '__main__':
    setup_logging()
    sys.exit(main(sys.argv[1:]))
