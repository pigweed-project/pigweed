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
"""Generates code from language-neutral pw_enum TOML and JSON definitions.

Subcommands:

- ``check``: Validates enum sources without generating code.
- ``cc``: Generates a C++ header for each enum source.
- ``py``: Generates a Python module for each enum source.

Arguments prefixed with ``@`` are read from a file with one argument per line,
so build systems can pass lists of dependency sources in files.
"""

import argparse
from collections.abc import Callable, Iterator, Sequence
from pathlib import Path
import sys

from pw_codegen import python
from pw_enum.generate import generate_cc_header
from pw_enum.generate_py import generate_py_module
from pw_enum.json_source import resolve_sources
from pw_enum.parse import EnumDescriptor, ParseError

_Generator = Callable[[str, Sequence[EnumDescriptor]], Iterator[str]]

_GENERATORS: dict[str, _Generator] = {
    "cc": generate_cc_header,
    "py": generate_py_module,
}


def _write_lines(path: Path, lines: Iterator[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        for line in lines:
            file.write(line)
            file.write("\n")


def _write_depfile(path: Path, output: Path, inputs: Sequence[Path]) -> None:
    """Writes a Makefile-style depfile, as used by Ninja."""

    def escape(file: Path) -> str:
        # Escape the characters that are special in Makefile-style depfiles.
        return (
            file.as_posix()
            .replace("$", "$$")
            .replace("#", "\\#")
            .replace(" ", "\\ ")
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"{escape(output)}: {' '.join(escape(i) for i in inputs)}\n",
        encoding="utf-8",
    )


def _run(args: argparse.Namespace) -> None:
    resolved = resolve_sources(args.sources, args.deps, args.transitive_deps)

    if args.command == "check":
        outputs = [args.stamp]
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        args.stamp.write_text("", encoding="utf-8")
    else:
        outputs = args.outputs
        _generate(args, resolved)

    if args.depfile:
        inputs = dict.fromkeys(
            [*args.sources, *args.deps, *args.transitive_deps]
        )
        _write_depfile(args.depfile, outputs[0], list(inputs))


def _check_import_paths(import_paths: Sequence[str], num_sources: int) -> None:
    """Checks that each part of each import path is a Python module name."""
    if len(import_paths) != num_sources:
        raise ValueError(
            f"Expected one import path per source; got {num_sources} "
            f"sources and {len(import_paths)} import paths"
        )
    for import_path in import_paths:
        for part in import_path.split("/"):
            if not python.is_identifier(part):
                raise ValueError(
                    f"Cannot generate Python for {import_path}: {part!r} is "
                    "not a valid Python module name"
                )


def _generate(
    args: argparse.Namespace, resolved: Sequence[tuple[EnumDescriptor, ...]]
) -> None:
    if len(args.outputs) != len(args.sources):
        raise ValueError(
            f"Expected one output per source; got {len(args.sources)} "
            f"sources and {len(args.outputs)} outputs"
        )
    if args.command == "py":
        _check_import_paths(
            args.import_paths or [output.stem for output in args.outputs],
            len(args.sources),
        )

    generator = _GENERATORS[args.command]
    for source, enums, output in zip(args.sources, resolved, args.outputs):
        _write_lines(output, generator(source.as_posix(), enums))


class _ArgumentParser(argparse.ArgumentParser):
    """Reads arguments from @files, one per line, ignoring blank lines."""

    def convert_arg_line_to_args(self, arg_line: str) -> list[str]:
        return [arg_line] if arg_line.strip() else []


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the pw_enum code generator."""
    parser = _ArgumentParser(
        description=__doc__.splitlines()[0], fromfile_prefix_chars="@"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command, help_text in (
        ("check", "Validate enum sources"),
        ("cc", "Generate C++ headers"),
        ("py", "Generate Python modules"),
    ):
        sub = subparsers.add_parser(command, help=help_text)
        sub.add_argument("--sources", nargs="+", type=Path, required=True)
        sub.add_argument(
            "--deps",
            nargs="*",
            type=Path,
            default=[],
            help="Direct dependency sources, which sources may reference",
        )
        sub.add_argument(
            "--transitive-deps",
            nargs="*",
            type=Path,
            default=[],
            help="All transitive dependency sources",
        )
        sub.add_argument(
            "--depfile",
            type=Path,
            help="Write a depfile listing the sources that were read",
        )
        if command == "check":
            sub.add_argument(
                "--stamp",
                type=Path,
                required=True,
                help="File to write if the sources are valid",
            )
        else:
            sub.add_argument("--outputs", nargs="+", type=Path, required=True)
        if command == "py":
            sub.add_argument(
                "--import-paths",
                nargs="+",
                help=(
                    "Import path of each source without an extension (e.g. "
                    "bar/baz/foo), which must form a valid Python module "
                    "name; only the output file names are checked if omitted"
                ),
            )

    try:
        _run(parser.parse_args(argv))
    except (OSError, ParseError, ValueError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
