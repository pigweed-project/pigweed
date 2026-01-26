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
"""
Entry point for the Bazel and GN bloat report builds.
"""

import argparse
import json
from pathlib import Path
import platform
import sys

from typing import List

import pw_cli.log

from pw_bloat.bloat import ReportTarget, diff_report, single_target_report
from pw_bloat.label_output import RstOutput


def _parse_args() -> argparse.Namespace:
    """Parses the script's arguments."""

    parser = argparse.ArgumentParser('Generate a size report card for binaries')
    parser.add_argument(
        '--target-json',
        type=str,
        help='File path to json of size report targets',
    )
    parser.add_argument(
        '--collect-fragments-to',
        type=Path,
        help='Path of output file to which to collect input fragments',
    )
    parser.add_argument(
        '--single-report',
        action='store_true',
        help='Determine if calling single size report',
    )
    parser.add_argument(
        '--generate-rst-fragment',
        action='store_true',
        help=(
            'If set, outputs only a fragment of an RST table which can be '
            'concatenated with others into a full table'
        ),
    )
    parser.add_argument(
        '--json-key-prefix',
        type=str,
        help='Prefix for json keys in size report, default = target name',
        default=None,
    )
    parser.add_argument(
        '--full-json-summary',
        action='store_true',
        help='Include all levels of data sources in json binary report',
    )
    parser.add_argument(
        '--ignore-unused-labels',
        action='store_true',
        help='Do not include labels with size equal to zero in report',
    )
    parser.add_argument(
        'fragment',
        nargs='*',
        type=Path,
        help='RST fragments to combine (with --collect-fragments-to)',
    )

    return parser.parse_args()


def _combine_fragments(output: Path, fragments: List[Path]) -> None:
    table = RstOutput.create_table_heading()

    for fragment in fragments:
        table += f'\n{fragment.read_text()}'

    output.write_text(table)


def main() -> int:
    """Program entry point."""

    args = _parse_args()

    if args.collect_fragments_to is not None:
        if len(args.fragment) == 0:
            print(
                'Must provide at least one fragment for --collect-fragments-to',
                file=sys.stderr,
            )
            return 1

        _combine_fragments(args.collect_fragments_to, args.fragment)
        return 0

    if not args.target_json:
        print(
            'Either --target-json or --collect-fragments-to must be provided',
            file=sys.stderr,
        )
        return 1

    extra_args = ['--tsv']
    default_data_sources = ['segment_names', 'symbols']

    json_file = open(args.target_json)
    json_args = json.load(json_file)
    json_key_prefix = args.json_key_prefix

    output_directory = json_args.get('out_dir', '.')
    output_filename = json_args['target_name']

    match sys.platform, platform.machine():
        case ('linux' | 'darwin', 'x86_64'):
            platform_supported = True
        case _:
            platform_supported = False

    if not platform_supported:
        out_file = Path(output_directory) / output_filename
        error_message = (
            'Size reports are not supported on this platform '
            '(https://pwbug.dev/397444383)'
        )
        if args.generate_rst_fragment:
            lines = [
                '   * - Error',
                f'     - {error_message}',
                '     - N/A',
            ]
        else:
            lines = [
                '.. note::',
                '',
                f'   {error_message}',
            ]
        out_file.write_text('\n'.join(lines))
        return 0

    targets = []
    for binary in json_args['binaries']:
        targets.append(
            ReportTarget(
                target_binary=binary['target'],
                base_binary=binary.get('base'),
                bloaty_config=binary.get('bloaty_config'),
                label=binary.get('label', ''),
                source_filter=binary.get('source_filter'),
                data_sources=binary.get('data_sources', default_data_sources),
            )
        )

    try:
        if args.single_report:
            target = targets[0]

            # Use target binary name as json key prefix if none given
            if not json_key_prefix:
                json_key_prefix = target.target_binary

            single_target_report(
                target,
                output_filename,
                output_directory,
                extra_args,
                json_key_prefix,
                args.full_json_summary,
                args.ignore_unused_labels,
            )
        else:
            diff_report(
                targets,
                output_filename,
                output_directory,
                default_data_sources,
                extra_args,
                fragment=args.generate_rst_fragment,
            )
    except Exception as e:  # pylint:disable=broad-exception-caught
        print(e, file=sys.stdout)
        return 1

    return 0


if __name__ == '__main__':
    pw_cli.log.install()
    sys.exit(main())
