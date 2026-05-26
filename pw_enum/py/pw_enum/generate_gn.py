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
"""GN wrapper for generate.py.

GN does not support extracting compiler flags directly. This wrapper parses
compiler flags from ninja files, and invokes generate.py, which Bazel and CMake
use. PW_NC_TESTs also rely on parsed ninja files for GN.
"""

import argparse
from pathlib import Path
import re
import shlex
import string
import sys
import tempfile
from typing import Any

from pw_enum.generate import main as generate_main

_RULE_REGEX = re.compile('^rule (?:cxx|.*_cxx)$')
_NINJA_VARIABLE = re.compile('^([a-zA-Z0-9_]+) = ?')
_EXPECTED_GN_VARS = (
    'asmflags',
    'cflags',
    'cflags_c',
    'cflags_cc',
    'cflags_objc',
    'cflags_objcc',
    'defines',
    'include_dirs',
)


def _find_cc_rule(toolchain_ninja_file: Path) -> str | None:
    """Searches the toolchain.ninja file for the cc rule."""
    cmd_prefix = '  command = '
    found_rule = False
    with toolchain_ninja_file.open() as fd:
        for line in fd:
            if found_rule:
                if line.startswith(cmd_prefix):
                    cmd = line[len(cmd_prefix) :].strip()
                    if cmd.startswith('ccache '):
                        cmd = cmd[len('ccache ') :]
                    return cmd
                if not line.startswith('  '):
                    break
            elif _RULE_REGEX.match(line):
                found_rule = True
    return None


def _parse_ninja_variables(target_ninja_file: Path) -> dict[str, str]:
    variables: dict[str, str] = {}
    with target_ninja_file.open() as fd:
        for line in fd:
            match = _NINJA_VARIABLE.match(line)
            if match:
                variables[match.group(1)] = line[match.end() :].strip()
    return variables


def _resolve_gn_compiler_and_flags(
    toolchain_ninja: Path,
    target_ninja: Path,
    base_cc: Path,
) -> tuple[str, list[str]]:
    """Resolves compiler path and flags from GN ninja files."""
    command_template = _find_cc_rule(toolchain_ninja)
    if command_template is None:
        raise RuntimeError(
            f"Failed to find C++ compilation command in {toolchain_ninja}"
        )

    variables = {key: '' for key in _EXPECTED_GN_VARS}
    variables.update(_parse_ninja_variables(target_ninja))

    # Set standard Ninja variables to the base_cc path
    variables['in'] = str(base_cc)
    variables['out'] = str(base_cc.with_suffix('.o'))

    resolved_command = string.Template(command_template).substitute(variables)
    split_cmd = shlex.split(resolved_command)
    if not split_cmd:
        raise RuntimeError("Resolved empty compiler command")

    if "END_OF_INVOKER" in split_cmd:
        idx = split_cmd.index("END_OF_INVOKER")
        if idx == 0:
            raise RuntimeError("END_OF_INVOKER is at index 0 in command")
        compiler = split_cmd[idx - 1]
        raw_flags = split_cmd[idx + 1 :]
    else:
        compiler = split_cmd[0]
        raw_flags = split_cmd[1:]

    if "&&" in raw_flags:
        raw_flags = raw_flags[: raw_flags.index("&&")]

    return compiler, raw_flags


def main(
    inputs: list[Path],
    outputs: list[Path],
    toolchain_ninja: Path,
    target_ninja: Path,
    base_cc: Path,
) -> int:
    """Main entry point for the GN/Ninja wrapper."""
    try:
        compiler, raw_flags = _resolve_gn_compiler_and_flags(
            toolchain_ninja, target_ninja, base_cc
        )
    except RuntimeError as e:
        print(f"Error resolving GN compiler context: {e}", file=sys.stderr)
        return 1

    # Create a temporary file to store compilation flags, as generate_main
    # expects a flags file path
    with tempfile.NamedTemporaryFile(
        mode="w", delete=False, encoding="utf-8"
    ) as f:
        flags_file = Path(f.name)
        f.write("\n".join(raw_flags))

    try:
        return generate_main(
            inputs,
            outputs,
            compiler,
            flags_file,
            base_cc,
        )
    finally:
        try:
            flags_file.unlink()
        except OSError:
            pass


def _parse_args() -> dict[str, Any]:
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(
        description=(
            "GN wrapper for generate.py utilizing toolchain and target ninja "
            "files"
        )
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="Input headers")
    parser.add_argument(
        "--outputs", nargs="+", type=Path, help="Output headers", required=True
    )
    parser.add_argument(
        "--toolchain-ninja",
        type=Path,
        required=True,
        help="toolchain.ninja file",
    )
    parser.add_argument(
        "--target-ninja",
        type=Path,
        required=True,
        help="target.ninja file",
    )
    parser.add_argument(
        "--base-cc",
        type=Path,
        required=True,
        help="base.cc placeholder file path",
    )
    return vars(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main(**_parse_args()))
