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
"""snapshot handler"""

import dataclasses
import functools
import logging
from io import StringIO

from pw_log import log_decoder
from pw_snapshot import processor
from pw_snapshot_metadata import metadata
from pw_snapshot_protos import snapshot_pb2
from pw_symbolizer import Symbolizer, LlvmSymbolizer
from pw_tokenizer import detokenize, elf_reader

_LOG = logging.getLogger(__package__)


def _parse_snapshot(serialized_snapshot: bytes) -> snapshot_pb2.Snapshot:
    """Parse the serialized snapshot as a Snapshot proto."""
    return snapshot_pb2.Snapshot.FromString(serialized_snapshot)


def _process_logs(
    detokenizer: detokenize.Detokenizer | None, snapshot: snapshot_pb2.Snapshot
) -> str:
    """Returns the logs from the snapshot."""
    if not snapshot.logs:
        return "No captured logs\n"

    output: list[str] = [
        "Device Logs: ",
    ]

    decoded_log_stream = StringIO()
    handler = logging.StreamHandler(stream=decoded_log_stream)

    string_logger = logging.getLogger('crash_snapshot_logs')
    string_logger.level = logging.DEBUG
    # only send logs to o StringIO handler
    string_logger.propagate = False
    string_logger.addHandler(handler)

    # LogStreamDecoder requires a decoded_log_handler, but it never actually
    # is invoked here, as we manually iterate the logs and call
    # parse_log_entry_proto directly.
    # pylint: disable=unused-argument
    def nop_log_handler(log: log_decoder.Log) -> None:
        return

    # pylint: enable=unused-argument

    decoder = log_decoder.LogStreamDecoder(
        decoded_log_handler=nop_log_handler,
        detokenizer=detokenizer,
        source_name='RpcDevice',
    )
    for log_entry in snapshot.logs:
        parsed_log = decoder.parse_log_entry_proto(log_entry)
        log_decoder.log_decoded_log(parsed_log, string_logger)

    output.append(decoded_log_stream.getvalue())

    return "\n".join(output)


def _snapshot_symbolizer_matcher(
    detokenizer: detokenize.Detokenizer | None,
    # pylint: disable=unused-argument
    snapshot: snapshot_pb2.Snapshot,
) -> Symbolizer:
    cpu_arch = metadata.MetadataProcessor(snapshot.metadata).cpu_arch()
    if isinstance(detokenizer, detokenize.AutoUpdatingDetokenizer):
        if len(detokenizer.paths) > 1:
            _LOG.info(
                'More than one token database file.  The first elf '
                'file in the list will be used for symbolization.'
            )

        for database_path in detokenizer.paths:
            path = database_path.path
            if elf_reader.compatible_file(path):
                _LOG.debug('Using %s for symbolization', path)
                return LlvmSymbolizer(path, cpu_arch=cpu_arch)

    _LOG.warning(
        'No elf token database specified.  Crash report will not '
        'have any symbols.'
    )
    return LlvmSymbolizer(cpu_arch=cpu_arch)


@dataclasses.dataclass
class _CustomProcessor:
    """Snapshot processor callback handler."""

    detokenizer: detokenize.Detokenizer | None

    def __call__(self, serialized_snapshot: bytes) -> str:
        snapshot = _parse_snapshot(serialized_snapshot)

        output: list[str] = []
        output.append(_process_logs(self.detokenizer, snapshot))

        return "\n".join(output)


def decode_snapshot(
    detokenizer: detokenize.Detokenizer | None,
    serialized_snapshot: bytes,
) -> str:
    """Decodes the serialized snapshot

    Args:
      detokenizer: String detokenizer.
      serialized_snapshot: Contents of the snapshot dump file.

    Returns:
      The decoded snapshot as a string.
    """
    custom_processor = _CustomProcessor(
        detokenizer,
    )

    return processor.process_snapshots(
        serialized_snapshot=serialized_snapshot,
        detokenizer=detokenizer,
        symbolizer_matcher=functools.partial(
            _snapshot_symbolizer_matcher, detokenizer
        ),
        user_processing_callback=custom_processor,
    )
