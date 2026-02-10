#!/usr/bin/env python3
# Copyright 2024 The Pigweed Authors
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
"""Substitute a template file with variables from bazel workspace status."""
import argparse
import string
import sys
from typing import TextIO


def _load_status_file(file: TextIO) -> dict[str, str]:
    """Load a bazel status file.

    E.g. bazel-out/stable-status.txt and volatile-status.txt.

    Args:
      file: The open status file handle to be read.

    Returns:
      A dictionary of key => value pairs (both strings) read from the file.
      If the value is missing, and empty string is provided.
    """
    result = {}
    for line in file:
        line = line.strip()
        parts = line.split(maxsplit=1)
        key = parts[0]
        value = parts[1] if len(parts) >= 2 else ""
        result[key] = value
    return result


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--status-file",
        action="append",
        dest="status_files",
        type=argparse.FileType("r"),
        help="A bazel status file, e.g. bazel-out/stable-status.txt",
    )
    parser.add_argument(
        "template",
        type=argparse.FileType("r"),
        help="The input template file whose $(VARIABLES) are to be expanded.",
    )
    parser.add_argument(
        "out",
        type=argparse.FileType("w"),
        nargs="?",
        default=sys.stdout,
        help="The output file to which the expanded template is written.",
    )

    args = parser.parse_args()

    if len(args.status_files) < 1:
        parser.error("At least one --status-file is required")

    return args


class SubstitutionError(Exception):
    pass


def _substitute_file(
    input_file: TextIO,
    output_file: TextIO,
    replacements: dict[str, str],
) -> None:
    """Substitute template placeholders in a file.

    Args:
      input_file: The input template file to be processed.
      output_file: That file to which the processed text is written.
      replacements: A dictionary of key => value pairs to replace in input_file.
    """
    for line_num, line in enumerate(input_file, start=1):
        template = string.Template(line)
        try:
            result = template.substitute(replacements)
        except KeyError as err:
            raise SubstitutionError(f"Invalid key on line {line_num}: {err}")
        except ValueError:
            # The ValueError contains "on line 1" so just ignore it.
            raise SubstitutionError(f"Invalid placeholder on line {line_num}")
        output_file.write(result)


def main():
    args = _parse_args()

    # Load all of the status files, merging them into a single dictionary.
    replacements: dict[str, str] = {}
    for status_file in args.status_files:
        with status_file:
            replacements.update(_load_status_file(status_file))

    # Perform substitution on the template input file.
    try:
        with args.template as input_file:
            _substitute_file(input_file, args.out, replacements)
    except SubstitutionError as err:
        status_filenames = ", ".join(repr(f.name) for f in args.status_files)
        raise SystemExit(
            f"Error substituting workspace status in {args.template.name!r}:\n"
            f"  {err}\n"
            f"  Using status files: {status_filenames}"
        )


if __name__ == "__main__":
    main()
