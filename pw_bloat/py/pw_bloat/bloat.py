# Copyright 2022 The Pigweed Authors
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
bloat is a script which generates a size report card for binary files.
"""

from dataclasses import dataclass
import json
import logging
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Iterable

from pw_bloat.bloaty_config import generate_bloaty_config
from pw_bloat.label import DataSourceMap
from pw_bloat.label_output import (
    BloatTableOutput,
    LineCharset,
    RstOutput,
    AsciiCharset,
)

_LOG = logging.getLogger(__name__)

MAX_COL_WIDTH = 50
BINARY_SIZES_EXTENSION = '.binary_sizes.json'


def run_bloaty(
    filename: str,
    config: str,
    base_file: str | None = None,
    data_sources: Iterable[str] = (),
    extra_args: Iterable[str] = (),
) -> bytes:
    """Executes a Bloaty size report on some binary file(s).

    Args:
        filename: Path to the binary.
        config: Path to Bloaty config file.
        base_file: Path to a base binary. If provided, a size diff is performed.
        data_sources: List of Bloaty data sources for the report.
        extra_args: Additional command-line arguments to pass to Bloaty.

    Returns:
        Binary output of the Bloaty invocation.

    Raises:
        subprocess.CalledProcessError: The Bloaty invocation failed.
    """

    try:
        # If running from within a Bazel context, find the Bloaty executable
        # within the project's runfiles.
        # pylint: disable=import-outside-toplevel
        from python.runfiles import runfiles  # type: ignore

        r = runfiles.Create()
        assert r is not None
        bloaty_path = r.Rlocation("bloaty/bin/bloaty", r.CurrentRepository())
        if bloaty_path is None or not os.path.exists(bloaty_path):
            bloaty_path = r.Rlocation("bloaty/bloaty", r.CurrentRepository())
    except ImportError:
        # Outside of Bazel, use Bloaty from the system path.
        default_bloaty = 'bloaty'
        bloaty_path = os.getenv('BLOATY_PATH', default_bloaty)

    assert bloaty_path is not None
    cmd = [
        bloaty_path,
        '-c',
        config,
        '-d',
        ','.join(data_sources),
        '--domain',
        'vm',
        '-n',
        '0',
        filename,
        *extra_args,
    ]

    if base_file is not None:
        cmd.extend(['--', base_file])

    return subprocess.check_output(cmd)


def basic_size_report(
    elf: Path,
    bloaty_config: Path,
    data_sources: Iterable[str] = (),
    extra_args: Iterable[str] = (),
) -> Iterable[str]:
    """Runs a size report on an ELF file using the specified Bloaty config.

    Arguments:
        elf: The ELF binary on which to run.
        bloaty_config: Path to a Bloaty configuration file.
        data_sources: Hierarchical data sources to display.
        extra_args: Additional command line arguments forwarded to bloaty.

    Returns:
        The bloaty TSV output detailing the size report.
    """
    return (
        run_bloaty(
            str(elf.resolve()),
            str(bloaty_config.resolve()),
            data_sources=data_sources,
            extra_args=extra_args,
        )
        .decode('utf-8')
        .splitlines()
    )


class NoMemoryRegions(Exception):
    """Exception raised if an ELF does not define any memory region symbols."""

    def __init__(self, elf: Path):
        super().__init__(f'ELF {elf} does not define memory region symbols')
        self.elf = elf


def memory_regions_size_report(
    elf: Path,
    data_sources: Iterable[str] = (),
    extra_args: Iterable[str] = (),
) -> Iterable[str]:
    """Runs a size report on an ELF file using pw_bloat memory region symbols.

    Arguments:
        elf: The ELF binary on which to run.
        data_sources: Hierarchical data sources to display.
        extra_args: Additional command line arguments forwarded to bloaty.

    Returns:
        The bloaty TSV output detailing the size report.

    Raises:
        NoMemoryRegions: The ELF does not define memory region symbols.
    """
    with tempfile.NamedTemporaryFile() as bloaty_config:
        with (
            open(elf.resolve(), "rb") as infile,
            open(bloaty_config.name, "w") as outfile,
        ):
            result = generate_bloaty_config(
                infile,
                enable_memoryregions=True,
                enable_utilization=False,
                out_file=outfile,
            )

            if not result.has_memoryregions:
                raise NoMemoryRegions(elf)

        return (
            run_bloaty(
                str(elf.resolve()),
                bloaty_config.name,
                data_sources=data_sources,
                extra_args=extra_args,
            )
            .decode('utf-8')
            .splitlines()
        )


def write_file(filename: str, contents: str, out_dir_file: str) -> None:
    path = os.path.join(out_dir_file, filename)
    with open(path, 'w') as output_file:
        output_file.write(contents)
    _LOG.debug('Output written to %s', path)


def create_binary_sizes_json(
    key_prefix: str,
    data_source_map: DataSourceMap,
    full_json: bool,
    ignore_unused_labels: bool,
) -> str:
    """Creates a binary_sizes.json file content from a list of labels.

    Args:
      key_prefix: Prefix for the json keys.
      data_source_map: Hierarchical structure containing size of sources.
      full_json: Report contains all sources, otherwise just top level.
      ignore_unused_labels: Doesn't include labels of size zero in json.

    Returns:
      A string of content to write to binary_sizes.json file.
    """
    json_content = {}
    if full_json:
        *ds_parents, last = data_source_map.get_ds_names()
        for label in data_source_map.labels():
            key = f'{key_prefix}.'
            for ds_parent, label_parent in zip(ds_parents, label.parents):
                key += f'{ds_parent}.{label_parent}.'
            key += f'{last}.{label.name}'
            if label.size != 0 or not ignore_unused_labels:
                json_content[key] = label.size
    else:
        for label in data_source_map.labels(ds_index=0):
            if label.size != 0 or not ignore_unused_labels:
                json_content[f'{key_prefix}.{label.name}'] = label.size
    return json.dumps(json_content, sort_keys=True, indent=2)


@dataclass
class ReportTarget:
    target_binary: str
    base_binary: str | None
    bloaty_config: str
    label: str
    source_filter: str | None
    data_sources: list[str]


# Legacy wrapper for single_target_report.
def single_target_output(
    target: str,
    bloaty_config: str,
    target_out_file: str,
    out_dir: str,
    data_sources: Iterable[str],
    extra_args: Iterable[str],
    json_key_prefix: str,
    full_json: bool,
    ignore_unused_labels: bool,
) -> int:
    """DEPRECATED - use single_target_report instead."""
    report_target = ReportTarget(
        target_binary=target,
        base_binary=None,
        bloaty_config=bloaty_config,
        label='',
        source_filter=None,
        data_sources=list(data_sources),
    )
    try:
        single_target_report(
            report_target,
            target_out_file,
            out_dir,
            extra_args,
            json_key_prefix,
            full_json,
            ignore_unused_labels,
        )
        return 0
    except Exception:  # pylint:disable=broad-exception-caught
        return 1


def single_target_report(
    target: ReportTarget,
    target_out_file: str,
    out_dir: str,
    extra_args: Iterable[str],
    json_key_prefix: str,
    full_json: bool,
    ignore_unused_labels: bool,
) -> None:
    """Generates size report for a single target.

    Args:
      target: Information about the ELF binary on which to run.
      target_out_file: Output file name for the generated reports.
      out_dir: Path to write size reports to.
      extra_args: Additional command-line arguments to pass to Bloaty.
      json_key_prefix: Prefix for the json keys, uses target name by default.
      full_json: Json report contains all hierarchical data source totals.
      ignore_unused_labels: If True, filters out zero-size labels in the output.
        Zero on success.

    Raises:
        subprocess.CalledProcessError: The Bloaty invocation failed.
    """
    extra_args = list(extra_args)
    if target.source_filter:
        extra_args.extend(('--source-filter', target.source_filter))

    try:
        single_output = run_bloaty(
            target.target_binary,
            target.bloaty_config,
            data_sources=target.data_sources,
            extra_args=extra_args,
        )

    except subprocess.CalledProcessError:
        _LOG.error(
            '%s: failed to run size report on %s',
            sys.argv[0],
            target.target_binary,
        )
        raise

    single_tsv = single_output.decode().splitlines()
    single_report = BloatTableOutput(
        DataSourceMap.from_bloaty_tsv(single_tsv), MAX_COL_WIDTH, LineCharset
    )

    data_source_map = DataSourceMap.from_bloaty_tsv(single_tsv)
    rst_single_report = BloatTableOutput(
        data_source_map,
        MAX_COL_WIDTH,
        AsciiCharset,
        True,
    )

    single_report_table = single_report.create_table()

    # Generates contents for summary printed to binary_sizes.json
    binary_json_content = create_binary_sizes_json(
        json_key_prefix, data_source_map, full_json, ignore_unused_labels
    )

    write_file(target_out_file, rst_single_report.create_table(), out_dir)
    write_file(f'{target_out_file}.txt', single_report_table, out_dir)
    write_file(
        f'{target_out_file}{BINARY_SIZES_EXTENSION}',
        binary_json_content,
        out_dir,
    )


def diff_report(
    targets: list[ReportTarget],
    target_out_file: str,
    out_dir: str,
    default_data_sources: Iterable[str],
    extra_args: Iterable[str],
    fragment: bool = False,
) -> None:
    """Generates a size report diff table containing one or more diff reports.

    Args:
        targets: List of diff comparisons to run. Each target must have both
            a base_binary and a target_binary.
        target_out_file: Output file name for the generated reports.
        out_dir: Directory to which to write output files.
        default_data_sources: Bloaty data sources to display in the report,
            unless overridden by individual targets.
        extra_args: Additional command line arguments to forward to Bloaty.

    Raises:
        ValueError: List of diff targets is malformed.
        subprocess.CalledProcessError: The Bloaty invocation failed.
    """
    default_data_sources = list(default_data_sources)
    extra_args = list(extra_args)

    diff_report_output = ''
    rst_diff_report = ''
    for diff_target in targets:
        curr_extra_args = extra_args.copy()
        data_sources = default_data_sources

        if diff_target.source_filter is not None:
            curr_extra_args.extend(
                ['--source-filter', diff_target.source_filter]
            )

        if diff_target.data_sources:
            data_sources = diff_target.data_sources

        if diff_target.base_binary is None:
            raise ValueError('Binaries to diff_report must have a diff base')

        try:
            single_output_base = run_bloaty(
                diff_target.base_binary,
                diff_target.bloaty_config,
                data_sources=data_sources,
                extra_args=curr_extra_args,
            )

        except subprocess.CalledProcessError:
            _LOG.error(
                '%s: failed to run base size report on %s',
                sys.argv[0],
                diff_target.base_binary,
            )
            raise

        try:
            single_output_target = run_bloaty(
                diff_target.target_binary,
                diff_target.bloaty_config,
                data_sources=data_sources,
                extra_args=curr_extra_args,
            )

        except subprocess.CalledProcessError:
            _LOG.error(
                '%s: failed to run target size report on %s',
                sys.argv[0],
                diff_target.target_binary,
            )
            raise

        if not single_output_target or not single_output_base:
            continue

        base_dsm = DataSourceMap.from_bloaty_tsv(
            single_output_base.decode().splitlines()
        )
        target_dsm = DataSourceMap.from_bloaty_tsv(
            single_output_target.decode().splitlines()
        )
        diff_dsm = target_dsm.diff(base_dsm)

        diff_report_output += BloatTableOutput(
            diff_dsm,
            MAX_COL_WIDTH,
            LineCharset,
            diff_label=diff_target.label,
        ).create_table()

        curr_rst_report = RstOutput(diff_dsm, diff_target.label)
        if not fragment and rst_diff_report == '':
            rst_diff_report = curr_rst_report.create_table()
        else:
            rst_diff_report += f"{curr_rst_report.add_report_row()}\n"

    if not fragment:
        print(diff_report_output)
        write_file(
            f"{target_out_file}.txt",
            diff_report_output,
            out_dir,
        )

    write_file(
        target_out_file,
        rst_diff_report,
        out_dir,
    )
