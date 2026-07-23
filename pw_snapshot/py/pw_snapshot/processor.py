# Copyright 2021 The Pigweed Authors
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
"""Tool for processing and outputting Snapshot protos as text"""

import argparse
import functools
import json
import logging
import sys
import textwrap
from pathlib import Path
from typing import BinaryIO, TextIO, Callable, Sequence

import pw_tokenizer
from pw_tokenizer.proto import decode_optionally_tokenized
import pw_cpu_exception_cortex_m
import pw_cpu_exception_risc_v
import pw_build_info.build_id
from pw_log.log_decoder import (
    LogStreamDecoder,
    timestamp_parser_ns_since_boot,
)
from pw_log.proto import log_pb2
from pw_metric.metric_parser import metrics_to_dict
from pw_snapshot_metadata import metadata
from pw_snapshot_metadata_proto import snapshot_metadata_pb2
from pw_snapshot_protos import snapshot_pb2
from pw_symbolizer import LlvmSymbolizer, Symbolizer
from pw_thread import thread_analyzer
from pw_chrono import timestamp_analyzer

_LOG = logging.getLogger('snapshot_processor')

_BRANDING = """
        ____ _       __    _____ _   _____    ____  _____ __  ______  ______
       / __ \\ |     / /   / ___// | / /   |  / __ \\/ ___// / / / __ \\/_  __/
      / /_/ / | /| / /    \\__ \\/  |/ / /| | / /_/ /\\__ \\/ /_/ / / / / / /
     / ____/| |/ |/ /    ___/ / /|  / ___ |/ ____/___/ / __  / /_/ / / /
    /_/     |__/|__/____/____/_/ |_/_/  |_/_/    /____/_/ /_/\\____/ /_/
                  /_____/

"""

# Deprecated, use SymbolizerMatcher. Will be removed shortly.
ElfMatcher = Callable[[snapshot_pb2.Snapshot], Path | None]

# Symbolizers are useful for turning addresses into source code locations and
# function names. As a single snapshot may contain embedded snapshots from
# multiple devices, there's a need to match ELF files to the correct snapshot to
# correctly symbolize addresses.
#
# A SymbolizerMatcher is a function that takes a snapshot and investigates its
# metadata (often build ID, device name, or the version string) to determine
# whether a Symbolizer may be loaded with a suitable ELF file for symbolization.
SymbolizerMatcher = Callable[[snapshot_pb2.Snapshot], Symbolizer]

LogProcessor = Callable[
    [Sequence[log_pb2.LogEntry], pw_tokenizer.Detokenizer | None], str
]


def _default_log_processor(
    logs: Sequence[log_pb2.LogEntry],
    detokenizer: pw_tokenizer.Detokenizer | None,
) -> str:
    assert logs

    log_decoder = LogStreamDecoder(
        detokenizer=detokenizer,
        timestamp_parser=timestamp_parser_ns_since_boot,
    )

    output = ["Logs:"]
    for log_msg in logs:
        log = log_decoder.parse_log_entry_proto(log_msg)
        output.append(f"  {log}")

    return "\n".join(output)


def process_snapshot(
    serialized_snapshot: bytes,
    detokenizer: pw_tokenizer.Detokenizer | None = None,
    elf_matcher: ElfMatcher | None = None,
    symbolizer_matcher: SymbolizerMatcher | None = None,
    llvm_symbolizer_binary: Path | None = None,
    thread_processing_callback: Callable[[bytes], str] | None = None,
    process_logs: LogProcessor | None = _default_log_processor,
    memory_region_processor: (
        Callable[[snapshot_pb2.MemoryRegion], str] | None
    ) = None,
) -> str:
    """Processes a single snapshot."""

    output = [_BRANDING]

    snapshot = snapshot_pb2.Snapshot.FromString(serialized_snapshot)

    # Open a symbolizer.
    if symbolizer_matcher is not None:
        symbolizer = symbolizer_matcher(snapshot)
    elif elf_matcher is not None:
        symbolizer = LlvmSymbolizer(
            elf_matcher(snapshot),
            llvm_symbolizer_binary=llvm_symbolizer_binary,
            cpu_arch=metadata.MetadataProcessor(snapshot.metadata).cpu_arch(),
        )
    else:
        symbolizer = LlvmSymbolizer(
            llvm_symbolizer_binary=llvm_symbolizer_binary,
            cpu_arch=metadata.MetadataProcessor(snapshot.metadata).cpu_arch(),
        )

    captured_metadata = metadata.process_snapshot(
        serialized_snapshot, detokenizer
    )
    if captured_metadata:
        output.append(captured_metadata)

    # Logs
    if snapshot.logs and process_logs:
        output.append(process_logs(snapshot.logs, detokenizer))
        output.append("")

    # Create MetadataProcessor
    snapshot_metadata = snapshot_metadata_pb2.SnapshotBasicInfo.FromString(
        serialized_snapshot
    )
    metadata_processor = metadata.MetadataProcessor(
        snapshot_metadata.metadata, detokenizer
    )

    # Check which CPU architecture to process the snapshot with.
    if metadata_processor.cpu_arch().startswith("RV"):
        risc_v_cpu_state = pw_cpu_exception_risc_v.process_snapshot(
            serialized_snapshot, symbolizer
        )
        if risc_v_cpu_state:
            output.append(risc_v_cpu_state)
    elif metadata_processor.cpu_arch().startswith("ARM"):
        cortex_m_cpu_state = pw_cpu_exception_cortex_m.process_snapshot(
            serialized_snapshot, symbolizer
        )
        if cortex_m_cpu_state:
            output.append(cortex_m_cpu_state)
    else:
        _LOG.warning(
            "Unhandled CPU architecture %s", metadata_processor.cpu_arch()
        )

    thread_info = thread_analyzer.process_snapshot(
        serialized_snapshot, detokenizer, symbolizer, thread_processing_callback
    )

    if thread_info:
        output.append(thread_info)

    timestamp_info = timestamp_analyzer.process_snapshot(
        serialized_snapshot, detokenizer
    )

    if timestamp_info:
        output.append(timestamp_info)

    # Metrics
    if snapshot.metrics:
        metrics_dict = metrics_to_dict(snapshot.metrics, detokenizer)
        output.append("Metrics:")
        output.append(json.dumps(metrics_dict, indent="  ", sort_keys=True))
        output.append("")

    # Memory Regions
    if snapshot.memory_regions:
        output.append("Memory Regions:")
        for region in snapshot.memory_regions:
            region_name = decode_optionally_tokenized(detokenizer, region.name)
            name = f" ({region_name})" if region_name else ""
            output.append(
                f"  {region.address:08x}{name} ({len(region.data)} bytes)"
            )
            if memory_region_processor is not None:
                region_output = memory_region_processor(region)
                if region_output:
                    output.append(textwrap.indent(region_output, "    "))
        output.append("")

    # Check and emit the number of related snapshots embedded in this snapshot.
    if snapshot.related_snapshots:
        snapshot_count = len(snapshot.related_snapshots)
        plural = 's' if snapshot_count > 1 else ''
        output.append(
            f'This snapshot contains {snapshot_count} related snapshot{plural}'
        )
        output.append('')

    return '\n'.join(output)


def process_snapshots(
    serialized_snapshot: bytes,
    detokenizer: pw_tokenizer.Detokenizer | None = None,
    elf_matcher: ElfMatcher | None = None,
    user_processing_callback: Callable[[bytes], str] | None = None,
    symbolizer_matcher: SymbolizerMatcher | None = None,
    thread_processing_callback: (
        Callable[[snapshot_pb2.Snapshot, bytes], str] | None
    ) = None,
    process_logs: LogProcessor | None = _default_log_processor,
    memory_region_processor: (
        Callable[[snapshot_pb2.MemoryRegion], str] | None
    ) = None,
) -> str:
    """Processes a snapshot that may have multiple embedded snapshots."""
    output = []
    # Process the top-level snapshot.
    snapshot = snapshot_pb2.Snapshot.FromString(serialized_snapshot)

    callback: Callable[[bytes], str] | None = None
    if thread_processing_callback:
        callback = functools.partial(thread_processing_callback, snapshot)

    output.append(
        process_snapshot(
            serialized_snapshot=serialized_snapshot,
            detokenizer=detokenizer,
            elf_matcher=elf_matcher,
            symbolizer_matcher=symbolizer_matcher,
            thread_processing_callback=callback,
            process_logs=process_logs,
            memory_region_processor=memory_region_processor,
        )
    )

    # If the user provided a custom processing callback, call it on each
    # snapshot.
    if user_processing_callback is not None:
        output.append(user_processing_callback(serialized_snapshot))

    # Process any related snapshots that were embedded in this one.
    for nested_snapshot in snapshot.related_snapshots:
        output.append('\n[' + '=' * 78 + ']\n')
        output.append(
            str(
                process_snapshots(
                    serialized_snapshot=nested_snapshot.SerializeToString(),
                    detokenizer=detokenizer,
                    elf_matcher=elf_matcher,
                    user_processing_callback=user_processing_callback,
                    symbolizer_matcher=symbolizer_matcher,
                    thread_processing_callback=thread_processing_callback,
                    process_logs=process_logs,
                    memory_region_processor=memory_region_processor,
                )
            )
        )

    return '\n'.join(output)


def _snapshot_symbolizer_matcher(
    artifacts_dir: Path, snapshot: snapshot_pb2.Snapshot
) -> LlvmSymbolizer:
    matching_elf: Path | None = pw_build_info.build_id.find_matching_elf(
        snapshot.metadata.software_build_uuid, artifacts_dir
    )
    if not matching_elf:
        _LOG.error(
            'Error: No matching ELF found for GNU build ID %s.',
            snapshot.metadata.software_build_uuid.hex(),
        )
    return LlvmSymbolizer(
        matching_elf,
        cpu_arch=metadata.MetadataProcessor(snapshot.metadata).cpu_arch(),
    )


def _load_and_dump_snapshots(
    in_file: BinaryIO,
    out_file: TextIO,
    token_db: str | None,
    artifacts_dir: Path | None,
):
    detokenizer = None
    if token_db:
        detokenizer = pw_tokenizer.Detokenizer(token_db)
    symbolizer_matcher: SymbolizerMatcher | None = None
    if artifacts_dir:
        symbolizer_matcher = functools.partial(
            _snapshot_symbolizer_matcher, artifacts_dir
        )
    out_file.write(
        process_snapshots(
            serialized_snapshot=in_file.read(),
            detokenizer=detokenizer,
            symbolizer_matcher=symbolizer_matcher,
        )
    )


def _parse_args():
    parser = argparse.ArgumentParser(description='Decode Pigweed snapshots')
    parser.add_argument(
        'in_file', type=argparse.FileType('rb'), help='Binary snapshot file'
    )
    parser.add_argument(
        '--out-file',
        '-o',
        default='-',
        type=argparse.FileType('w'),
        help='File to output decoded snapshots to. Defaults to stdout.',
    )
    parser.add_argument(
        '--token-db',
        help='Token database or ELF file to use for detokenization.',
    )
    parser.add_argument(
        '--artifacts-dir',
        type=Path,
        help=(
            'Directory to recursively search for matching ELF files to use '
            'for symbolization.'
        ),
    )
    return parser.parse_args()


if __name__ == '__main__':
    logging.basicConfig(format='%(message)s', level=logging.INFO)
    _load_and_dump_snapshots(**vars(_parse_args()))
    sys.exit(0)
