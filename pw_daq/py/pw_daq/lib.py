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
"""Programmatic DAQ library (daq-lib) for interacting with an abstract DAQ."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Iterator, Mapping
from dataclasses import dataclass
import enum
import logging
import math
import re
from types import MappingProxyType, TracebackType
from typing import Any, ClassVar, Generic, TypeVar, cast, overload

from pw_daq.backend import DaqBackend
from pw_daq.common import (
    ActionRef,
    ActionResult,
    AttributeRef,
    BlockRef,
    BlockType,
    DaqError,
    DaqTimeoutError,
    DigitalDirection,
    DigitalPull,
    DigitalState,
    EndpointRef,
    EventStream,
    SignalRef,
    UartParity,
    Value,
    ValueType,
)

_LOG = logging.getLogger(__name__)
_PIN_PATTERN = re.compile(r"^p\d+$")
_BlockSelfT = TypeVar("_BlockSelfT", bound="Block")
_EndpointRefT = TypeVar("_EndpointRefT", bound=EndpointRef)
_EndpointSpecT = TypeVar("_EndpointSpecT", bound="EndpointSpec")
_EndpointT = TypeVar("_EndpointT", bound="_Endpoint[Any, Any]")
_AttrT = TypeVar("_AttrT")
_SignalT = TypeVar("_SignalT")
_InT = TypeVar("_InT")
_OutT = TypeVar("_OutT")


class AttributeMutability(enum.Enum):
    """Mutability trait for a block attribute."""

    CONST = "const"
    INIT = "init"
    MUTABLE = "mutable"


@dataclass(frozen=True)
class EndpointSpec:
    """Base metadata and validation rules for a block endpoint."""

    input_value_type: ValueType | None = None
    output_value_type: ValueType | None = None
    allowed_values: frozenset[Any] | None = None
    description: str = ""

    def _validate_value(
        self,
        value: Any,
        expected_value_type: ValueType | None,
        *,
        context: str,
        direction: str,
    ) -> Value:
        if expected_value_type is None:
            if value is not None:
                raise DaqError(
                    f"{context} does not accept an {direction} value, got "
                    f"'{value}'."
                )
            _LOG.debug("Validated None %s value for %s", direction, context)
            return None

        if value is None:
            raise DaqError(
                f"{context} requires an {direction} value of type "
                f"'{expected_value_type.value}'."
            )

        _LOG.debug(
            "Validating %s value %r as '%s' for %s",
            direction,
            value,
            expected_value_type.value,
            context,
        )
        enum_cls = expected_value_type.enum_cls
        if enum_cls is not None:
            if not isinstance(value, enum_cls):
                raise DaqError(
                    f"Invalid enum value {value!r} for {context}. "
                    f"Expected {enum_cls.__name__}."
                )
            validated: Value = value
        elif expected_value_type == ValueType.CHAR:
            if not isinstance(value, str) or len(value) != 1:
                raise DaqError(
                    f"Invalid char value '{value}' for {context}. "
                    "Expected a single character."
                )
            validated = value
        elif expected_value_type == ValueType.INT:
            if not isinstance(value, int) or isinstance(value, bool):
                raise DaqError(
                    f"Invalid int value {value!r} for {context}. "
                    "Expected an int."
                )
            validated = value
        elif expected_value_type == ValueType.BOOL:
            if not isinstance(value, bool):
                raise DaqError(
                    f"Invalid bool value {value!r} for {context}. "
                    "Expected a bool."
                )
            validated = value
        elif expected_value_type == ValueType.STR:
            if not isinstance(value, str):
                raise DaqError(
                    f"Invalid str value {value!r} for {context}. "
                    "Expected a str."
                )
            validated = value
        elif expected_value_type == ValueType.PIN:
            if not isinstance(value, str):
                raise DaqError(
                    f"Invalid pin value {value!r} for {context}. "
                    "Expected a virtual pin string 'p<N>' (e.g. 'p0')."
                )
            norm_pin = value.strip().lower()
            if not _PIN_PATTERN.match(norm_pin):
                raise DaqError(
                    f"Invalid pin value '{value}' for {context}. "
                    "Expected 'p<N>' (e.g. 'p0')."
                )
            validated = norm_pin
        else:
            raise DaqError(f"Unsupported value type '{expected_value_type}'.")

        if (
            self.allowed_values is not None
            and validated not in self.allowed_values
        ):
            allowed_str = ", ".join(sorted(str(v) for v in self.allowed_values))
            raise DaqError(
                f"Invalid value '{value}' for {context}. "
                f"Expected one of: {allowed_str}."
            )
        _LOG.debug(
            "Validated %s value %r for %s",
            direction,
            validated,
            context,
        )
        return validated

    def validate_input(
        self,
        value: Any,
        *,
        context: str,
    ) -> Value:
        """Validates an endpoint input value against `input_value_type`."""
        return self._validate_value(
            value,
            self.input_value_type,
            context=context,
            direction="input",
        )

    def validate_output(
        self,
        value: Any,
        *,
        context: str,
    ) -> Value:
        """Validates an endpoint output value against `output_value_type`."""
        return self._validate_value(
            value,
            self.output_value_type,
            context=context,
            direction="output",
        )


@dataclass(frozen=True, init=False)
class AttributeSpec(EndpointSpec):
    """Metadata and validation rules for a block Attribute endpoint."""

    mutability: AttributeMutability = AttributeMutability.MUTABLE
    default: Any = None
    required_on_create: bool = False

    def __init__(
        self,
        value_type: ValueType,
        *,
        mutability: AttributeMutability = AttributeMutability.MUTABLE,
        allowed_values: frozenset[Any] | None = None,
        default: Any = None,
        required_on_create: bool = False,
        description: str = "",
    ) -> None:
        if value_type is None:
            raise DaqError("AttributeSpec requires a non-None value_type.")
        super().__init__(
            input_value_type=(
                None if mutability == AttributeMutability.CONST else value_type
            ),
            output_value_type=value_type,
            allowed_values=allowed_values,
            description=description,
        )
        object.__setattr__(self, "mutability", mutability)
        object.__setattr__(self, "default", default)
        object.__setattr__(self, "required_on_create", required_on_create)


@dataclass(frozen=True, init=False)
class SignalSpec(EndpointSpec):
    """Metadata and validation rules for a block Signal endpoint."""

    def __init__(
        self,
        output_value_type: ValueType | None = None,
        *,
        allowed_values: frozenset[Any] | None = None,
        description: str = "",
    ) -> None:
        super().__init__(
            input_value_type=None,
            output_value_type=output_value_type,
            allowed_values=allowed_values,
            description=description,
        )

    def matches_input(self, action_spec: ActionSpec) -> bool:
        """Returns True if this Signal's output matches an Action's input."""
        return self.output_value_type == action_spec.input_value_type


@dataclass(frozen=True)
class ActionSpec(EndpointSpec):
    """Metadata and validation rules for a block Action endpoint."""


class _Endpoint(Generic[_EndpointRefT, _EndpointSpecT]):
    """Base programmatic representation of a block endpoint."""

    _REF_CLS: ClassVar[type[EndpointRef]]

    def __init__(
        self,
        block: Block,
        name: str,
        spec: _EndpointSpecT,
    ) -> None:
        if not isinstance(block, Block):
            raise DaqError(
                f"Expected a Block instance, got {type(block).__name__}."
            )
        if not isinstance(name, str):
            raise DaqError(
                f"Endpoint name must be a str, got {type(name).__name__}."
            )
        if not isinstance(spec, EndpointSpec):
            raise DaqError(
                "Expected an EndpointSpec instance, got "
                f"{type(spec).__name__}."
            )
        self._block = block
        self._name = name.strip().lower()
        self._spec = spec
        self._ref = cast(
            _EndpointRefT,
            self._REF_CLS(
                block._ref,  # pylint: disable=protected-access
                self._name,
            ),
        )

    @property
    def block(self) -> Block:
        return self._block

    @property
    def name(self) -> str:
        return self._name

    @property
    def spec(self) -> _EndpointSpecT:
        return self._spec

    @property
    def input_value_type(self) -> ValueType | None:
        return self._spec.input_value_type

    @property
    def output_value_type(self) -> ValueType | None:
        return self._spec.output_value_type

    def __str__(self) -> str:
        return str(self._ref)

    def __repr__(self) -> str:
        return (
            f"{type(self).__name__}({str(self._ref)!r}, "
            f"backend={self._block.backend.name()!r})"
        )

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, type(self)):
            return NotImplemented
        return (
            self._ref == other._ref
            and self._block.backend is other._block.backend
        )

    def __hash__(self) -> int:
        return hash((self._ref, id(self._block.backend)))


class Attribute(_Endpoint[AttributeRef, AttributeSpec], Generic[_AttrT]):
    """Programmatic representation of a block Attribute endpoint."""

    _REF_CLS: ClassVar[type[EndpointRef]] = AttributeRef

    def get(self) -> _AttrT:
        """Gets and validates the current value of this attribute."""
        _LOG.debug("Getting attribute '%s'", self._ref)
        raw = self._block.backend.get(self._ref)
        validated = cast(
            _AttrT,
            self._spec.validate_output(raw, context=f"Attribute '{self._ref}'"),
        )
        _LOG.debug("Attribute '%s' returned %r", self._ref, validated)
        return validated

    def set(self, value: _AttrT) -> None:
        """Validates and sets the value of this attribute on the backend."""
        if self._spec.mutability in (
            AttributeMutability.CONST,
            AttributeMutability.INIT,
        ):
            raise DaqError(
                f"Attribute '{self._ref}' is "
                f"{self._spec.mutability.value} and cannot be modified."
            )
        validated = self._spec.validate_input(
            value, context=f"Attribute '{self._ref}'"
        )
        _LOG.info("Setting attribute '%s' to %r", self._ref, validated)
        self._block.backend.set(self._ref, validated)


@dataclass(frozen=True, init=False)
class Event(Generic[_SignalT]):
    """An Event emitted by a bound Signal endpoint."""

    timestamp_ns: int
    endpoint: Signal[_SignalT]
    value: _SignalT | None = None
    value_type: ValueType | None = None

    @overload
    def __init__(
        self: Event[None],
        timestamp_ns: int,
        endpoint: Signal[None],
        value: None = None,
        value_type: ValueType | None = None,
    ) -> None: ...

    @overload
    def __init__(
        self,
        timestamp_ns: int,
        endpoint: Signal[_SignalT],
        value: _SignalT,
        value_type: ValueType | None = None,
    ) -> None: ...

    def __init__(
        self,
        timestamp_ns: int,
        endpoint: Signal[Any],
        value: _SignalT | None = None,
        value_type: ValueType | None = None,
    ) -> None:
        if not isinstance(timestamp_ns, int) or isinstance(timestamp_ns, bool):
            raise DaqError(
                "Event.timestamp_ns must be an int, got "
                f"{type(timestamp_ns).__name__}."
            )
        if not isinstance(endpoint, Signal):
            raise DaqError(
                "Event.endpoint must be a Signal instance, got "
                f"{type(endpoint).__name__}."
            )
        normalized_value_type = None if value is None else value_type
        if value is not None and not isinstance(
            normalized_value_type, ValueType
        ):
            raise DaqError(
                f"Event for '{endpoint}' with a non-None value "
                f"({value!r}) requires a valid ValueType, got {value_type!r}."
            )
        validated_value = endpoint.spec.validate_output(
            value, context=f"Signal '{endpoint}'"
        )
        if (
            normalized_value_type is not None
            and normalized_value_type != endpoint.output_value_type
        ):
            raise DaqError(
                f"Event value_type {normalized_value_type!r} does not "
                f"match Signal '{endpoint}' output type "
                f"{endpoint.output_value_type!r}."
            )
        object.__setattr__(self, "timestamp_ns", timestamp_ns)
        object.__setattr__(self, "endpoint", endpoint)
        object.__setattr__(self, "value", validated_value)
        object.__setattr__(self, "value_type", normalized_value_type)


class Signal(_Endpoint[SignalRef, SignalSpec], Generic[_SignalT]):
    """Programmatic representation of a block Signal endpoint."""

    _REF_CLS: ClassVar[type[EndpointRef]] = SignalRef

    def listen(
        self,
        count: int | None = None,
        timeout: float | None = None,
    ) -> EventStream[Event[_SignalT]]:
        """Streams new Event instances from this Signal endpoint."""
        if count is not None:
            if (
                not isinstance(count, int)
                or isinstance(count, bool)
                or count <= 0
            ):
                raise DaqError("'count' must be a positive integer.")
        if timeout is not None:
            if (
                not isinstance(timeout, (int, float))
                or isinstance(timeout, bool)
                or not math.isfinite(timeout)
                or timeout <= 0
            ):
                raise DaqError("'timeout' must be a positive number.")
        _LOG.debug(
            "Starting listen on '%s' (count=%r, timeout=%r)",
            self._ref,
            count,
            timeout,
        )
        backend_stream = self._block.backend.listen(
            self._ref, count=count, timeout=timeout
        )

        def _iter_bound() -> Iterator[Event[_SignalT]]:
            with backend_stream:
                for event_ref in backend_stream:
                    if event_ref.endpoint != self._ref:
                        raise DaqError(
                            f"Backend yielded event for '{event_ref.endpoint}' "
                            f"while listening on Signal '{self}'."
                        )
                    validated_val = self._spec.validate_output(
                        event_ref.value, context=f"Signal '{self}'"
                    )
                    if (
                        validated_val is not None
                        and event_ref.value_type != self.output_value_type
                    ):
                        _LOG.warning(
                            "Backend event on '%s' had value_type %r instead "
                            "of %r; normalizing",
                            self._ref,
                            event_ref.value_type,
                            self.output_value_type,
                        )
                    _LOG.debug(
                        "Yielding event on '%s' at timestamp_ns=%d: %r",
                        self._ref,
                        event_ref.timestamp_ns,
                        validated_val,
                    )
                    yield Event(
                        timestamp_ns=event_ref.timestamp_ns,
                        endpoint=self,
                        value=cast(_SignalT, validated_val),
                        value_type=self.output_value_type,
                    )

        return EventStream(_iter_bound(), on_close=backend_stream.close)

    def wait(
        self,
        timeout: float | None = None,
        predicate: Callable[[Event[_SignalT]], bool] | None = None,
    ) -> Event[_SignalT]:
        """Blocks waiting for a single Event from this Signal endpoint."""
        count = 1 if predicate is None else None
        _LOG.debug(
            "Waiting on '%s' (timeout=%r, has_predicate=%s)",
            self._ref,
            timeout,
            predicate is not None,
        )
        with self.listen(count=count, timeout=timeout) as stream:
            for event in stream:
                if predicate is None or predicate(event):
                    _LOG.debug(
                        "Matched event on '%s' at timestamp_ns=%d",
                        self._ref,
                        event.timestamp_ns,
                    )
                    return event
                _LOG.debug(
                    "Skipping event on '%s' at timestamp_ns=%d "
                    "(predicate returned False)",
                    self._ref,
                    event.timestamp_ns,
                )
        raise DaqTimeoutError(
            f"Timed out waiting for event on Signal '{self}'."
        )

    def on(
        self,
        action: Action[Any, Any],
        action_arg: Value = None,
    ) -> None:
        """Registers `action` to be invoked when this Signal emits an Event."""
        if not isinstance(action, Action):
            raise DaqError(
                f"Expected an Action instance, got {type(action).__name__}."
            )
        if action.block.backend is not self._block.backend:
            raise DaqError(
                f"Signal '{self}' and Action '{action}' belong to different "
                "DaqBackend instances."
            )
        action_spec = action.spec
        if action_arg is not None or action_spec.input_value_type is None:
            _LOG.debug(
                "Validating explicit/valueless arg %r for trigger '%s' -> '%s'",
                action_arg,
                self._ref,
                action,
            )
            validated_arg = action_spec.validate_input(
                action_arg, context=f"Action '{action}'"
            )
        else:
            _LOG.debug(
                "Validating implicit value forwarding for trigger '%s' -> '%s'",
                self._ref,
                action,
            )
            if not self._spec.matches_input(action_spec):
                sig_type = (
                    self._spec.output_value_type.value
                    if self._spec.output_value_type is not None
                    else "none"
                )
                act_type = action_spec.input_value_type.value
                raise DaqError(
                    f"Type mismatch between Signal '{self}' ({sig_type}) "
                    f"and Action '{action}' ({act_type})."
                )
            validated_arg = None

        _LOG.info(
            "Registering trigger on '%s' -> '%s' (arg=%r)",
            self._ref,
            action,
            validated_arg,
        )
        self._block.backend.on(
            self._ref,
            action._ref,  # pylint: disable=protected-access
            validated_arg,
        )


class Action(_Endpoint[ActionRef, ActionSpec], Generic[_InT, _OutT]):
    """Programmatic representation of a block Action endpoint."""

    _REF_CLS: ClassVar[type[EndpointRef]] = ActionRef

    def do(self, action_arg: _InT | None = None) -> ActionResult[_OutT]:
        """Invokes this Action endpoint and returns its ActionResult."""
        validated_arg = self._spec.validate_input(
            action_arg, context=f"Action '{self}'"
        )
        _LOG.info("Invoking action '%s' (arg=%r)", self._ref, validated_arg)
        result = self._block.backend.do(self._ref, validated_arg)
        if result.success:
            validated_val = self._spec.validate_output(
                result.value, context=f"Action '{self}' result"
            )
            if (
                validated_val is not None
                and result.value_type != self._spec.output_value_type
            ):
                _LOG.warning(
                    "Action '%s' returned value_type %r instead of %r; "
                    "normalizing",
                    self._ref,
                    result.value_type,
                    self._spec.output_value_type,
                )
                return ActionResult(
                    success=True,
                    value=cast(_OutT, validated_val),
                    value_type=self._spec.output_value_type,
                )
            _LOG.debug(
                "Action '%s' succeeded with result %r", self._ref, validated_val
            )
        else:
            _LOG.warning(
                "Action '%s' reported failure (success=False)", self._ref
            )
        return cast(ActionResult[_OutT], result)

    def __call__(self, action_arg: _InT | None = None) -> ActionResult[_OutT]:
        return self.do(action_arg)


class Block:
    """Base programmatic representation of a DAQ block."""

    BLOCK_TYPE: ClassVar[BlockType]
    ATTRIBUTES: ClassVar[Mapping[str, AttributeSpec]] = MappingProxyType({})
    SIGNALS: ClassVar[Mapping[str, SignalSpec]] = MappingProxyType({})
    ACTIONS: ClassVar[Mapping[str, ActionSpec]] = MappingProxyType({})

    def __init__(
        self,
        index: int,
        backend: DaqBackend,
        **attributes: Any,
    ) -> None:
        if not isinstance(backend, DaqBackend):
            raise DaqError(
                f"Expected a DaqBackend instance, got {type(backend).__name__}."
            )
        self._ref = BlockRef(block_type=self.BLOCK_TYPE, index=index)
        self._backend = backend
        self._initial_attributes: dict[str, Any] = {
            k: v for k, v in attributes.items() if v is not None
        }
        self._attributes: dict[str, Attribute[Any]] = {
            name: Attribute(self, name, spec)
            for name, spec in self.ATTRIBUTES.items()
        }
        self._signals: dict[str, Signal[Any]] = {
            name: Signal(self, name, spec)
            for name, spec in self.SIGNALS.items()
        }
        self._actions: dict[str, Action[Any, Any]] = {
            name: Action(self, name, spec)
            for name, spec in self.ACTIONS.items()
        }

    @property
    def backend(self) -> DaqBackend:
        return self._backend

    @property
    def block_type(self) -> BlockType:
        return self._ref.block_type

    @property
    def index(self) -> int:
        return self._ref.index

    @property
    def name(self) -> str:
        return self._ref.name

    def __str__(self) -> str:
        return self.name

    def __repr__(self) -> str:
        return (
            f"{type(self).__name__}(index={self.index!r}, "
            f"backend={self._backend.name()!r})"
        )

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Block):
            return NotImplemented
        return self._ref == other._ref and self._backend is other._backend

    def __hash__(self) -> int:
        return hash((self._ref, id(self._backend)))

    def _lookup_endpoint(
        self,
        name: str,
        endpoints: Mapping[str, _EndpointT],
        *,
        kind_label: str,
    ) -> _EndpointT:
        """Resolves `name` in `endpoints` or raises a descriptive DaqError."""
        normalized = name.strip().lower()
        if not normalized:
            raise DaqError(f"{kind_label} name cannot be empty.")
        if normalized not in endpoints:
            valid = sorted(endpoints.keys())
            formatted_valid = ", ".join(f".{item}" for item in valid)
            article = "an" if kind_label[0].lower() in "aeiou" else "a"
            raise DaqError(
                f"Endpoint '{self._ref}.{normalized}' is not {article} "
                f"{kind_label}. Valid {kind_label.lower()}s for "
                f"'{self.block_type.value}': {formatted_valid}."
            )
        _LOG.debug(
            "Resolved %s '.%s' on block '%s'",
            kind_label.lower(),
            normalized,
            self._ref,
        )
        return endpoints[normalized]

    def attribute(self, name: str) -> Attribute[Any]:
        """Returns the Attribute for `name` on this block or raises DaqError."""
        return self._lookup_endpoint(
            name,
            self._attributes,
            kind_label="Attribute",
        )

    def signal(self, name: str) -> Signal[Any]:
        """Returns the Signal for `name` on this block or raises DaqError."""
        return self._lookup_endpoint(
            name,
            self._signals,
            kind_label="Signal",
        )

    def action(self, name: str) -> Action[Any, Any]:
        """Returns the Action for `name` on this block or raises DaqError."""
        return self._lookup_endpoint(
            name,
            self._actions,
            kind_label="Action",
        )

    def create(self: _BlockSelfT, **attributes: Any) -> _BlockSelfT:
        """Validates attributes and creates this block on the backend."""
        combined = {
            **self._initial_attributes,
            **{k: v for k, v in attributes.items() if v is not None},
        }
        validated = self.validate_create_attributes(combined)
        _LOG.info(
            "Creating block '%s' on backend '%s' with attributes %r",
            self._ref,
            self._backend.name(),
            validated,
        )
        self._backend.create(self._ref, **validated)
        return self

    def destroy(self) -> None:
        """Tears down this block on the owning backend."""
        _LOG.info(
            "Destroying block '%s' on backend '%s'",
            self._ref,
            self._backend.name(),
        )
        self._backend.destroy(self._ref)

    def validate_create_attributes(
        self,
        attributes: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Validates create-time attributes against the abstract DAQ spec."""
        _LOG.debug(
            "Validating create attributes for '%s': %r",
            self._ref,
            attributes,
        )
        unknown = set(attributes.keys()) - set(self.ATTRIBUTES.keys())
        if unknown:
            raise DaqError(
                f"Unknown attribute(s) for '{self._ref}': "
                f"{', '.join(f'.{a}' for a in sorted(unknown))}."
            )

        for attr_name, val in attributes.items():
            spec = self.ATTRIBUTES[attr_name]
            if spec.mutability == AttributeMutability.CONST and val is not None:
                raise DaqError(
                    f"Attribute '{self._ref}.{attr_name}' is const and "
                    "cannot be set at creation."
                )

        validated: dict[str, Any] = {}
        for attr_name, spec in self.ATTRIBUTES.items():
            if spec.mutability == AttributeMutability.CONST:
                continue
            is_required = spec.required_on_create and spec.default is None
            if is_required and attributes.get(attr_name) is None:
                raise DaqError(
                    f"Missing required attribute '.{attr_name}' for "
                    f"'{self._ref}'."
                )

        for attr_name, val in attributes.items():
            if val is None:
                validated[attr_name] = None
            else:
                spec = self.ATTRIBUTES[attr_name]
                validated[attr_name] = spec.validate_input(
                    val, context=f"Attribute '{self._ref}.{attr_name}'"
                )

        return validated


_BlockT = TypeVar("_BlockT", bound=type[Block])
_block_registry: dict[BlockType, type[Block]] = {}


def register_block_class(block_cls: _BlockT) -> _BlockT:
    """Decorator registering a Block subclass for its BLOCK_TYPE."""
    _block_registry[block_cls.BLOCK_TYPE] = block_cls
    _LOG.debug(
        "Registered Block subclass %s for BlockType '%s'",
        block_cls.__name__,
        block_cls.BLOCK_TYPE.value,
    )
    return block_cls


def get_block_class(block_type: BlockType) -> type[Block] | None:
    """Returns the registered Block subclass for `block_type`, or None."""
    return _block_registry.get(block_type)


@register_block_class
class DigitalBlock(Block):
    """Programmatic representation of a `digital<N>` GPIO block."""

    BLOCK_TYPE: ClassVar[BlockType] = BlockType.DIGITAL
    ATTRIBUTES: ClassVar[Mapping[str, AttributeSpec]] = MappingProxyType(
        {
            "pin": AttributeSpec(
                ValueType.PIN,
                mutability=AttributeMutability.CONST,
                description="The virtual pin associated with this block.",
            ),
            "direction": AttributeSpec(
                ValueType.DIGITAL_DIRECTION,
                mutability=AttributeMutability.INIT,
                required_on_create=True,
                description="The direction of the digital block.",
            ),
            "initial": AttributeSpec(
                ValueType.DIGITAL_STATE,
                mutability=AttributeMutability.INIT,
                default=DigitalState.INACTIVE,
                description=(
                    "The initial value of the pin. Only valid in output or "
                    "open drain mode."
                ),
            ),
            "pull": AttributeSpec(
                ValueType.DIGITAL_PULL,
                mutability=AttributeMutability.MUTABLE,
                default=DigitalPull.NONE,
                description=(
                    "Pull-up or pull-down setting. Only valid in input or open "
                    "drain mode."
                ),
            ),
        }
    )
    SIGNALS: ClassVar[Mapping[str, SignalSpec]] = MappingProxyType(
        {
            "change": SignalSpec(ValueType.DIGITAL_STATE),
            "risingedge": SignalSpec(None),
            "fallingedge": SignalSpec(None),
        }
    )
    ACTIONS: ClassVar[Mapping[str, ActionSpec]] = MappingProxyType(
        {
            "write": ActionSpec(ValueType.DIGITAL_STATE),
            "toggle": ActionSpec(None),
            "read": ActionSpec(output_value_type=ValueType.DIGITAL_STATE),
        }
    )

    @property
    def pin(self) -> Attribute[str]:
        return self.attribute("pin")

    @property
    def direction(self) -> Attribute[DigitalDirection]:
        return self.attribute("direction")

    @property
    def initial(self) -> Attribute[DigitalState]:
        return self.attribute("initial")

    @property
    def pull(self) -> Attribute[DigitalPull]:
        return self.attribute("pull")

    @property
    def change(self) -> Signal[DigitalState]:
        return self.signal("change")

    @property
    def risingedge(self) -> Signal[None]:
        return self.signal("risingedge")

    @property
    def fallingedge(self) -> Signal[None]:
        return self.signal("fallingedge")

    @property
    def write(self) -> Action[DigitalState, None]:
        return self.action("write")

    @property
    def toggle(self) -> Action[None, None]:
        return self.action("toggle")

    @property
    def read(self) -> Action[None, DigitalState]:
        return self.action("read")

    def validate_create_attributes(
        self,
        attributes: Mapping[str, Any],
    ) -> dict[str, Any]:
        validated = super().validate_create_attributes(attributes)
        direction = validated.get("direction")
        if (
            direction == DigitalDirection.IN
            and validated.get("initial") is not None
        ):
            raise DaqError(
                f"Attribute '.initial' is invalid when direction is 'in' "
                f"for '{self._ref}'."
            )
        if (
            direction == DigitalDirection.OUT
            and validated.get("pull") is not None
        ):
            raise DaqError(
                f"Attribute '.pull' is only valid in 'in' or 'opendrain' "
                f"mode for '{self._ref}'."
            )
        return validated


@register_block_class
class UartBlock(Block):
    """Programmatic representation of a `uart<N>` block."""

    BLOCK_TYPE: ClassVar[BlockType] = BlockType.UART
    ATTRIBUTES: ClassVar[Mapping[str, AttributeSpec]] = MappingProxyType(
        {
            "txpin": AttributeSpec(
                ValueType.PIN,
                mutability=AttributeMutability.CONST,
                description="The virtual pin used for sending characters.",
            ),
            "rxpin": AttributeSpec(
                ValueType.PIN,
                mutability=AttributeMutability.CONST,
                description="The virtual pin used for receiving characters.",
            ),
            "baudrate": AttributeSpec(
                ValueType.INT,
                mutability=AttributeMutability.INIT,
                required_on_create=True,
                description="The baud rate to configure.",
            ),
            "databits": AttributeSpec(
                ValueType.INT,
                mutability=AttributeMutability.INIT,
                allowed_values=frozenset({7, 8, 9}),
                default=8,
                description="The number of data bits per character.",
            ),
            "parity": AttributeSpec(
                ValueType.UART_PARITY,
                mutability=AttributeMutability.INIT,
                default=UartParity.NONE,
                description="The type of parity to insert into each character.",
            ),
            "stopbits": AttributeSpec(
                ValueType.INT,
                mutability=AttributeMutability.INIT,
                allowed_values=frozenset({1, 2}),
                default=1,
                description="The number of stop bits per character.",
            ),
            "loopback": AttributeSpec(
                ValueType.BOOL,
                mutability=AttributeMutability.INIT,
                default=False,
                description=(
                    "Whether to automatically loop received characters back to "
                    "the transmit channel."
                ),
            ),
        }
    )
    SIGNALS: ClassVar[Mapping[str, SignalSpec]] = MappingProxyType(
        {
            "rx": SignalSpec(ValueType.CHAR),
            "sent": SignalSpec(ValueType.CHAR),
            "break": SignalSpec(None),
            "error": SignalSpec(ValueType.INT),
        }
    )
    ACTIONS: ClassVar[Mapping[str, ActionSpec]] = MappingProxyType(
        {
            "tx": ActionSpec(
                ValueType.CHAR,
            ),
        }
    )

    @property
    def txpin(self) -> Attribute[str]:
        return self.attribute("txpin")

    @property
    def rxpin(self) -> Attribute[str]:
        return self.attribute("rxpin")

    @property
    def baudrate(self) -> Attribute[int]:
        return self.attribute("baudrate")

    @property
    def databits(self) -> Attribute[int]:
        return self.attribute("databits")

    @property
    def parity(self) -> Attribute[UartParity]:
        return self.attribute("parity")

    @property
    def stopbits(self) -> Attribute[int]:
        return self.attribute("stopbits")

    @property
    def loopback(self) -> Attribute[bool]:
        return self.attribute("loopback")

    @property
    def rx(self) -> Signal[str]:
        return self.signal("rx")

    @property
    def sent(self) -> Signal[str]:
        return self.signal("sent")

    @property
    def break_signal(self) -> Signal[None]:
        return self.signal("break")

    @property
    def error(self) -> Signal[int]:
        return self.signal("error")

    @property
    def tx(self) -> Action[str, None]:
        return self.action("tx")

    def validate_create_attributes(
        self,
        attributes: Mapping[str, Any],
    ) -> dict[str, Any]:
        validated = super().validate_create_attributes(attributes)
        baudrate = validated.get("baudrate")
        if baudrate is not None and baudrate <= 0:
            raise DaqError(
                f"Attribute '.baudrate' must be a positive integer for "
                f"'{self._ref}', got {baudrate!r}."
            )
        return validated


SUPPORTED_BLOCK_TYPES: frozenset[BlockType] = frozenset(_block_registry.keys())


class Daq:
    """Represents the abstract DAQ device.

    This class wraps a pre-configured `DaqBackend` and provides an interface
    which matches the abstract DAQ API defined in design.rst. Any required
    backend configuration (`backend.configure(...)`) must be applied to the
    `DaqBackend` before passing it to `Daq`.

    The backend may not support all commands, blocks, attributes, etc. defined
    by the abstract DAQ. Use `capabilities()` to get a full listing of what the
    backend supports.
    """

    def __init__(self, backend: DaqBackend) -> None:
        """Initializes a `Daq` wrapper around a pre-configured `DaqBackend`."""
        if not isinstance(backend, DaqBackend):
            raise DaqError(
                f"Expected a DaqBackend instance, got {type(backend).__name__}."
            )
        self._backend = backend
        _LOG.debug("Initialized Daq with backend '%s'", self._backend.name())

    def __repr__(self) -> str:
        return f"Daq(backend={self._backend.name()!r})"

    @property
    def backend(self) -> DaqBackend:
        """Returns the underlying pre-configured `DaqBackend` instance."""
        return self._backend

    def name(self) -> str:
        """Returns the name of the underlying backend."""
        return self._backend.name()

    def open(self) -> None:
        """Opens the underlying backend."""
        _LOG.info("Opening DAQ backend '%s'", self._backend.name())
        self._backend.open()

    def close(self) -> None:
        """Closes the underlying backend."""
        _LOG.info("Closing DAQ backend '%s'", self._backend.name())
        self._backend.close()

    def __enter__(self) -> Daq:
        self.open()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()

    def _check_backend(self, backend: DaqBackend, label: str) -> None:
        """Raises DaqError if `backend` does not match this Daq's backend."""
        if backend is not self._backend:
            raise DaqError(
                f"{label} belongs to a different DaqBackend than this Daq."
            )

    @overload
    def _bind(self, ref: BlockRef) -> Block: ...

    @overload
    def _bind(self, ref: AttributeRef) -> Attribute[Any]: ...

    @overload
    def _bind(self, ref: SignalRef) -> Signal[Any]: ...

    @overload
    def _bind(self, ref: ActionRef) -> Action[Any, Any]: ...

    def _bind(
        self,
        ref: BlockRef | AttributeRef | SignalRef | ActionRef,
    ) -> Block | Attribute[Any] | Signal[Any] | Action[Any, Any]:
        """Returns a Block, Attribute, Signal, or Action bound to this Daq."""
        if isinstance(ref, BlockRef):
            block_cls = get_block_class(ref.block_type)
            if block_cls is None:
                raise DaqError(
                    f"Block type '{ref.block_type.value}' in '{ref}' is not "
                    "yet supported."
                )
            _LOG.debug(
                "Binding BlockRef '%s' to %s on backend '%s'",
                ref,
                block_cls.__name__,
                self._backend.name(),
            )
            return block_cls(ref.index, self._backend)
        if isinstance(ref, AttributeRef):
            _LOG.debug("Binding AttributeRef '%s'", ref)
            return self._bind(ref.block).attribute(ref.attribute)
        if isinstance(ref, SignalRef):
            _LOG.debug("Binding SignalRef '%s'", ref)
            return self._bind(ref.block).signal(ref.signal)
        if isinstance(ref, ActionRef):
            _LOG.debug("Binding ActionRef '%s'", ref)
            return self._bind(ref.block).action(ref.action)
        raise DaqError(
            "Expected a BlockRef, AttributeRef, SignalRef, or ActionRef "
            f"instance, got {type(ref).__name__}."
        )

    def digital(
        self,
        index: int,
        *,
        direction: DigitalDirection | None = None,
        initial: DigitalState | None = None,
        pull: DigitalPull | None = None,
    ) -> DigitalBlock:
        """Returns a bound DigitalBlock instance without creating it."""
        return DigitalBlock(
            index,
            self._backend,
            direction=direction,
            initial=initial,
            pull=pull,
        )

    def uart(
        self,
        index: int,
        *,
        baudrate: int | None = None,
        databits: int | None = None,
        parity: UartParity | None = None,
        stopbits: int | None = None,
        loopback: bool | None = None,
    ) -> UartBlock:
        """Returns a bound UartBlock instance without creating it."""
        return UartBlock(
            index,
            self._backend,
            baudrate=baudrate,
            databits=databits,
            parity=parity,
            stopbits=stopbits,
            loopback=loopback,
        )

    def create(self, block: _BlockSelfT, **attributes: Any) -> _BlockSelfT:
        """Validates attributes and creates `block` on the backend."""
        if not isinstance(block, Block):
            raise DaqError(
                f"Expected a Block instance, got {type(block).__name__}."
            )
        self._check_backend(block.backend, f"Block '{block}'")
        return block.create(**attributes)

    def list(self) -> list[Block]:
        """Lists all currently created blocks as bound Block instances."""
        blocks = [self._bind(ref) for ref in self._backend.list()]
        _LOG.debug(
            "Listed %d active block(s) on backend '%s': %r",
            len(blocks),
            self._backend.name(),
            [b.name for b in blocks],
        )
        return blocks

    def destroy(self, block: Block) -> None:
        """Tears down the specified block."""
        if not isinstance(block, Block):
            raise DaqError(
                f"Expected a Block instance, got {type(block).__name__}."
            )
        self._check_backend(block.backend, f"Block '{block}'")
        block.destroy()

    def set(self, attribute: Attribute[_AttrT], value: _AttrT) -> None:
        """Validates and sets the value of `attribute` on the backend."""
        if not isinstance(attribute, Attribute):
            raise DaqError(
                "Expected an Attribute instance, got "
                f"{type(attribute).__name__}."
            )
        self._check_backend(attribute.block.backend, f"Attribute '{attribute}'")
        attribute.set(value)

    def get(self, attribute: Attribute[_AttrT]) -> _AttrT:
        """Gets and validates the current value of `attribute`."""
        if not isinstance(attribute, Attribute):
            raise DaqError(
                "Expected an Attribute instance, got "
                f"{type(attribute).__name__}."
            )
        self._check_backend(attribute.block.backend, f"Attribute '{attribute}'")
        return attribute.get()

    def on(
        self,
        signal: Signal[Any],
        action: Action[Any, Any],
        action_arg: Value = None,
    ) -> None:
        """Registers `action` to be invoked when `signal` emits an Event."""
        if not isinstance(signal, Signal):
            raise DaqError(
                f"Expected a Signal instance, got {type(signal).__name__}."
            )
        if not isinstance(action, Action):
            raise DaqError(
                f"Expected an Action instance, got {type(action).__name__}."
            )
        self._check_backend(signal.block.backend, f"Signal '{signal}'")
        self._check_backend(action.block.backend, f"Action '{action}'")
        signal.on(action, action_arg)

    def listen(
        self,
        signal: Signal[_SignalT],
        count: int | None = None,
        timeout: float | None = None,
    ) -> EventStream[Event[_SignalT]]:
        """Streams new Event instances from `signal`."""
        if not isinstance(signal, Signal):
            raise DaqError(
                f"Expected a Signal instance, got {type(signal).__name__}."
            )
        self._check_backend(signal.block.backend, f"Signal '{signal}'")
        return signal.listen(count=count, timeout=timeout)

    def wait(
        self,
        signal: Signal[_SignalT],
        timeout: float | None = None,
        predicate: Callable[[Event[_SignalT]], bool] | None = None,
    ) -> Event[_SignalT]:
        """Blocks waiting for a single Event from `signal`."""
        if not isinstance(signal, Signal):
            raise DaqError(
                f"Expected a Signal instance, got {type(signal).__name__}."
            )
        self._check_backend(signal.block.backend, f"Signal '{signal}'")
        return signal.wait(timeout=timeout, predicate=predicate)

    def do(
        self,
        action: Action[_InT, _OutT],
        action_arg: _InT | None = None,
    ) -> ActionResult[_OutT]:
        """Invokes `action` and returns its ActionResult."""
        if not isinstance(action, Action):
            raise DaqError(
                f"Expected an Action instance, got {type(action).__name__}."
            )
        self._check_backend(action.block.backend, f"Action '{action}'")
        return action.do(action_arg)

    def dump(self) -> Iterable[Event[Any]]:
        """Returns all logged Event instances from the DAQ device."""
        _LOG.debug("Dumping events from backend '%s'", self._backend.name())
        events: list[Event[Any]] = []
        for event_ref in self._backend.dump():
            signal = self._bind(event_ref.endpoint)
            validated_val = signal.spec.validate_output(
                event_ref.value, context=f"Signal '{signal}'"
            )
            if (
                validated_val is not None
                and event_ref.value_type != signal.output_value_type
            ):
                _LOG.warning(
                    "Dumped event on '%s' had value_type %r instead of "
                    "%r; normalizing",
                    signal,
                    event_ref.value_type,
                    signal.output_value_type,
                )
            events.append(
                Event(
                    timestamp_ns=event_ref.timestamp_ns,
                    endpoint=signal,
                    value=validated_val,
                    value_type=signal.output_value_type,
                )
            )
        _LOG.debug(
            "Dumped %d event(s) from backend '%s'",
            len(events),
            self._backend.name(),
        )
        return events

    def clear(self) -> None:
        """Clears all Event logs from the DAQ device."""
        _LOG.info("Clearing event logs on backend '%s'", self._backend.name())
        self._backend.clear()

    def info(self) -> Mapping[str, Any]:
        """Returns information about the connected DAQ device or backend."""
        _LOG.debug("Querying info from backend '%s'", self._backend.name())
        return self._backend.info()

    def capabilities(self) -> Mapping[str, Any]:
        """Returns capabilities of the DAQ device."""
        _LOG.debug(
            "Querying capabilities from backend '%s'", self._backend.name()
        )
        return self._backend.capabilities()

    def reset(self) -> None:
        """Resets the DAQ device configuration to a power-on state."""
        _LOG.info("Resetting DAQ device on backend '%s'", self._backend.name())
        self._backend.reset()
