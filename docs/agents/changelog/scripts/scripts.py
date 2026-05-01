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
"""Entrypoint for the changelog workflow scripts."""

import argparse

import start_command
import next_command
import end_command


def parse_args() -> argparse.Namespace:
    """Parses and returns the CLI args."""
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    for cmd in ["next", "start", "end"]:
        cmd_parser = subparsers.add_parser(cmd)
        cmd_parser.add_argument(
            "--year", type=int, required=True, help="Year in YYYY format"
        )
        cmd_parser.add_argument(
            "--month", type=int, required=True, help="Month in MM format"
        )
        if cmd == "start":
            cmd_parser.add_argument(
                "--overwrite",
                action="store_true",
                help="Overwrite in-progress work if month/year differ",
            )

    return parser.parse_args()


def main() -> None:
    """CLI entrypoint."""
    args = parse_args()
    if args.command == "start":
        start_command.start_command(args)
    elif args.command == "next":
        next_command.next_command(args)
    elif args.command == "end":
        end_command.end_command(args)


if __name__ == "__main__":
    main()
