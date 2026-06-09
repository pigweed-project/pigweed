#!/usr/bin/env python3
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
r"""

Trace module which creates trace files from a list of trace events.

This is a work in progress, future work will look to add:
    - Config options to customize output.
    - A method of providing custom data formatters.
    - Perfetto support.
"""
from enum import Enum
import json
import logging
import struct
from typing import Any, Callable, Dict, Iterable, NamedTuple, TypeAlias

_LOG = logging.getLogger('pw_trace')
_ORDERING_CHARS = ("@", "=", "<", ">", "!")
_PLUGIN_DATA_FORMAT_HANDLERS = {}


class TraceType(Enum):
    INVALID = 0
    INSTANTANEOUS = 1
    INSTANTANEOUS_GROUP = 2
    ASYNC_START = 3
    ASYNC_STEP = 4
    ASYNC_END = 5
    DURATION_START = 6
    DURATION_END = 7
    DURATION_GROUP_START = 8
    DURATION_GROUP_END = 9

    # TODO(hepler): Remove these aliases for the original style-incompliant
    #     names when users have migrated.
    DurationStart = 6  # pylint: disable=invalid-name
    DurationEnd = 7  # pylint: disable=invalid-name


class TraceEvent(NamedTuple):
    event_type: TraceType
    module: str
    label: str
    timestamp_us: float
    group: str = ""
    trace_id: int = 0
    flags: int = 0
    has_data: bool = False
    data_fmt: str = ""
    data: bytes = b''


# A Callable that processes a plugin data format.
# - TraceEvent: The trace event containing data to be decoded.
# - dict[str, Any]: The line dictionary that the handler can modify
#   in place (e.g. modifying the name, tid, scope, or adding to args).
DataFormatHandler: TypeAlias = Callable[[TraceEvent, dict[str, Any]], None]


def register_plugin_data_format_handler(
    plugin_fmt: str, handler: DataFormatHandler
) -> None:
    """Registers a handler for a plugin data format string.

    Args:
      plugin_fmt: The data format string.
      handler: The callable DataFormatHandler to process the trace format.
    """
    if plugin_fmt in _PLUGIN_DATA_FORMAT_HANDLERS:
        _LOG.warning(
            "Plugin data format handler for %s already registered. "
            "Overriding previous handler.",
            plugin_fmt,
        )
    _PLUGIN_DATA_FORMAT_HANDLERS[plugin_fmt] = handler


def clear_plugin_data_format_handlers() -> None:
    """Clears all registered plugin data format handlers."""
    _PLUGIN_DATA_FORMAT_HANDLERS.clear()


def event_has_trace_id(event_type):
    return event_type in {
        "PW_TRACE_EVENT_TYPE_ASYNC_START",
        "PW_TRACE_EVENT_TYPE_ASYNC_STEP",
        "PW_TRACE_EVENT_TYPE_ASYNC_END",
    }


def decode_struct_fmt_args(event):
    """Decodes the trace's event data for struct-formatted data"""
    args = {}
    # We assume all data is packed, little-endian ordering if not specified.
    struct_fmt = event.data_fmt[len("@pw_py_struct_fmt:") :]
    if not struct_fmt.startswith(_ORDERING_CHARS):
        struct_fmt = "<" + struct_fmt
    try:
        # Assert is needed in case the buffer is larger than expected.
        assert struct.calcsize(struct_fmt) == len(event.data)
        items = struct.unpack_from(struct_fmt, event.data)
        for i, item in enumerate(items):
            # Try to decode the item in case it is a byte array (string) since
            # the JSON lib cannot serialize byte arrays.
            try:
                args["data_" + str(i)] = item.decode()
            except (UnicodeDecodeError, AttributeError):
                args["data_" + str(i)] = item
    except (AssertionError, struct.error):
        args["error"] = (
            f"Mismatched struct/data format {event.data_fmt} "
            f"expected data len {struct.calcsize(struct_fmt)} "
            f"data {event.data.hex()} "
            f"data len {len(event.data)}"
        )
    return args


def decode_map_fmt_args(event):
    """Decodes the trace's event data for map-formatted data"""
    args = {}
    fmt = event.data_fmt[len("@pw_py_map_fmt:") :]

    # We assume all data is packed, little-endian ordering if not specified.
    if not fmt.startswith(_ORDERING_CHARS):
        fmt = '<' + fmt

    try:
        (fmt_bytes, fmt_list) = fmt.split("{")
        fmt_list = fmt_list.strip("}").split(",")

        names = []
        for pair in fmt_list:
            (name, fmt_char) = (s.strip() for s in pair.split(":"))
            names.append(name)
            fmt_bytes += fmt_char
    except ValueError:
        args["error"] = f"Invalid map format {event.data_fmt}"
    else:
        try:
            # Assert is needed in case the buffer is larger than expected.
            assert struct.calcsize(fmt_bytes) == len(event.data)
            items = struct.unpack_from(fmt_bytes, event.data)
            for i, item in enumerate(items):
                # Try to decode the item in case it is a byte array (string)
                # since the JSON lib cannot serialize byte arrays.
                try:
                    args[names[i]] = item.decode()
                except (UnicodeDecodeError, AttributeError):
                    args[names[i]] = item
        except (AssertionError, struct.error):
            args["error"] = (
                f"Mismatched map/data format {event.data_fmt} "
                f"expected data len {struct.calcsize(fmt_bytes)} "
                f"data {event.data.hex()} "
                f"data len {len(event.data)}"
            )
    return args


def generate_trace_json(events: Iterable[TraceEvent]):
    """Generates a list of JSON lines from provided trace events."""
    json_lines = []
    for event in events:
        if (
            event.module is None
            or event.timestamp_us is None
            or event.event_type is None
            or event.label is None
        ):
            _LOG.error("Invalid sample")
            continue

        line: Dict[str, Any] = {
            "pid": event.module,
            "name": (event.label),
            "ts": event.timestamp_us,
        }
        if event.event_type == TraceType.DURATION_START:
            line["ph"] = "B"
            line["tid"] = event.label
        elif event.event_type == TraceType.DURATION_END:
            line["ph"] = "E"
            line["tid"] = event.label
        elif event.event_type == TraceType.DURATION_GROUP_START:
            line["ph"] = "B"
            line["tid"] = event.group
        elif event.event_type == TraceType.DURATION_GROUP_END:
            line["ph"] = "E"
            line["tid"] = event.group
        elif event.event_type == TraceType.INSTANTANEOUS:
            line["ph"] = "I"
            line["s"] = "p"
        elif event.event_type == TraceType.INSTANTANEOUS_GROUP:
            line["ph"] = "I"
            line["s"] = "t"
            line["tid"] = event.group
        elif event.event_type == TraceType.ASYNC_START:
            line["ph"] = "b"
            line["scope"] = event.group
            line["tid"] = event.group
            line["cat"] = event.module
            line["id"] = event.trace_id
            line["args"] = {"id": line["id"]}
        elif event.event_type == TraceType.ASYNC_STEP:
            line["ph"] = "n"
            line["scope"] = event.group
            line["tid"] = event.group
            line["cat"] = event.module
            line["id"] = event.trace_id
            line["args"] = {"id": line["id"]}
        elif event.event_type == TraceType.ASYNC_END:
            line["ph"] = "e"
            line["scope"] = event.group
            line["tid"] = event.group
            line["cat"] = event.module
            line["id"] = event.trace_id
            line["args"] = {"id": line["id"]}
        else:
            _LOG.error("Unknown event type, skipping")
            continue

        # Handle Data
        if event.has_data:
            if event.data_fmt == "@pw_arg_label":
                line["name"] = event.data.decode("utf-8")
            elif event.data_fmt == "@pw_arg_group":
                line["tid"] = event.data.decode("utf-8")
            elif event.data_fmt == "@pw_arg_counter":
                line["ph"] = "C"
                line["args"] = {
                    line["name"]: int.from_bytes(event.data, "little")
                }
            elif event.data_fmt.startswith("@pw_py_struct_fmt:"):
                line["args"] = decode_struct_fmt_args(event)
            elif event.data_fmt.startswith("@pw_py_map_fmt:"):
                line["args"] = decode_map_fmt_args(event)
            else:
                plugin_key = event.data_fmt.split(':', 1)[0]
                if handler := _PLUGIN_DATA_FORMAT_HANDLERS.get(plugin_key):
                    try:
                        handler(event, line)
                    except ValueError as e:
                        line.setdefault("args", {})[
                            "error"
                        ] = f"Plugin handler {plugin_key} failed: {e}"
                        line["data"] = event.data.hex()
                else:
                    line["args"] = {"data": event.data.hex()}

        # Encode as JSON
        json_lines.append(json.dumps(line))

    return json_lines
