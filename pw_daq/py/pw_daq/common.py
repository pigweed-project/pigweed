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
"""Common types, enums, and exceptions for pw_daq."""

from __future__ import annotations

import abc
from collections.abc import Callable, Iterator
from dataclasses import dataclass
import enum
import re
from types import TracebackType
from typing import Any, ClassVar, Generic, TypeVar, overload

_EndpointRefT = TypeVar("_EndpointRefT", bound="EndpointRef")
_EnumT = TypeVar("_EnumT", bound=enum.Enum)
_SignalT = TypeVar("_SignalT")
_OutT = TypeVar("_OutT")
_StreamItemT = TypeVar("_StreamItemT")

_BLOCK_NAME_PATTERN = re.compile(r"^([a-zA-Z_][a-zA-Z0-9_]*?)(\d+)$")
_ENDPOINT_NAME_PATTERN = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]*$")
_DOT_REF_PATTERN = re.compile(
    r"^([a-zA-Z_][a-zA-Z0-9_]*\d+)\.([a-zA-Z_][a-zA-Z0-9_]*)$"
)


class DaqError(Exception):
    """Exception raised for DAQ configuration, validation, or backend errors."""


class DaqTimeoutError(DaqError):
    """Exception raised when a DAQ operation or event wait times out."""


class BlockType(enum.Enum):
    """Block types defined in the pw_daq specification."""

    DIGITAL = "digital"
    ADC = "adc"
    DAC = "dac"
    PWM = "pwm"
    UART = "uart"
    SPI = "spi"
    I2C = "i2c"
    TIMER = "timer"
    STOPWATCH = "stopwatch"
    LUT = "lut"


class ValueType(enum.Enum):
    """Data types for Signal outputs, Action inputs/results, and Attributes."""

    CHAR = "char"
    INT = "int"
    BOOL = "bool"
    STR = "str"
    PIN = "pin"
    DIGITAL_DIRECTION = "digital_direction"
    DIGITAL_STATE = "digital_state"
    DIGITAL_PULL = "digital_pull"
    UART_PARITY = "uart_parity"

    @property
    def enum_cls(self) -> type[EnumValue] | None:
        """Returns the Enum class for enum value types, or None for scalars."""
        return _VALUE_TYPE_ENUM_CLS.get(self)


_VALUE_TYPE_ENUM_CLS: dict[ValueType, type[Any]] = {}


def _register_value_enum(
    value_type: ValueType,
) -> Callable[[type[_EnumT]], type[_EnumT]]:
    """Decorator associating a ValueType member with its Enum class."""

    def _decorator(enum_cls: type[_EnumT]) -> type[_EnumT]:
        _VALUE_TYPE_ENUM_CLS[value_type] = enum_cls
        return enum_cls

    return _decorator


@_register_value_enum(ValueType.DIGITAL_DIRECTION)
class DigitalDirection(enum.Enum):
    """Direction of a digital GPIO block."""

    IN = "in"
    OUT = "out"
    OPENDRAIN = "opendrain"


@_register_value_enum(ValueType.DIGITAL_STATE)
class DigitalState(enum.Enum):
    """State of a digital GPIO pin (`active` or `inactive`)."""

    ACTIVE = "active"
    INACTIVE = "inactive"


@_register_value_enum(ValueType.DIGITAL_PULL)
class DigitalPull(enum.Enum):
    """Pull-up or pull-down configuration for a digital GPIO pin."""

    UP = "up"
    DOWN = "down"
    NONE = "none"


@_register_value_enum(ValueType.UART_PARITY)
class UartParity(enum.Enum):
    """Parity configuration for a UART block."""

    ODD = "odd"
    EVEN = "even"
    NONE = "none"


EnumValue = DigitalDirection | DigitalState | DigitalPull | UartParity
Value = EnumValue | str | int | bool | None


@dataclass(frozen=True)
class BlockRef:
    """Reference to a specific DAQ block instance (e.g. 'digital0', 'uart1')."""

    block_type: BlockType
    index: int

    def __post_init__(self) -> None:
        if not isinstance(self.block_type, BlockType):
            raise DaqError(
                "BlockRef.block_type must be a BlockType instance, got "
                f"{type(self.block_type).__name__}."
            )
        if not isinstance(self.index, int) or isinstance(self.index, bool):
            raise DaqError(
                "Block index must be an integer, got "
                f"{type(self.index).__name__}."
            )
        if self.index < 0:
            raise DaqError(
                f"Block index must be non-negative, got {self.index}."
            )

    @property
    def name(self) -> str:
        return f"{self.block_type.value}{self.index}"

    def __str__(self) -> str:
        return self.name

    @classmethod
    def from_str(cls, spec: str) -> BlockRef:
        """Parses a block name string of the form '<block_type><N>'."""
        if not isinstance(spec, str):
            raise DaqError(
                f"Invalid block name {spec!r}. Expected '<block_type><N>' "
                "(e.g. 'digital0', 'uart1')."
            )
        match = _BLOCK_NAME_PATTERN.match(spec.strip())
        if not match:
            raise DaqError(
                f"Invalid block name '{spec}'. Expected '<block_type><N>' "
                "(e.g. 'digital0', 'uart1')."
            )
        type_str, index_str = match.groups()
        try:
            block_type = BlockType(type_str.lower())
        except ValueError as exc:
            valid = ", ".join(b.value for b in BlockType)
            raise DaqError(
                f"Unknown block type '{type_str}' in '{spec}'. "
                f"Valid block types: {valid}."
            ) from exc
        return cls(block_type=block_type, index=int(index_str))


class EndpointRef(abc.ABC):
    """Base reference to a block endpoint ('<block>.<endpoint>')."""

    _KIND: ClassVar[str]

    block: BlockRef

    @abc.abstractmethod
    def __init__(self, block: BlockRef, endpoint: str, /) -> None:
        """Initializes an EndpointRef for `block` and `endpoint`."""

    @property
    def block_name(self) -> str:
        return self.block.name

    @property
    @abc.abstractmethod
    def endpoint(self) -> str:
        """Returns the normalized endpoint identifier."""

    def __str__(self) -> str:
        return f"{self.block.name}.{self.endpoint}"

    def _validate_and_normalize(self, endpoint_field: str) -> None:
        """Validates `self.block` and normalizes `endpoint_field` in-place."""
        if not isinstance(self.block, BlockRef):
            raise DaqError(
                f"{type(self).__name__}.block must be a BlockRef instance, "
                f"got {type(self.block).__name__}."
            )
        raw_endpoint = getattr(self, endpoint_field)
        if not isinstance(raw_endpoint, str):
            raise DaqError(
                f"{type(self).__name__}.{endpoint_field} must be a str, "
                f"got {type(raw_endpoint).__name__}."
            )
        normalized = raw_endpoint.strip().lower()
        if not _ENDPOINT_NAME_PATTERN.match(normalized):
            raise DaqError(
                f"Invalid {self._KIND} name {raw_endpoint!r} for block "
                f"'{self.block}'. Expected a valid identifier."
            )
        object.__setattr__(self, endpoint_field, normalized)

    @classmethod
    def from_str(
        cls: type[_EndpointRefT],
        spec: str,
    ) -> _EndpointRefT:
        """Parses an endpoint reference string '<block>.<endpoint>'."""
        if cls is EndpointRef or not hasattr(cls, "_KIND"):
            raise DaqError(
                f"{cls.__name__}.from_str() cannot be called directly; use a "
                "concrete EndpointRef subclass (AttributeRef, SignalRef, or "
                "ActionRef)."
            )
        if not isinstance(spec, str):
            raise DaqError(
                f"Invalid {cls._KIND} reference {spec!r}. Expected format "
                f"'<block>.<{cls._KIND}>'."
            )
        match = _DOT_REF_PATTERN.match(spec.strip())
        if not match:
            raise DaqError(
                f"Invalid {cls._KIND} reference '{spec}'. Expected format "
                f"'<block>.<{cls._KIND}>'."
            )
        block_str, endpoint_str = match.groups()
        return cls(
            BlockRef.from_str(block_str),
            endpoint_str.strip().lower(),
        )


@dataclass(frozen=True)
class AttributeRef(EndpointRef):
    """Reference to a block Attribute endpoint (e.g. 'digital1.pull')."""

    _KIND: ClassVar[str] = "attribute"

    block: BlockRef
    attribute: str

    def __post_init__(self) -> None:
        self._validate_and_normalize("attribute")

    @property
    def endpoint(self) -> str:
        return self.attribute


@dataclass(frozen=True)
class SignalRef(EndpointRef):
    """Reference to a block Signal endpoint (e.g. 'digital1.change')."""

    _KIND: ClassVar[str] = "signal"

    block: BlockRef
    signal: str

    def __post_init__(self) -> None:
        self._validate_and_normalize("signal")

    @property
    def endpoint(self) -> str:
        return self.signal


@dataclass(frozen=True)
class ActionRef(EndpointRef):
    """Reference to a block Action endpoint (e.g. 'digital0.write')."""

    _KIND: ClassVar[str] = "action"

    block: BlockRef
    action: str

    def __post_init__(self) -> None:
        self._validate_and_normalize("action")

    @property
    def endpoint(self) -> str:
        return self.action


@dataclass(frozen=True, init=False)
class EventRef(Generic[_SignalT]):
    """Unbound reference to a Signal Event emitted by a SignalRef."""

    timestamp_ns: int
    endpoint: SignalRef
    value: _SignalT | None = None
    value_type: ValueType | None = None

    @overload
    def __init__(
        self: EventRef[None],
        timestamp_ns: int,
        endpoint: SignalRef,
        value: None = None,
        value_type: ValueType | None = None,
    ) -> None: ...

    @overload
    def __init__(
        self: EventRef[_SignalT],
        timestamp_ns: int,
        endpoint: SignalRef,
        value: _SignalT,
        value_type: ValueType | None = None,
    ) -> None: ...

    def __init__(
        self,
        timestamp_ns: int,
        endpoint: SignalRef,
        value: _SignalT | None = None,
        value_type: ValueType | None = None,
    ) -> None:
        if not isinstance(timestamp_ns, int) or isinstance(timestamp_ns, bool):
            raise DaqError(
                "EventRef.timestamp_ns must be an int, got "
                f"{type(timestamp_ns).__name__}."
            )
        if not isinstance(endpoint, SignalRef):
            raise DaqError(
                "EventRef.endpoint must be a SignalRef instance, got "
                f"{type(endpoint).__name__}."
            )
        normalized_value_type = None if value is None else value_type
        if value is not None and not isinstance(
            normalized_value_type, ValueType
        ):
            raise DaqError(
                f"EventRef for '{endpoint}' with a non-None value "
                f"({value!r}) requires a valid ValueType, got {value_type!r}."
            )
        object.__setattr__(self, "timestamp_ns", timestamp_ns)
        object.__setattr__(self, "endpoint", endpoint)
        object.__setattr__(self, "value", value)
        object.__setattr__(self, "value_type", normalized_value_type)


@dataclass(frozen=True, init=False)
class ActionResult(Generic[_OutT]):
    """Result of invoking an Action endpoint."""

    success: bool
    value: _OutT | None = None
    value_type: ValueType | None = None

    @overload
    def __init__(
        self: ActionResult[None],
        success: bool,
        value: None = None,
        value_type: ValueType | None = None,
    ) -> None: ...

    @overload
    def __init__(
        self: ActionResult[_OutT],
        success: bool,
        value: _OutT,
        value_type: ValueType | None = None,
    ) -> None: ...

    def __init__(
        self,
        success: bool,
        value: _OutT | None = None,
        value_type: ValueType | None = None,
    ) -> None:
        if not isinstance(success, bool):
            raise DaqError(
                "ActionResult.success must be a bool, got "
                f"{type(success).__name__}."
            )
        if not success or value is None:
            normalized_value: _OutT | None = None
            normalized_value_type: ValueType | None = None
        else:
            if not isinstance(value_type, ValueType):
                raise DaqError(
                    "ActionResult with a non-None value requires a valid "
                    f"ValueType, got {value_type!r}."
                )
            normalized_value = value
            normalized_value_type = value_type
        object.__setattr__(self, "success", success)
        object.__setattr__(self, "value", normalized_value)
        object.__setattr__(self, "value_type", normalized_value_type)

    def __bool__(self) -> bool:
        return self.success


class EventStream(Iterator[_StreamItemT], Generic[_StreamItemT]):
    """Context-manageable iterator over event items from `listen()`."""

    def __init__(
        self,
        iterator: Iterator[_StreamItemT],
        on_close: Callable[[], None] | None = None,
    ) -> None:
        self._iterator = iterator
        self._on_close = on_close
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        """Closes the underlying stream and releases any resources."""
        if self._closed:
            return
        self._closed = True
        close_fn = getattr(self._iterator, "close", None)
        if callable(close_fn):
            close_fn()
        if self._on_close is not None:
            self._on_close()

    def __iter__(self) -> EventStream[_StreamItemT]:
        return self

    def __next__(self) -> _StreamItemT:
        if self._closed:
            raise StopIteration
        try:
            return next(self._iterator)
        except BaseException:
            self.close()
            raise

    def __enter__(self) -> EventStream[_StreamItemT]:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()
