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
"""CLI string parsing and formatting utilities for daq-cli."""

from __future__ import annotations

import enum
import json
from typing import Any, TypeVar, cast

from pw_daq.common import (
    ActionRef,
    AttributeRef,
    BlockRef,
    DaqError,
    DigitalState,
    EventRef,
    SignalRef,
    Value,
    ValueType,
)
from pw_daq.lib import (
    Action,
    Attribute,
    Block,
    Daq,
    EndpointSpec,
    Event,
    Signal,
)


def parse_block(daq: Daq, spec: str) -> Block:
    """Parses '<block_type><N>' into a bound daq-lib Block."""
    return daq._bind(  # pylint: disable=protected-access
        BlockRef.from_str(spec)
    )


def parse_attribute(daq: Daq, spec: str) -> Attribute[Any]:
    """Parses '<block>.<attribute>' into a bound daq-lib Attribute."""
    return daq._bind(  # pylint: disable=protected-access
        AttributeRef.from_str(spec)
    )


def parse_signal(daq: Daq, spec: str) -> Signal[Any]:
    """Parses '<block>.<signal>' into a bound daq-lib Signal."""
    return daq._bind(  # pylint: disable=protected-access
        SignalRef.from_str(spec)
    )


def parse_action(daq: Daq, spec: str) -> Action[Any, Any]:
    """Parses '<block>.<action>' into a bound daq-lib Action."""
    return daq._bind(  # pylint: disable=protected-access
        ActionRef.from_str(spec)
    )


_ENUM_CLI_ALIASES: dict[type[enum.Enum], dict[str, enum.Enum]] = {
    DigitalState: {
        "high": DigitalState.ACTIVE,
        "true": DigitalState.ACTIVE,
        "1": DigitalState.ACTIVE,
        "on": DigitalState.ACTIVE,
        "low": DigitalState.INACTIVE,
        "false": DigitalState.INACTIVE,
        "0": DigitalState.INACTIVE,
        "off": DigitalState.INACTIVE,
    },
}


def enum_allowed_labels(enum_cls: type[enum.Enum]) -> list[str]:
    """Returns canonical values and any CLI alias names for an Enum class."""
    labels: list[str] = []
    for name, member in enum_cls.__members__.items():
        val_str = str(member.value)
        if val_str not in labels:
            labels.append(val_str)
        alias_str = name.lower()
        if alias_str not in labels:
            labels.append(alias_str)
    for cli_alias in _ENUM_CLI_ALIASES.get(enum_cls, {}):
        if cli_alias not in labels:
            labels.append(cli_alias)
    return labels


_EnumT = TypeVar("_EnumT", bound=enum.Enum)


def parse_enum_member(
    enum_cls: type[_EnumT],
    raw_value: str,
    *,
    context: str,
) -> _EnumT:
    """Parses an enum value by value or CLI alias name (case-insensitive)."""
    normalized = raw_value.strip().lower()
    for name, member in enum_cls.__members__.items():
        if (
            str(member.value).lower() == normalized
            or name.lower() == normalized
        ):
            return cast(_EnumT, member)
    cli_aliases = _ENUM_CLI_ALIASES.get(enum_cls, {})
    if normalized in cli_aliases:
        return cast(_EnumT, cli_aliases[normalized])
    valid_vals = ", ".join(enum_allowed_labels(enum_cls))
    raise DaqError(
        f"Invalid enum value '{raw_value}' for {context}. "
        f"Expected one of: {valid_vals}."
    )


def parse_endpoint_value(
    spec: EndpointSpec,
    raw_value: str | None,
    *,
    context: str,
) -> Value:
    """Parses and validates a CLI string argument against an EndpointSpec."""
    value_type = spec.input_value_type
    if value_type is None:
        if raw_value is not None:
            raise DaqError(
                f"{context} does not accept an input value, got '{raw_value}'."
            )
        return None

    if raw_value is None:
        raise DaqError(
            f"{context} requires an input value of type "
            f"'{value_type.value}'."
        )

    enum_cls = value_type.enum_cls
    if enum_cls is not None:
        parsed: Value = parse_enum_member(enum_cls, raw_value, context=context)
    elif value_type == ValueType.CHAR:
        if len(raw_value) == 1:
            parsed = raw_value
        else:
            stripped = raw_value.strip()
            if stripped.startswith(("0x", "0X")):
                try:
                    code_point = int(stripped, 16)
                except ValueError as exc:
                    raise DaqError(
                        f"Invalid char value '{raw_value}' for {context}. "
                        "Expected a single character or hex byte (e.g. '0x61')."
                    ) from exc
                if not 0 <= code_point <= 0xFF:
                    raise DaqError(
                        f"Invalid char value '{raw_value}' for {context}. "
                        "Hex value must be in range 0x00..0xff."
                    )
                parsed = chr(code_point)
            else:
                raise DaqError(
                    f"Invalid char value '{raw_value}' for {context}. "
                    "Expected a single character or hex byte (e.g. '0x61')."
                )
    elif value_type == ValueType.INT:
        try:
            parsed = int(raw_value, 0)
        except ValueError as exc:
            raise DaqError(
                f"Invalid int value '{raw_value}' for {context}."
            ) from exc
    elif value_type == ValueType.BOOL:
        normalized = raw_value.strip().lower()
        if normalized == "true":
            parsed = True
        elif normalized == "false":
            parsed = False
        else:
            raise DaqError(
                f"Invalid bool value '{raw_value}' for {context}. "
                "Expected 'true' or 'false'."
            )
    elif value_type == ValueType.STR:
        parsed = raw_value
    elif value_type == ValueType.PIN:
        parsed = raw_value.strip().lower()
    else:
        raise DaqError(f"Unsupported value type '{value_type}'.")

    return spec.validate_input(parsed, context=context)


def format_endpoint_value(
    value: Value,
    value_type: ValueType | None = None,
) -> str:
    """Formats a Value into a string."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, enum.Enum):
        return str(value.value)
    if (
        (value_type == ValueType.CHAR or value_type is None)
        and isinstance(value, str)
        and len(value) == 1
    ):
        if value.isprintable():
            return f"0x{ord(value):02x} '{value}'"
        return f"0x{ord(value):02x}"
    return str(value)


def format_event_cli(
    event: Event[Any] | EventRef[Any],
) -> str:
    """Formats an Event or EventRef for `listen` CLI output."""
    if event.value is None or event.value_type is None:
        return f"[{event.timestamp_ns}] {event.endpoint}"
    return (
        f"[{event.timestamp_ns}] {event.endpoint}: "
        f"{format_endpoint_value(event.value, event.value_type)}"
    )


def format_event_json(
    event: Event[Any] | EventRef[Any],
) -> str:
    """Formats an Event or EventRef as a single-line NDJSON object."""
    json_val: Any = None
    json_type: str | None = None
    if event.value is not None and event.value_type is not None:
        json_type = event.value_type.value
        if event.value_type == ValueType.CHAR and isinstance(event.value, str):
            json_val = ord(event.value)
        elif isinstance(event.value, enum.Enum):
            json_val = event.value.value
        else:
            json_val = event.value

    return json.dumps(
        {
            "timestamp_ns": event.timestamp_ns,
            "endpoint": str(event.endpoint),
            "value": json_val,
            "value_type": json_type,
        }
    )
