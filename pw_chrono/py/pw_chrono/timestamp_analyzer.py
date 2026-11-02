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
"""Library to analyze timestamps."""

from datetime import datetime, timedelta, timezone, UTC
import logging
import pw_tokenizer
from pw_chrono_protos import chrono_pb2
from pw_tokenizer import proto as proto_detokenizer

_LOG = logging.getLogger(__name__)


def process_snapshot(
    serialized_snapshot: bytes,
    detokenizer: pw_tokenizer.Detokenizer | None = None,
):
    snapshot = chrono_pb2.SnapshotTimestamps.FromString(serialized_snapshot)

    output: list[str] = []
    for timestamp in snapshot.timestamps or []:
        proto_detokenizer.detokenize_fields(
            detokenizer, timestamp.clock_parameters
        )
        try:
            time_info = TimePointInfo(timestamp)
            output.append(f'  {time_info.name_str()}: {time_info}')
        except ValueError:
            _LOG.warning('Failed to decode timestamp:\n%s', str(timestamp))

    if not output:
        return ''
    plural = '' if len(output) == 1 else 's'
    output.insert(0, f'Snapshot capture timestamp{plural}')
    output.append('')
    return '\n'.join(output)


class TimePointInfo:
    """Decodes pw.chrono.TimePoint protos into various representations."""

    def __init__(self, timepoint: chrono_pb2.TimePoint):
        self._timepoint = timepoint
        parameters = self._timepoint.clock_parameters
        if (
            parameters.tick_period_seconds_denominator == 0
            or parameters.tick_period_seconds_numerator == 0
        ):
            raise ValueError('Invalid timepoint')
        self._timepoint = timepoint

    def ticks_per_sec(self) -> float:
        parameters = self._timepoint.clock_parameters
        return (
            parameters.tick_period_seconds_denominator
            / parameters.tick_period_seconds_numerator
        )

    def period_suffix(self) -> str:
        return {
            1.0: 's',
            1000.0: 'ms',
            1_000_000.0: 'us',
            1_000_000_000.0: 'ns',
        }.get(self.ticks_per_sec(), '')

    def duration(self) -> timedelta:
        return timedelta(
            seconds=self._timepoint.timestamp / self.ticks_per_sec()
        )

    def as_unix_time(self, tz: timezone = UTC) -> datetime:
        return datetime.fromtimestamp(self.duration().total_seconds(), tz)

    def tick_count_str(self) -> str:
        return f'{self._timepoint.timestamp} {self.period_suffix()}'.rstrip()

    def __str__(self) -> str:
        epoch_type = self._timepoint.clock_parameters.epoch_type
        if epoch_type == chrono_pb2.EpochType.Enum.TIME_SINCE_BOOT:
            return f'{self.duration()} ({self.tick_count_str()})'
        if epoch_type == chrono_pb2.EpochType.Enum.UTC_WALL_CLOCK:
            return f'{self.as_unix_time()} ({self.tick_count_str()})'
        return self.tick_count_str()

    def name_str(self) -> str:
        parameters = self._timepoint.clock_parameters
        try:
            epoch_str = chrono_pb2.EpochType.Enum.Name(parameters.epoch_type)
        except ValueError:
            epoch_str = str(parameters.epoch_type)

        if parameters.name:
            return f'{parameters.name.decode()} (epoch {epoch_str})'
        if parameters.epoch_type == chrono_pb2.EpochType.Enum.TIME_SINCE_BOOT:
            return 'Time since boot'
        if parameters.epoch_type == chrono_pb2.EpochType.Enum.UTC_WALL_CLOCK:
            return 'UTC time'
        return f'Timestamp (epoch {epoch_str})'
