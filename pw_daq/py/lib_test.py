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
"""Unit tests for pw_daq programmatic library (daq-lib)."""

# pylint: disable=missing-function-docstring

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping
from dataclasses import FrozenInstanceError
import enum
from pathlib import Path
import types
import typing
import unittest
from unittest import mock

import pw_daq
from pw_daq import lib as lib_mod
from pw_daq.backend import DaqBackend
from pw_daq.backends.mock_daq import mock_backend as mock_backend_mod
from pw_daq.backends.mock_daq.mock_backend import (
    DEFAULT_MOCK_CAPABILITIES,
    MockBackend,
)
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
    EnumValue,
    SignalRef,
    EventRef,
    EventStream,
    UartParity,
    Value,
    ValueType,
)
from pw_daq.lib import (
    SUPPORTED_BLOCK_TYPES,
    Action,
    ActionSpec,
    Attribute,
    AttributeMutability,
    AttributeSpec,
    Block,
    Daq,
    DigitalBlock,
    EndpointSpec,
    Signal,
    SignalSpec,
    Event,
    UartBlock,
)


class PackageExportsTest(unittest.TestCase):
    """Tests for top-level `pw_daq` public API re-exports."""

    def test_public_exports_and_ref_exclusion(self) -> None:
        expected_exports: dict[str, object] = {
            "SUPPORTED_BLOCK_TYPES": SUPPORTED_BLOCK_TYPES,
            "Action": Action,
            "ActionResult": ActionResult,
            "Attribute": Attribute,
            "AttributeMutability": AttributeMutability,
            "Block": Block,
            "BlockType": BlockType,
            "Daq": Daq,
            "DaqError": DaqError,
            "DaqTimeoutError": DaqTimeoutError,
            "DigitalBlock": DigitalBlock,
            "DigitalDirection": DigitalDirection,
            "DigitalPull": DigitalPull,
            "DigitalState": DigitalState,
            "EnumValue": EnumValue,
            "Signal": Signal,
            "Event": Event,
            "EventStream": EventStream,
            "UartBlock": UartBlock,
            "UartParity": UartParity,
            "Value": Value,
            "ValueType": ValueType,
        }

        self.assertEqual(len(pw_daq.__all__), len(set(pw_daq.__all__)))
        self.assertEqual(set(pw_daq.__all__), set(expected_exports.keys()))

        for name, expected_obj in expected_exports.items():
            self.assertIn(name, pw_daq.__all__)
            self.assertTrue(hasattr(pw_daq, name), f"Missing pw_daq.{name}")
            self.assertIs(getattr(pw_daq, name), expected_obj)

        # Also verify direct attribute resolution for static type checkers.
        self.assertIs(pw_daq.Daq, Daq)
        self.assertIs(pw_daq.Block, Block)
        self.assertIs(pw_daq.DigitalBlock, DigitalBlock)
        self.assertIs(pw_daq.UartBlock, UartBlock)
        self.assertIs(pw_daq.Attribute, Attribute)
        self.assertIs(pw_daq.Signal, Signal)
        self.assertIs(pw_daq.Action, Action)
        self.assertIs(pw_daq.Event, Event)
        self.assertIs(pw_daq.EventStream, EventStream)
        self.assertIs(pw_daq.ActionResult, ActionResult)
        self.assertIs(pw_daq.AttributeMutability, AttributeMutability)
        self.assertIs(pw_daq.BlockType, BlockType)
        self.assertIs(pw_daq.DigitalDirection, DigitalDirection)
        self.assertIs(pw_daq.DigitalPull, DigitalPull)
        self.assertIs(pw_daq.DigitalState, DigitalState)
        self.assertIs(pw_daq.UartParity, UartParity)
        self.assertIs(pw_daq.ValueType, ValueType)
        self.assertIs(pw_daq.EnumValue, EnumValue)
        self.assertIs(pw_daq.Value, Value)
        self.assertIs(pw_daq.SUPPORTED_BLOCK_TYPES, SUPPORTED_BLOCK_TYPES)
        self.assertIs(pw_daq.DaqError, DaqError)
        self.assertIs(pw_daq.DaqTimeoutError, DaqTimeoutError)

        internal_ref_names = (
            "BlockRef",
            "EndpointRef",
            "AttributeRef",
            "SignalRef",
            "ActionRef",
            "EventRef",
        )
        for ref_name in internal_ref_names:
            self.assertNotIn(ref_name, pw_daq.__all__)
            self.assertFalse(
                hasattr(pw_daq, ref_name),
                f"Internal ref type pw_daq.{ref_name} should not be exported",
            )


class BlockRegistryTest(unittest.TestCase):
    """Tests for SUPPORTED_BLOCK_TYPES and Daq._bind."""

    def test_supported_blocks(self) -> None:
        daq = Daq(MockBackend())
        self.assertIn(BlockType.DIGITAL, SUPPORTED_BLOCK_TYPES)
        self.assertIn(BlockType.UART, SUPPORTED_BLOCK_TYPES)
        self.assertIsInstance(
            daq._bind(  # pylint: disable=protected-access
                BlockRef(BlockType.DIGITAL, 0)
            ),
            DigitalBlock,
        )
        self.assertIsInstance(
            daq._bind(  # pylint: disable=protected-access
                BlockRef(BlockType.UART, 0)
            ),
            UartBlock,
        )

        d0 = BlockRef(BlockType.DIGITAL, 0)
        self.assertEqual(d0.name, "digital0")
        self.assertEqual(BlockRef.from_str("digital0"), d0)

        u3 = BlockRef(BlockType.UART, 3)
        self.assertEqual(u3.name, "uart3")
        self.assertEqual(BlockRef.from_str("uart3"), u3)
        attr_ref = AttributeRef.from_str("digital0.pull")
        self.assertEqual(attr_ref, AttributeRef(d0, "pull"))
        self.assertEqual(attr_ref.endpoint, "pull")
        signal_ref = SignalRef.from_str("uart3.rx")
        self.assertEqual(signal_ref, SignalRef(u3, "rx"))
        self.assertEqual(signal_ref.endpoint, "rx")
        action_ref = ActionRef.from_str("digital0.write")
        self.assertEqual(action_ref, ActionRef(d0, "write"))
        self.assertEqual(action_ref.endpoint, "write")

        with self.assertRaises(DaqError):
            BlockRef("digital", 0)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            BlockRef(None, 0)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            BlockRef(0, 0)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            BlockRef(BlockType.DIGITAL, -1)
        with self.assertRaises(DaqError):
            BlockRef(BlockType.DIGITAL, False)
        with self.assertRaises(DaqError):
            BlockRef(BlockType.DIGITAL, True)
        with self.assertRaises(DaqError):
            BlockRef(BlockType.DIGITAL, "0")  # type: ignore[arg-type]
        for bad_block_spec in (None, 123, True):
            with self.assertRaises(DaqError):
                BlockRef.from_str(bad_block_spec)  # type: ignore[arg-type]
        for bad_block in ("digital", "foo0", "digital-1", ""):
            with self.assertRaises(DaqError):
                BlockRef.from_str(bad_block)
        for bad_ref in ("digital0", ".pull", "digital0.", ""):
            with self.assertRaises(DaqError):
                AttributeRef.from_str(bad_ref)
            with self.assertRaises(DaqError):
                SignalRef.from_str(bad_ref)
            with self.assertRaises(DaqError):
                ActionRef.from_str(bad_ref)

    def test_endpoint_ref_abstract_and_validation(self) -> None:
        daq = Daq(MockBackend())
        d0 = BlockRef(BlockType.DIGITAL, 0)

        with self.assertRaises(TypeError):
            # pylint: disable=abstract-class-instantiated
            EndpointRef(d0, "pull")  # type: ignore[abstract]

        with self.assertRaises(DaqError):
            EndpointRef.from_str("digital0.pull")  # type: ignore[type-abstract]

        bad_blocks: tuple[typing.Any, ...] = (
            "digital0",
            None,
            0,
            daq.digital(0),
        )
        for bad_block in bad_blocks:
            with self.assertRaises(DaqError):
                AttributeRef(bad_block, "pull")
            with self.assertRaises(DaqError):
                SignalRef(bad_block, "change")
            with self.assertRaises(DaqError):
                ActionRef(bad_block, "write")

        bad_endpoints: tuple[typing.Any, ...] = (
            None,
            123,
            True,
            "",
            "   ",
            "123bad",
            "pull.extra",
            "bad-name",
            "has space",
        )
        for bad_endpoint in bad_endpoints:
            with self.assertRaises(DaqError):
                AttributeRef(d0, bad_endpoint)
            with self.assertRaises(DaqError):
                SignalRef(d0, bad_endpoint)
            with self.assertRaises(DaqError):
                ActionRef(d0, bad_endpoint)

        attr_ref = AttributeRef(d0, " PULL ")
        self.assertEqual(attr_ref, AttributeRef(d0, "pull"))
        self.assertEqual(attr_ref, AttributeRef.from_str("digital0.PULL"))
        self.assertEqual(attr_ref.attribute, "pull")
        self.assertEqual(attr_ref.endpoint, "pull")
        self.assertEqual(attr_ref.block_name, "digital0")
        self.assertEqual(str(attr_ref), "digital0.pull")
        self.assertEqual(hash(attr_ref), hash(AttributeRef(d0, "pull")))

        signal_ref = SignalRef(d0, " CHANGE ")
        self.assertEqual(signal_ref, SignalRef(d0, "change"))
        self.assertEqual(signal_ref, SignalRef.from_str("digital0.CHANGE"))
        self.assertEqual(signal_ref.signal, "change")
        self.assertEqual(signal_ref.endpoint, "change")
        self.assertEqual(str(signal_ref), "digital0.change")
        self.assertEqual(hash(signal_ref), hash(SignalRef(d0, "change")))

        action_ref = ActionRef(d0, " WRITE ")
        self.assertEqual(action_ref, ActionRef(d0, "write"))
        self.assertEqual(action_ref, ActionRef.from_str("digital0.WRITE"))
        self.assertEqual(action_ref.action, "write")
        self.assertEqual(action_ref.endpoint, "write")
        self.assertEqual(str(action_ref), "digital0.write")
        self.assertEqual(hash(action_ref), hash(ActionRef(d0, "write")))

        # Valid identifiers with leading underscore and digits are accepted.
        underscore_ref = AttributeRef(d0, " _Pin_2 ")
        self.assertEqual(underscore_ref.attribute, "_pin_2")
        self.assertEqual(
            underscore_ref, AttributeRef.from_str("digital0._Pin_2")
        )


class SpecValidationTest(unittest.TestCase):
    """Tests for EndpointSpec, AttributeSpec, SignalSpec, and ActionSpec."""

    def test_digital_state_enum_members_and_values(self) -> None:
        self.assertEqual(
            [m.name for m in DigitalState],
            ["ACTIVE", "INACTIVE"],
        )
        self.assertEqual(
            [m.value for m in DigitalState],
            ["active", "inactive"],
        )
        self.assertEqual(
            list(DigitalState.__members__.keys()),
            ["ACTIVE", "INACTIVE"],
        )
        self.assertIs(DigitalState("active"), DigitalState.ACTIVE)
        self.assertIs(DigitalState("inactive"), DigitalState.INACTIVE)
        self.assertFalse(hasattr(DigitalState, "HIGH"))
        self.assertFalse(hasattr(DigitalState, "LOW"))
        for removed in ("high", "low", "HIGH", "LOW"):
            with self.assertRaises(ValueError):
                DigitalState(removed)
        self.assertIs(
            DigitalBlock.ATTRIBUTES["initial"].default,
            DigitalState.INACTIVE,
        )

    def test_value_type_enum_matches_value_union(self) -> None:
        value_type_to_python_type: dict[ValueType, type[object]] = {
            ValueType.CHAR: str,
            ValueType.INT: int,
            ValueType.BOOL: bool,
            ValueType.STR: str,
            ValueType.PIN: str,
            ValueType.DIGITAL_DIRECTION: DigitalDirection,
            ValueType.DIGITAL_STATE: DigitalState,
            ValueType.DIGITAL_PULL: DigitalPull,
            ValueType.UART_PARITY: UartParity,
        }
        self.assertEqual(
            set(value_type_to_python_type.keys()),
            set(ValueType),
        )
        self.assertEqual(
            set(value_type_to_python_type.values()) | {type(None)},
            set(typing.get_args(Value)),
        )
        for value_type, py_type in value_type_to_python_type.items():
            if issubclass(py_type, enum.Enum):
                self.assertIs(value_type.enum_cls, py_type)
            else:
                self.assertIsNone(value_type.enum_cls)

    def test_attribute_spec_validation(self) -> None:
        with self.assertRaises(DaqError):
            AttributeSpec(None)  # type: ignore[arg-type]

        const_spec = AttributeSpec(
            ValueType.PIN,
            mutability=AttributeMutability.CONST,
        )
        self.assertIsNone(const_spec.input_value_type)
        self.assertEqual(const_spec.output_value_type, ValueType.PIN)
        self.assertEqual(const_spec.validate_output("P3", context="pin"), "p3")
        for bad_pin in (3, "3", "gp3", "p-1", ""):
            with self.subTest(bad_pin=bad_pin):
                with self.assertRaises(DaqError):
                    const_spec.validate_output(bad_pin, context="pin")
        with self.assertRaises(DaqError):
            const_spec.validate_input("p3", context="pin")

        enum_spec = AttributeSpec(ValueType.DIGITAL_PULL)
        self.assertIsInstance(enum_spec, EndpointSpec)
        self.assertEqual(enum_spec.input_value_type, ValueType.DIGITAL_PULL)
        self.assertEqual(enum_spec.output_value_type, ValueType.DIGITAL_PULL)
        self.assertEqual(
            enum_spec.validate_input(DigitalPull.UP, context="pull"),
            DigitalPull.UP,
        )
        self.assertEqual(
            enum_spec.validate_output(DigitalPull.UP, context="pull"),
            DigitalPull.UP,
        )
        with self.assertRaises(DaqError):
            enum_spec.validate_input("up", context="pull")
        with self.assertRaises(DaqError):
            enum_spec.validate_output("up", context="pull")

        int_spec = AttributeSpec(
            ValueType.INT,
            allowed_values=frozenset({7, 8, 9}),
        )
        self.assertEqual(
            int_spec.validate_input(8, context="databits"),
            8,
        )
        self.assertEqual(
            int_spec.validate_output(8, context="databits"),
            8,
        )
        with self.assertRaises(DaqError):
            int_spec.validate_input(True, context="databits")
        with self.assertRaises(DaqError):
            int_spec.validate_input("8", context="databits")
        with self.assertRaises(DaqError):
            int_spec.validate_input(6, context="databits")

        bool_spec = AttributeSpec(ValueType.BOOL)
        self.assertIs(bool_spec.validate_input(True, context="loopback"), True)
        with self.assertRaises(DaqError):
            bool_spec.validate_input(1, context="loopback")

        str_spec = AttributeSpec(ValueType.STR)
        self.assertEqual(
            str_spec.validate_input("DIO0", context="label"),
            "DIO0",
        )
        with self.assertRaises(DaqError):
            str_spec.validate_input(14, context="label")
        with self.assertRaises(DaqError):
            str_spec.validate_input(False, context="label")

    def test_signal_and_action_spec_validation(self) -> None:
        valueless_spec = ActionSpec(None)
        self.assertIsInstance(valueless_spec, EndpointSpec)
        self.assertIsNone(valueless_spec.input_value_type)
        self.assertIsNone(valueless_spec.output_value_type)
        self.assertIsNone(valueless_spec.validate_input(None, context="toggle"))
        self.assertIsNone(
            valueless_spec.validate_output(None, context="toggle")
        )
        with self.assertRaises(DaqError):
            valueless_spec.validate_input(1, context="toggle")
        with self.assertRaises(DaqError):
            valueless_spec.validate_output(1, context="toggle")

        enum_signal = SignalSpec(ValueType.DIGITAL_STATE)
        enum_action = ActionSpec(ValueType.DIGITAL_STATE)
        read_action = ActionSpec(output_value_type=ValueType.DIGITAL_STATE)
        self.assertIsInstance(enum_signal, EndpointSpec)
        self.assertIsNone(enum_signal.input_value_type)
        self.assertEqual(enum_signal.output_value_type, ValueType.DIGITAL_STATE)
        self.assertTrue(enum_signal.matches_input(enum_action))
        self.assertFalse(enum_signal.matches_input(valueless_spec))
        self.assertEqual(
            enum_action.validate_input(DigitalState.ACTIVE, context="write"),
            DigitalState.ACTIVE,
        )
        self.assertEqual(
            enum_action.validate_input(DigitalState.INACTIVE, context="write"),
            DigitalState.INACTIVE,
        )
        self.assertEqual(
            read_action.validate_output(DigitalState.ACTIVE, context="read"),
            DigitalState.ACTIVE,
        )
        self.assertEqual(
            read_action.validate_output(DigitalState.INACTIVE, context="read"),
            DigitalState.INACTIVE,
        )
        with self.assertRaises(DaqError):
            enum_action.validate_input(None, context="write")
        for raw_state in ("active", "inactive", "high", "low"):
            with self.assertRaises(DaqError):
                enum_action.validate_input(raw_state, context="write")
            with self.assertRaises(DaqError):
                read_action.validate_output(raw_state, context="read")

        char_spec = ActionSpec(ValueType.CHAR)
        self.assertEqual(
            char_spec.validate_input("x", context="tx"),
            "x",
        )
        with self.assertRaises(DaqError):
            char_spec.validate_input("xy", context="tx")
        with self.assertRaises(DaqError):
            char_spec.validate_input("", context="tx")

        int_spec = SignalSpec(ValueType.INT)
        self.assertEqual(int_spec.validate_output(42, context="err"), 42)
        with self.assertRaises(DaqError):
            int_spec.validate_output(True, context="err")

        bool_spec = SignalSpec(ValueType.BOOL)
        self.assertIs(bool_spec.validate_output(False, context="b"), False)
        with self.assertRaises(DaqError):
            bool_spec.validate_output(0, context="b")

    def test_enum_allowed_values_validation(self) -> None:
        allowed_directions = frozenset(
            {DigitalDirection.IN, DigitalDirection.OUT}
        )
        expected_suffix = (
            "Expected one of: DigitalDirection.IN, DigitalDirection.OUT."
        )

        attr_spec = AttributeSpec(
            ValueType.DIGITAL_DIRECTION,
            allowed_values=allowed_directions,
        )
        for allowed in (DigitalDirection.IN, DigitalDirection.OUT):
            self.assertEqual(
                attr_spec.validate_input(allowed, context="direction"),
                allowed,
            )
            self.assertEqual(
                attr_spec.validate_output(allowed, context="direction"),
                allowed,
            )
        with self.assertRaises(DaqError) as ctx:
            attr_spec.validate_input(
                DigitalDirection.OPENDRAIN, context="direction"
            )
        self.assertIn(expected_suffix, str(ctx.exception))
        with self.assertRaises(DaqError) as ctx:
            attr_spec.validate_output(
                DigitalDirection.OPENDRAIN, context="direction"
            )
        self.assertIn(expected_suffix, str(ctx.exception))

        signal_spec = SignalSpec(
            ValueType.DIGITAL_DIRECTION,
            allowed_values=allowed_directions,
        )
        for allowed in (DigitalDirection.IN, DigitalDirection.OUT):
            self.assertEqual(
                signal_spec.validate_output(allowed, context="dir_change"),
                allowed,
            )
        with self.assertRaises(DaqError) as ctx:
            signal_spec.validate_output(
                DigitalDirection.OPENDRAIN, context="dir_change"
            )
        self.assertIn(expected_suffix, str(ctx.exception))

        action_spec = ActionSpec(
            input_value_type=ValueType.DIGITAL_DIRECTION,
            output_value_type=ValueType.DIGITAL_DIRECTION,
            allowed_values=allowed_directions,
        )
        for allowed in (DigitalDirection.IN, DigitalDirection.OUT):
            self.assertEqual(
                action_spec.validate_input(allowed, context="set_dir"),
                allowed,
            )
            self.assertEqual(
                action_spec.validate_output(allowed, context="set_dir"),
                allowed,
            )
        with self.assertRaises(DaqError) as ctx:
            action_spec.validate_input(
                DigitalDirection.OPENDRAIN, context="set_dir"
            )
        self.assertIn(expected_suffix, str(ctx.exception))
        with self.assertRaises(DaqError) as ctx:
            action_spec.validate_output(
                DigitalDirection.OPENDRAIN, context="set_dir"
            )
        self.assertIn(expected_suffix, str(ctx.exception))


class DaqAndBlocksTest(unittest.TestCase):
    """Tests for Daq, Block, DigitalBlock, UartBlock, and endpoints."""

    def setUp(self) -> None:
        self.backend = MockBackend(log_capacity=256)
        self.daq = Daq(self.backend)

    def test_daq_initialization_and_lifecycle(self) -> None:
        with self.assertRaises(DaqError):
            Daq("not_a_backend")  # type: ignore[arg-type]

        with self.assertRaises(DaqError):
            DigitalBlock(0, "not_a_backend")  # type: ignore[arg-type]

        d0 = self.daq.digital(0)
        for bad_blk in ("digital0", None, 0, BlockRef(BlockType.DIGITAL, 0)):
            with self.assertRaises(DaqError):
                Attribute(
                    bad_blk,  # type: ignore[arg-type]
                    "pull",
                    DigitalBlock.ATTRIBUTES["pull"],
                )
            with self.assertRaises(DaqError):
                Signal(
                    bad_blk,  # type: ignore[arg-type]
                    "change",
                    DigitalBlock.SIGNALS["change"],
                )
            with self.assertRaises(DaqError):
                Action(
                    bad_blk,  # type: ignore[arg-type]
                    "write",
                    DigitalBlock.ACTIONS["write"],
                )
        for bad_name in (None, 123, True):
            with self.assertRaises(DaqError):
                Attribute(
                    d0,
                    bad_name,  # type: ignore[arg-type]
                    DigitalBlock.ATTRIBUTES["pull"],
                )
        for bad_spec in (None, "pull"):
            with self.assertRaises(DaqError):
                Attribute(
                    d0,
                    "pull",
                    bad_spec,  # type: ignore[arg-type]
                )

        self.assertEqual(self.daq.name(), "mock")
        self.assertIsInstance(self.daq.backend, MockBackend)

        with (
            mock.patch.object(self.backend, "open") as mock_open,
            mock.patch.object(self.backend, "close") as mock_close,
        ):
            with self.daq as entered:
                self.assertIs(entered, self.daq)
                mock_open.assert_called_once_with()
            mock_close.assert_called_once_with()

    def test_block_lookup_and_equality(self) -> None:
        b1 = self.daq.digital(1)
        b2 = self.daq.digital(1)
        u0 = self.daq.uart(0)
        d1_ref = BlockRef(BlockType.DIGITAL, 1)

        self.assertIsInstance(b1, DigitalBlock)
        self.assertIsInstance(u0, UartBlock)
        self.assertEqual(b1, b2)
        self.assertNotEqual(b1, d1_ref)
        self.assertNotEqual(d1_ref, b1)
        self.assertEqual(hash(b1), hash(b2))
        self.assertEqual(b1.block_type, BlockType.DIGITAL)
        self.assertEqual(b1.index, 1)
        self.assertEqual(b1.name, "digital1")
        self.assertEqual(str(b1), "digital1")
        self.assertNotEqual(b1, "digital1")

        # Block bound to another backend is not equal to b1 or BlockRef.
        other_backend = MockBackend()
        b_other = DigitalBlock(1, other_backend)
        self.assertNotEqual(b1, b_other)
        self.assertNotEqual(b_other, d1_ref)
        self.assertNotEqual(b1.pull, b_other.pull)
        self.assertNotEqual(b1.change, b_other.change)
        self.assertNotEqual(b1.write, b_other.write)

        # Internal Daq._bind binds BlockRef, AttributeRef, SignalRef, ActionRef.
        # pylint: disable=protected-access
        self.assertEqual(self.daq._bind(d1_ref), b1)
        self.assertEqual(
            self.daq._bind(AttributeRef.from_str("digital1.pull")),
            b1.pull,
        )
        self.assertEqual(
            self.daq._bind(SignalRef.from_str("digital1.change")),
            b1.change,
        )
        self.assertEqual(
            self.daq._bind(ActionRef.from_str("digital1.write")),
            b1.write,
        )

        with self.assertRaises(DaqError):
            self.daq._bind(BlockRef(BlockType.ADC, 0))
        with self.assertRaises(DaqError):
            self.daq._bind("digital1")  # type: ignore[call-overload]
        # pylint: enable=protected-access
        with self.assertRaises(DaqError):
            self.daq.destroy("digital1")  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            self.daq.destroy(d1_ref)  # type: ignore[arg-type]

    def test_daq_create_set_and_get(self) -> None:
        d0_spec = self.daq.digital(
            0,
            direction=DigitalDirection.IN,
            pull=DigitalPull.DOWN,
        )
        d0: DigitalBlock = self.daq.create(d0_spec)
        self.assertIs(d0, d0_spec)
        self.assertIsInstance(d0, DigitalBlock)

        d1_spec = self.daq.digital(
            1,
            direction=DigitalDirection.IN,
            pull=DigitalPull.DOWN,
        )
        d1: DigitalBlock = self.daq.create(d1_spec, pull=DigitalPull.UP)
        self.assertIs(d1, d1_spec)
        self.assertIsInstance(d1, DigitalBlock)

        d2_spec = self.daq.digital(2)
        d2: DigitalBlock = self.daq.create(
            d2_spec,
            direction=DigitalDirection.OUT,
            initial=DigitalState.ACTIVE,
        )
        self.assertIs(d2, d2_spec)
        self.assertIsInstance(d2, DigitalBlock)

        u0_spec = self.daq.uart(0, baudrate=9600, parity=UartParity.NONE)
        u0: UartBlock = self.daq.create(u0_spec)
        self.assertIs(u0, u0_spec)
        self.assertIsInstance(u0, UartBlock)

        u1_spec = self.daq.uart(1, baudrate=9600, parity=UartParity.NONE)
        u1: UartBlock = self.daq.create(
            u1_spec,
            baudrate=115200,
            parity=UartParity.EVEN,
            loopback=True,
        )
        self.assertIs(u1, u1_spec)
        self.assertIsInstance(u1, UartBlock)

        self.assertEqual(self.daq.list(), [d0, d1, d2, u0, u1])

        # Reading and updating attributes via daq.get and daq.set.
        pin_val: str = self.daq.get(d0.pin)
        dir_val: DigitalDirection = self.daq.get(d0.direction)
        pull_val: DigitalPull = self.daq.get(d0.pull)
        self.assertEqual(pin_val, "p0")
        self.assertEqual(dir_val, DigitalDirection.IN)
        self.assertEqual(pull_val, DigitalPull.DOWN)
        self.assertEqual(self.daq.get(d1.pull), DigitalPull.UP)
        self.assertEqual(self.daq.get(d2.initial), DigitalState.ACTIVE)
        self.assertEqual(self.daq.get(u0.baudrate), 9600)
        self.assertEqual(self.daq.get(u0.parity), UartParity.NONE)
        self.assertEqual(self.daq.get(u1.baudrate), 115200)
        self.assertEqual(self.daq.get(u1.parity), UartParity.EVEN)
        self.assertIs(self.daq.get(u1.loopback), True)

        self.daq.set(d0.pull, DigitalPull.UP)
        self.assertEqual(self.daq.get(d0.pull), DigitalPull.UP)
        self.assertEqual(d0.pull.get(), DigitalPull.UP)

        self.daq.set(d0.pull, DigitalPull.NONE)
        self.assertEqual(self.daq.get(d0.pull), DigitalPull.NONE)

        # Invalid attribute value or create attributes raise DaqError.
        with self.assertRaises(DaqError):
            self.daq.set(d0.pull, "up")  # type: ignore[misc]
        with self.assertRaises(DaqError):
            self.daq.create(self.daq.digital(3))
        with self.assertRaises(DaqError):
            self.daq.create(
                self.daq.digital(3, direction=DigitalDirection.IN),
                pin="p3",
            )

        # Const and init attributes cannot be modified via daq.set.
        with self.assertRaises(DaqError):
            self.daq.set(d0.pin, "p10")
        with self.assertRaises(DaqError):
            self.daq.set(d0.direction, DigitalDirection.OUT)
        with self.assertRaises(DaqError):
            self.daq.set(d2.initial, DigitalState.INACTIVE)
        with self.assertRaises(DaqError):
            self.daq.set(u0.txpin, "p4")
        with self.assertRaises(DaqError):
            self.daq.set(u0.baudrate, 19200)

        # Non-Block arguments to daq.create and non-Attribute arguments to
        # daq.set / daq.get raise DaqError.
        d0_ref = BlockRef(BlockType.DIGITAL, 0)
        pull_ref = AttributeRef.from_str("digital0.pull")
        change_ref = SignalRef.from_str("digital0.change")
        write_ref = ActionRef.from_str("digital0.write")

        for bad_block in (
            "digital0",
            d0_ref,
            pull_ref,
            d0.pull,
            d0.change,
            d0.write,
            None,
        ):
            with self.assertRaises(DaqError):
                self.daq.create(bad_block)  # type: ignore[type-var]

        for bad_attr in (
            "digital0.pull",
            pull_ref,
            d0_ref,
            change_ref,
            write_ref,
            d0,
            d0.change,
            d0.write,
            None,
        ):
            with self.assertRaises(DaqError):
                self.daq.set(
                    bad_attr,  # type: ignore[arg-type]
                    DigitalPull.UP,
                )
            with self.assertRaises(DaqError):
                self.daq.get(bad_attr)  # type: ignore[arg-type]

    def test_digital_block_create_attributes_and_endpoints(self) -> None:
        d1 = self.daq.digital(
            1,
            direction=DigitalDirection.IN,
            pull=DigitalPull.DOWN,
        ).create()
        self.assertIsInstance(d1, DigitalBlock)
        self.assertIsInstance(d1, Block)
        self.assertIsInstance(d1.pin, Attribute)
        self.assertIsInstance(d1.change, Signal)
        self.assertIsInstance(d1.write, Action)

        # Properties and lookups.
        self.assertEqual(d1.pin, d1.attribute("pin"))
        self.assertEqual(d1.direction, d1.attribute("direction"))
        self.assertEqual(d1.initial, d1.attribute("initial"))
        self.assertEqual(d1.pull, d1.attribute("pull"))
        self.assertEqual(d1.change, d1.signal("change"))
        self.assertEqual(d1.risingedge, d1.signal("risingedge"))
        self.assertEqual(d1.fallingedge, d1.signal("fallingedge"))
        self.assertEqual(d1.write, d1.action("write"))
        self.assertEqual(d1.toggle, d1.action("toggle"))
        self.assertEqual(d1.read, d1.action("read"))

        pull_ref = AttributeRef.from_str("digital1.pull")
        self.assertIsNone(d1.pin.input_value_type)
        self.assertEqual(d1.pin.output_value_type, ValueType.PIN)
        self.assertIs(d1.pull.block, d1)
        self.assertEqual(d1.pull.name, "pull")
        self.assertNotEqual(d1.pull, pull_ref)
        self.assertNotEqual(pull_ref, d1.pull)
        self.assertEqual(d1.pull.input_value_type, ValueType.DIGITAL_PULL)
        self.assertEqual(d1.pull.output_value_type, ValueType.DIGITAL_PULL)
        self.assertEqual(d1.pull.spec.mutability, AttributeMutability.MUTABLE)
        self.assertEqual(str(d1.pull), "digital1.pull")
        self.assertEqual(hash(d1.pull), hash(d1.attribute("pull")))
        self.assertNotEqual(d1.pull, "digital1.pull")

        self.assertEqual(d1.pin.get(), "p1")
        self.assertEqual(d1.direction.get(), DigitalDirection.IN)
        self.assertEqual(d1.pull.get(), DigitalPull.DOWN)

        d1.pull.set(DigitalPull.UP)
        self.assertEqual(d1.pull.get(), DigitalPull.UP)

        # Const and init attributes cannot be modified via set().
        with self.assertRaises(DaqError):
            d1.pin.set("p10")
        with self.assertRaises(DaqError):
            d1.direction.set(DigitalDirection.OUT)

        # Invalid attribute/signal/action lookups on Block format valid
        # endpoints with a leading '.'.
        with self.assertRaises(DaqError):
            d1.attribute("  ")
        with self.assertRaises(DaqError) as attr_err:
            d1.attribute("unknown")
        self.assertIn(
            ".direction, .initial, .pin, .pull",
            str(attr_err.exception),
        )
        with self.assertRaises(DaqError):
            d1.signal("")
        with self.assertRaises(DaqError) as signal_err:
            d1.signal("write")
        self.assertIn(
            ".change, .fallingedge, .risingedge",
            str(signal_err.exception),
        )
        with self.assertRaises(DaqError):
            d1.action("")
        with self.assertRaises(DaqError) as action_err:
            d1.action("change")
        self.assertIn(
            ".read, .toggle, .write",
            str(action_err.exception),
        )

        # Non-writable or unknown attributes on Daq.digital() raise TypeError.
        # pylint: disable=unexpected-keyword-arg
        with self.assertRaises(TypeError):
            self.daq.digital(
                2,
                direction=DigitalDirection.IN,
                pin="p2",  # type: ignore[call-arg, unexpected-keyword]
            )
        with self.assertRaises(TypeError):
            self.daq.digital(
                2,
                direction=DigitalDirection.IN,
                slew="fast",  # type: ignore[call-arg, unexpected-keyword]
            )
        # pylint: enable=unexpected-keyword-arg

        # Create validation errors on DigitalBlock.
        with self.assertRaises(DaqError):
            self.daq.digital(2).create()
        with self.assertRaises(DaqError):
            self.daq.digital(2, direction=None).create()
        with self.assertRaises(DaqError):
            self.daq.digital(2, direction=DigitalDirection.IN).create(pin="p2")
        with self.assertRaises(DaqError):
            DigitalBlock(
                2, self.backend, direction=DigitalDirection.IN, pin="p2"
            ).create()
        with self.assertRaises(DaqError):
            self.daq.digital(2, direction=DigitalDirection.IN).create(
                slew="fast"
            )
        with self.assertRaises(DaqError):
            DigitalBlock(
                2, self.backend, direction=DigitalDirection.IN, slew="fast"
            ).create()
        with self.assertRaises(DaqError):
            self.daq.digital(
                2,
                direction=DigitalDirection.IN,
                initial=DigitalState.ACTIVE,
            ).create()
        with self.assertRaises(DaqError):
            self.daq.digital(
                2,
                direction=DigitalDirection.OUT,
                pull=DigitalPull.UP,
            ).create()

        # Default `initial` state is DigitalState.INACTIVE.
        d3 = self.daq.digital(3, direction=DigitalDirection.OUT).create()
        self.assertIs(d3.initial.get(), DigitalState.INACTIVE)
        self.assertIs(d3.read().value, DigitalState.INACTIVE)

        # daq-lib accepts only typed DigitalDirection/DigitalState enums
        # (rejecting raw strings including "high"/"low").
        d4 = self.daq.digital(
            4,
            direction=DigitalDirection.OPENDRAIN,
            initial=DigitalState.ACTIVE,
        ).create()
        self.assertIs(self.daq.get(d4.initial), DigitalState.ACTIVE)
        self.daq.destroy(d4)
        for bad_state in ("active", "inactive", "high", "low"):
            with self.assertRaises(DaqError):
                self.daq.create(
                    self.daq.digital(4, direction=DigitalDirection.OUT),
                    initial=bad_state,
                )

    def test_uart_block_create_attributes_and_endpoints(self) -> None:
        self.backend.set_capabilities(
            {
                "blocks": ["uart0", "uart1"],
                "resources": ["dio0", "dio1", "dio2", "dio3"],
                "mappings": {
                    "p0": "dio0",
                    "p1": "dio1",
                    "p2": "dio2",
                    "p3": "dio3",
                },
                "config": {
                    "uart0": {"attributes": {"txpin": "p0", "rxpin": "p1"}},
                    "uart1": {"attributes": {"txpin": "p2", "rxpin": "p3"}},
                },
            }
        )
        u0 = self.daq.uart(
            0,
            baudrate=115200,
            databits=7,
            parity=UartParity.EVEN,
            stopbits=2,
            loopback=True,
        )
        u0.create()
        self.assertIsInstance(u0, UartBlock)

        self.assertEqual(u0.txpin, u0.attribute("txpin"))
        self.assertEqual(u0.rxpin, u0.attribute("rxpin"))
        self.assertEqual(u0.baudrate, u0.attribute("baudrate"))
        self.assertEqual(u0.databits, u0.attribute("databits"))
        self.assertEqual(u0.parity, u0.attribute("parity"))
        self.assertEqual(u0.stopbits, u0.attribute("stopbits"))
        self.assertEqual(u0.loopback, u0.attribute("loopback"))
        self.assertEqual(u0.rx, u0.signal("rx"))
        self.assertEqual(u0.sent, u0.signal("sent"))
        self.assertEqual(u0.break_signal, u0.signal("break"))
        self.assertEqual(u0.error, u0.signal("error"))
        self.assertEqual(u0.tx, u0.action("tx"))

        self.assertEqual(u0.txpin.get(), "p0")
        self.assertEqual(u0.rxpin.get(), "p1")
        self.assertEqual(u0.baudrate.get(), 115200)
        self.assertEqual(u0.databits.get(), 7)
        self.assertEqual(u0.parity.get(), UartParity.EVEN)
        self.assertEqual(u0.stopbits.get(), 2)
        self.assertIs(u0.loopback.get(), True)

        self.assertEqual(u0.tx.do("a"), ActionResult(success=True))
        self.assertTrue(u0.tx("b"))
        self.assertEqual(self.daq.do(u0.tx, "c"), ActionResult(success=True))

        # ActionResult with failure.
        tx_ref = ActionRef.from_str("uart0.tx")
        self.backend.set_action_result(
            tx_ref,
            ActionResult(
                success=False,
                value="ignored",
                value_type=ValueType.CHAR,
            ),
        )
        failed_res = u0.tx("d")
        self.assertFalse(failed_res)
        self.assertFalse(failed_res.success)
        self.assertIsNone(failed_res.value)
        self.assertIsNone(failed_res.value_type)
        self.backend.set_action_result(tx_ref, None)

        # pylint: disable=unexpected-keyword-arg
        with self.assertRaises(TypeError):
            self.daq.uart(
                1,
                baudrate=115200,
                txpin="p4",  # type: ignore[call-arg, unexpected-keyword]
            )
        with self.assertRaises(TypeError):
            self.daq.uart(
                1,
                baudrate=115200,
                flow_control=True,  # type: ignore[call-arg, unexpected-keyword]
            )
        # pylint: enable=unexpected-keyword-arg
        with self.assertRaises(DaqError):
            self.daq.uart(1, baudrate=115200).create(flow_control=True)
        with self.assertRaises(DaqError):
            UartBlock(
                1, self.backend, baudrate=115200, flow_control=True
            ).create()
        self.assertEqual(
            u0.validate_create_attributes({"baudrate": 9600}),
            {"baudrate": 9600},
        )
        with self.assertRaises(DaqError):
            u0.validate_create_attributes({"baudrate": 0})
        with self.assertRaises(DaqError):
            u0.validate_create_attributes({"baudrate": -9600})

        self.backend.recorded_calls.clear()
        with mock.patch.object(
            self.backend, "create", wraps=self.backend.create
        ) as mock_create:
            with self.assertRaises(DaqError):
                self.daq.uart(1, baudrate=0).create()
            with self.assertRaises(DaqError):
                self.daq.uart(1, baudrate=-9600).create()
            with self.assertRaises(DaqError):
                self.daq.uart(1, baudrate=115200).create(baudrate=0)
            with self.assertRaises(DaqError):
                self.daq.uart(1, baudrate=115200).create(baudrate=-9600)
            mock_create.assert_not_called()
        self.assertEqual(self.backend.recorded_calls, [])
        self.assertNotIn(BlockRef(BlockType.UART, 1), self.backend.blocks)

    def test_signals_and_actions(self) -> None:
        d0 = self.daq.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        d1 = self.daq.digital(
            1,
            direction=DigitalDirection.IN,
        ).create()
        u0 = self.daq.uart(0, baudrate=115200).create()

        # Signal and Action properties.
        change_ref = SignalRef.from_str("digital1.change")
        write_ref = ActionRef.from_str("digital0.write")

        self.assertIs(d1.change.block, d1)
        self.assertEqual(d1.change.name, "change")
        self.assertNotEqual(d1.change, change_ref)
        self.assertNotEqual(change_ref, d1.change)
        self.assertIsNone(d1.change.input_value_type)
        self.assertEqual(d1.change.output_value_type, ValueType.DIGITAL_STATE)
        self.assertEqual(str(d1.change), "digital1.change")
        self.assertEqual(hash(d1.change), hash(d1.signal("change")))
        self.assertNotEqual(d1.change, "digital1.change")

        self.assertIs(d0.write.block, d0)
        self.assertEqual(d0.write.name, "write")
        self.assertNotEqual(d0.write, write_ref)
        self.assertNotEqual(write_ref, d0.write)
        self.assertEqual(d0.write.input_value_type, ValueType.DIGITAL_STATE)
        self.assertIsNone(d0.write.output_value_type)
        self.assertIsNone(d0.read.input_value_type)
        self.assertEqual(d0.read.output_value_type, ValueType.DIGITAL_STATE)
        self.assertEqual(str(d0.write), "digital0.write")
        self.assertEqual(hash(d0.write), hash(d0.action("write")))
        self.assertNotEqual(d0.write, "digital0.write")

        # Signal.on and Daq.on.
        d1.change.on(d0.write)
        self.daq.on(d1.risingedge, d0.write, DigitalState.ACTIVE)
        self.daq.on(d1.risingedge, d0.toggle)
        d1.change.on(d0.read)
        self.assertTrue(self.daq.do(d0.write, DigitalState.INACTIVE))
        self.assertEqual(d0.read().value, DigitalState.INACTIVE)
        self.assertTrue(d0.toggle.do())
        self.assertEqual(d0.read().value, DigitalState.ACTIVE)
        self.assertTrue(d0.toggle())
        self.assertEqual(d0.read().value, DigitalState.INACTIVE)

        with self.assertRaises(DaqError):
            d1.change.on("not_an_action")  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            d1.change.on(write_ref)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            d1.risingedge.on(d0.read, DigitalState.ACTIVE)
        with self.assertRaises(DaqError):
            u0.rx.on(d0.write)
        with self.assertRaises(DaqError):
            self.daq.on(d0.write, d0.read)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            self.daq.on(d1.change, d1.change)  # type: ignore[arg-type]
        with self.assertRaises(DaqError):
            self.daq.do(d1.change)  # type: ignore[arg-type]

    def test_listen_dump_and_device_methods(self) -> None:
        d0 = self.daq.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        d1 = self.daq.digital(
            1,
            direction=DigitalDirection.IN,
        ).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")

        # Signal.listen and Daq.listen.
        occ_ref1 = EventRef(
            timestamp_ns=10,
            endpoint=rx_ref,
            value="x",
            value_type=ValueType.CHAR,
        )
        occ_ref2 = EventRef(
            timestamp_ns=20,
            endpoint=rx_ref,
            value="y",
            value_type=ValueType.CHAR,
        )
        occ1 = Event(
            timestamp_ns=10,
            endpoint=u0.rx,
            value="x",
            value_type=ValueType.CHAR,
        )
        occ2 = Event(
            timestamp_ns=20,
            endpoint=u0.rx,
            value="y",
            value_type=ValueType.CHAR,
        )
        self.assertEqual(occ1.timestamp_ns, 10)
        self.assertEqual(occ1.endpoint, u0.rx)
        self.assertIs(occ1.endpoint, u0.rx)
        self.assertNotEqual(occ1, occ_ref1)
        self.assertNotEqual(occ_ref1, occ1)
        with self.assertRaises(DaqError):
            EventRef(  # type: ignore[call-overload]
                timestamp_ns=10,
                endpoint=u0.rx,  # type: ignore[bad-argument-type]
            )
        with self.assertRaises(DaqError):
            EventRef(
                timestamp_ns=10,
                endpoint=rx_ref,
                value="x",
            )
        with self.assertRaises(DaqError):
            Event(  # type: ignore[call-overload]
                timestamp_ns=10,
                endpoint=rx_ref,  # type: ignore[bad-argument-type]
            )
        with self.assertRaises(DaqError):
            Event(
                timestamp_ns=10,
                endpoint=u0.rx,
                value="x",
            )
        with self.assertRaises(DaqError):
            Event(  # type: ignore[misc]
                timestamp_ns=10,
                endpoint=d1.risingedge,
                value=1,  # type: ignore[call-overload]
            )
        self.backend.queue_event(occ_ref1)
        self.backend.queue_event(occ_ref2)

        received = list(self.daq.listen(u0.rx, count=1, timeout=2.5))
        self.assertEqual(received, [occ1])
        self.assertIsInstance(received[0], Event)
        self.assertIsInstance(received[0].endpoint, Signal)
        self.assertIs(received[0].endpoint, u0.rx)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, 2.5)),
        )
        received_direct = list(u0.rx.listen(count=1))
        self.assertEqual(received_direct, [occ2])
        self.assertIs(received_direct[0].endpoint, u0.rx)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, None)),
        )

        with self.assertRaises(DaqError):
            list(u0.rx.listen(count=0))
        with self.assertRaises(DaqError):
            list(u0.rx.listen(timeout=0))
        with self.assertRaises(DaqError):
            list(u0.rx.listen(timeout=-1.0))
        with self.assertRaises(DaqError):
            self.daq.listen(d0.write)  # type: ignore[arg-type]

        # Dump, clear, info, capabilities, list, destroy, reset.
        dumped = list(self.daq.dump())
        self.assertEqual(dumped, [occ1, occ2])

        info = self.daq.info()
        self.assertEqual(info["backend"], "mock")
        self.assertEqual(info["used"], 2)
        self.assertEqual(info["capacity"], 256)

        self.daq.clear()
        self.assertEqual(list(self.daq.dump()), [])

        caps = self.daq.capabilities()
        self.assertEqual(caps, DEFAULT_MOCK_CAPABILITIES)
        self.assertIn("digital0", caps["blocks"])
        self.assertIn("uart0", caps["blocks"])
        self.assertEqual(caps["mappings"]["p0"], "dio0")
        self.assertEqual(
            caps["config"]["digital0"]["attributes"]["pin"],
            "p0",
        )
        self.assertEqual(
            caps["config"]["uart0"]["attributes"],
            {"txpin": "p5", "rxpin": "p6"},
        )

        self.assertEqual(
            [b.name for b in self.daq.list()],
            ["digital0", "digital1", "uart0"],
        )
        self.daq.destroy(d1)
        self.assertEqual(
            [b.name for b in self.daq.list()],
            ["digital0", "uart0"],
        )

        self.daq.reset()
        self.assertEqual(self.daq.list(), [])

    def test_event_stream_and_wait(self) -> None:
        d0 = self.daq.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")

        # Context-manageable EventStream from listen().
        occ_ref3 = EventRef(
            timestamp_ns=30,
            endpoint=rx_ref,
            value="a",
            value_type=ValueType.CHAR,
        )
        occ_ref4 = EventRef(
            timestamp_ns=40,
            endpoint=rx_ref,
            value="b",
            value_type=ValueType.CHAR,
        )
        occ3 = Event(
            timestamp_ns=30,
            endpoint=u0.rx,
            value="a",
            value_type=ValueType.CHAR,
        )
        occ4 = Event(
            timestamp_ns=40,
            endpoint=u0.rx,
            value="b",
            value_type=ValueType.CHAR,
        )
        self.backend.queue_event(occ_ref3)
        self.backend.queue_event(occ_ref4)

        backend_streams: list[EventStream[EventRef[typing.Any]]] = []
        original_backend_listen = self.backend.listen

        def spy_listen(
            signal_ref: SignalRef,
            count: int | None = None,
            timeout: float | None = None,
        ) -> EventStream[EventRef[typing.Any]]:
            backend_stream = original_backend_listen(
                signal_ref, count=count, timeout=timeout
            )
            backend_streams.append(backend_stream)
            return backend_stream

        with mock.patch.object(self.backend, "listen", side_effect=spy_listen):
            with u0.rx.listen(count=2) as stream:
                self.assertIsInstance(stream, EventStream)
                self.assertFalse(stream.closed)
                self.assertFalse(backend_streams[0].closed)
                first_item = next(stream)
                self.assertEqual(first_item, occ3)
                # Break out of context manager early before consuming occ_ref4.

        self.assertTrue(stream.closed)
        self.assertTrue(backend_streams[0].closed)
        with self.assertRaises(StopIteration):
            next(stream)

        # Custom EventStream on_close callback is invoked once on exit.
        closed_calls: list[str] = []
        custom_stream = EventStream(
            iter([occ4]),
            on_close=lambda: closed_calls.append("closed"),
        )
        with custom_stream as entered_stream:
            self.assertIs(entered_stream, custom_stream)
            self.assertEqual(next(entered_stream), occ4)
        self.assertEqual(closed_calls, ["closed"])
        custom_stream.close()
        self.assertEqual(closed_calls, ["closed"])

        # Signal.wait and Daq.wait.
        # occ_ref4 is still queued in self.backend from earlier early break.
        waited = u0.rx.wait(timeout=1.5)
        self.assertEqual(waited, occ4)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, 1.5)),
        )

        occ_ref5 = EventRef(
            timestamp_ns=50,
            endpoint=rx_ref,
            value="m",
            value_type=ValueType.CHAR,
        )
        occ_ref6 = EventRef(
            timestamp_ns=60,
            endpoint=rx_ref,
            value="z",
            value_type=ValueType.CHAR,
        )
        occ6 = Event(
            timestamp_ns=60,
            endpoint=u0.rx,
            value="z",
            value_type=ValueType.CHAR,
        )
        self.backend.queue_event(occ_ref5)
        self.backend.queue_event(occ_ref6)
        matched = self.daq.wait(
            u0.rx,
            timeout=3.0,
            predicate=lambda occ: occ.value == "z",
        )
        self.assertEqual(matched, occ6)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, None, 3.0)),
        )

        occ_ref7 = EventRef(
            timestamp_ns=70,
            endpoint=rx_ref,
            value="k",
            value_type=ValueType.CHAR,
        )
        occ7 = Event(
            timestamp_ns=70,
            endpoint=u0.rx,
            value="k",
            value_type=ValueType.CHAR,
        )
        self.backend.queue_event(occ_ref7)
        self.assertEqual(self.daq.wait(u0.rx), occ7)

        # wait() raises DaqTimeoutError when stream ends without a match.
        with self.assertRaises(DaqTimeoutError):
            u0.rx.wait(timeout=0.5)

        with self.assertRaises(DaqError):
            self.daq.wait(d0.write)  # type: ignore[arg-type]

    def test_listen_and_wait_argument_validation(self) -> None:
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")
        self.backend.recorded_calls.clear()

        with mock.patch.object(
            self.backend, "listen", wraps=self.backend.listen
        ) as mock_listen:
            bad_int_counts: tuple[int, ...] = (True, False, 0, -1)
            for bad_count in bad_int_counts:
                with self.assertRaises(DaqError):
                    u0.rx.listen(count=bad_count)
                with self.assertRaises(DaqError):
                    self.daq.listen(u0.rx, count=bad_count)

            with self.assertRaises(DaqError):
                u0.rx.listen(count=1.5)  # type: ignore[arg-type]
            with self.assertRaises(DaqError):
                u0.rx.listen(count="1")  # type: ignore[arg-type]
            with self.assertRaises(DaqError):
                self.daq.listen(u0.rx, count=1.5)  # type: ignore[arg-type]
            with self.assertRaises(DaqError):
                self.daq.listen(u0.rx, count="1")  # type: ignore[arg-type]

            bad_num_timeouts: tuple[float, ...] = (
                True,
                False,
                0,
                0.0,
                -1.0,
                float("nan"),
                float("inf"),
                float("-inf"),
            )
            for bad_timeout in bad_num_timeouts:
                with self.assertRaises(DaqError):
                    u0.rx.listen(timeout=bad_timeout)
                with self.assertRaises(DaqError):
                    self.daq.listen(u0.rx, timeout=bad_timeout)
                with self.assertRaises(DaqError) as wait_ctx:
                    u0.rx.wait(timeout=bad_timeout)
                self.assertNotIsInstance(wait_ctx.exception, DaqTimeoutError)
                with self.assertRaises(DaqError) as daq_wait_ctx:
                    self.daq.wait(
                        u0.rx,
                        timeout=bad_timeout,
                        predicate=lambda occ: occ.value == "a",
                    )
                self.assertNotIsInstance(
                    daq_wait_ctx.exception, DaqTimeoutError
                )

            with self.assertRaises(DaqError):
                u0.rx.listen(timeout="1.0")  # type: ignore[arg-type]
            with self.assertRaises(DaqError):
                self.daq.listen(u0.rx, timeout="1.0")  # type: ignore[arg-type]
            with self.assertRaises(DaqError) as str_wait_ctx:
                u0.rx.wait(timeout="1.0")  # type: ignore[arg-type]
            self.assertNotIsInstance(str_wait_ctx.exception, DaqTimeoutError)
            with self.assertRaises(DaqError) as str_daq_wait_ctx:
                self.daq.wait(u0.rx, timeout="1.0")  # type: ignore[arg-type]
            self.assertNotIsInstance(
                str_daq_wait_ctx.exception, DaqTimeoutError
            )

            mock_listen.assert_not_called()
        self.assertEqual(self.backend.recorded_calls, [])

        for idx, ch in enumerate(("a", "b", "c", "d", "e"), start=1):
            self.backend.queue_event(
                EventRef(
                    timestamp_ns=idx * 10,
                    endpoint=rx_ref,
                    value=ch,
                    value_type=ValueType.CHAR,
                )
            )

        res1 = list(u0.rx.listen(count=1, timeout=1))
        self.assertEqual([occ.value for occ in res1], ["a"])
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, 1)),
        )

        res2 = list(self.daq.listen(u0.rx, count=2, timeout=2.5))
        self.assertEqual([occ.value for occ in res2], ["b", "c"])
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 2, 2.5)),
        )

        waited1 = u0.rx.wait(timeout=1)
        self.assertEqual(waited1.value, "d")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, 1)),
        )

        waited2 = self.daq.wait(u0.rx, timeout=2.5)
        self.assertEqual(waited2.value, "e")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (rx_ref, 1, 2.5)),
        )

    def test_wait_predicate_type_annotations_and_inference(self) -> None:
        daq_wait_hints = typing.get_type_hints(Daq.wait)
        signal_wait_hints = typing.get_type_hints(Signal.wait)
        self.assertEqual(
            daq_wait_hints["predicate"],
            signal_wait_hints["predicate"],
        )
        self.assertEqual(
            daq_wait_hints["timeout"], signal_wait_hints["timeout"]
        )
        self.assertEqual(daq_wait_hints["return"], signal_wait_hints["return"])

        predicate_union_args = typing.get_args(daq_wait_hints["predicate"])
        self.assertIn(type(None), predicate_union_args)
        callable_args = [
            arg for arg in predicate_union_args if arg is not type(None)
        ]
        self.assertEqual(len(callable_args), 1)
        callable_hint = callable_args[0]
        self.assertIs(typing.get_origin(callable_hint), Callable)

        param_hints, return_hint = typing.get_args(callable_hint)
        self.assertIs(return_hint, bool)
        self.assertEqual(len(param_hints), 1)
        occ_hint = param_hints[0]
        self.assertIs(typing.get_origin(occ_hint), Event)

        occ_type_args = typing.get_args(occ_hint)
        self.assertEqual(len(occ_type_args), 1)
        occ_type_var = occ_type_args[0]
        self.assertIsInstance(occ_type_var, typing.TypeVar)
        self.assertIsNot(occ_type_var, typing.Any)
        self.assertNotEqual(occ_type_var, typing.Any)
        self.assertEqual(occ_type_var.__name__, "_SignalT")
        self.assertIs(typing.get_origin(daq_wait_hints["signal"]), Signal)
        self.assertEqual(
            typing.get_args(daq_wait_hints["signal"]),
            (occ_type_var,),
        )
        self.assertIs(typing.get_origin(daq_wait_hints["return"]), Event)
        self.assertEqual(
            typing.get_args(daq_wait_hints["return"]),
            (occ_type_var,),
        )

        d1 = self.daq.digital(1, direction=DigitalDirection.IN).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")
        change_ref = SignalRef.from_str("digital1.change")

        self.backend.queue_event(
            EventRef(
                timestamp_ns=10,
                endpoint=rx_ref,
                value="a",
                value_type=ValueType.CHAR,
            )
        )
        self.backend.queue_event(
            EventRef(
                timestamp_ns=20,
                endpoint=rx_ref,
                value="Q",
                value_type=ValueType.CHAR,
            )
        )
        rx_occ: Event[str] = self.daq.wait(
            u0.rx,
            predicate=lambda occ: occ.value is not None and occ.value.isupper(),
        )
        self.assertEqual(
            rx_occ,
            Event(
                timestamp_ns=20,
                endpoint=u0.rx,
                value="Q",
                value_type=ValueType.CHAR,
            ),
        )

        self.backend.queue_event(
            EventRef(
                timestamp_ns=30,
                endpoint=change_ref,
                value=DigitalState.INACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            )
        )
        self.backend.queue_event(
            EventRef(
                timestamp_ns=40,
                endpoint=change_ref,
                value=DigitalState.ACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            )
        )
        change_occ: Event[DigitalState] = self.daq.wait(
            d1.change,
            predicate=lambda occ: occ.value is DigitalState.ACTIVE,
        )
        self.assertEqual(
            change_occ,
            Event(
                timestamp_ns=40,
                endpoint=d1.change,
                value=DigitalState.ACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            ),
        )

    def test_ref_encapsulation_and_inequality(self) -> None:
        d0 = self.daq.digital(0, direction=DigitalDirection.IN).create()
        u0 = self.daq.uart(0, baudrate=115200).create()

        occ_ref = EventRef(
            timestamp_ns=100,
            endpoint=SignalRef.from_str("uart0.rx"),
            value="q",
            value_type=ValueType.CHAR,
        )
        edge_ref = EventRef(
            timestamp_ns=200,
            endpoint=SignalRef.from_str("digital0.risingedge"),
        )
        self.backend.queue_event(occ_ref)
        self.backend.queue_event(edge_ref)

        occ = u0.rx.wait()
        edge = d0.risingedge.wait()

        # Public daq-lib objects do not expose .ref or Daq.bind.
        self.assertFalse(hasattr(self.daq, "bind"))
        for obj in (d0, u0, d0.pull, u0.rx, u0.tx, occ):
            self.assertFalse(hasattr(obj, "ref"))

        # Bound types never compare equal to *Ref types in either direction.
        pairs: list[tuple[object, object]] = [
            (d0, BlockRef(BlockType.DIGITAL, 0)),
            (d0.pull, AttributeRef.from_str("digital0.pull")),
            (u0.rx, SignalRef.from_str("uart0.rx")),
            (u0.tx, ActionRef.from_str("uart0.tx")),
            (occ, occ_ref),
            (edge, edge_ref),
        ]
        for bound_obj, ref_obj in pairs:
            self.assertNotEqual(bound_obj, ref_obj)
            self.assertNotEqual(ref_obj, bound_obj)
            self.assertFalse(bound_obj == ref_obj)
            self.assertFalse(ref_obj == bound_obj)

        # Event and EventRef are frozen/immutable.
        with self.assertRaises(FrozenInstanceError):
            occ.value = "z"  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            occ_ref.value = "z"  # type: ignore[misc]

    def test_multi_daq_provenance_and_hashing(self) -> None:
        daq1 = Daq(MockBackend())
        daq2 = Daq(MockBackend())

        d0_1 = daq1.digital(0, direction=DigitalDirection.IN).create()
        d0_2 = daq2.digital(0, direction=DigitalDirection.IN).create()
        u0_1 = daq1.uart(0, baudrate=115200).create()
        u0_2 = daq2.uart(0, baudrate=115200).create()

        signal_ref = SignalRef.from_str("uart0.rx")
        occ_ref = EventRef(
            timestamp_ns=100,
            endpoint=signal_ref,
            value="q",
            value_type=ValueType.CHAR,
        )
        edge_ref = EventRef(
            timestamp_ns=200,
            endpoint=SignalRef.from_str("digital0.risingedge"),
        )

        assert isinstance(daq1.backend, MockBackend)
        assert isinstance(daq2.backend, MockBackend)
        daq1.backend.queue_event(occ_ref)
        daq1.backend.queue_event(edge_ref)
        daq2.backend.queue_event(occ_ref)
        daq2.backend.queue_event(edge_ref)

        occ1 = u0_1.rx.wait()
        occ2 = u0_2.rx.wait()
        edge1 = d0_1.risingedge.wait()
        edge2 = d0_2.risingedge.wait()

        # Multi-DAQ provenance: identical names/timestamps/values on distinct
        # DAQs compare unequal, while matching instances on the same DAQ match.
        self.assertNotEqual(occ1, occ2)
        self.assertNotEqual(edge1, edge2)

        dumped1 = list(daq1.dump())
        dumped2 = list(daq2.dump())
        self.assertEqual(dumped1, [occ1, edge1])
        self.assertEqual(dumped2, [occ2, edge2])
        self.assertNotEqual(dumped1[0], dumped2[0])
        self.assertNotEqual(dumped1[1], dumped2[1])

        # Verify __eq__ / __hash__ contract in set and dict lookups.
        self.assertEqual(hash(d0_1), hash(daq1.digital(0)))
        self.assertEqual(hash(d0_1.pull), hash(daq1.digital(0).pull))
        self.assertEqual(hash(u0_1.rx), hash(daq1.uart(0).rx))
        self.assertEqual(hash(u0_1.tx), hash(daq1.uart(0).tx))
        self.assertEqual(hash(occ1), hash(dumped1[0]))
        self.assertEqual(hash(edge1), hash(dumped1[1]))

        self.assertEqual(
            len({d0_1, daq1.digital(0), d0_2, BlockRef(BlockType.DIGITAL, 0)}),
            3,
        )
        self.assertEqual(
            len(
                {
                    d0_1.pull,
                    daq1.digital(0).pull,
                    d0_2.pull,
                    AttributeRef.from_str("digital0.pull"),
                }
            ),
            3,
        )
        self.assertEqual(
            len({u0_1.rx, daq1.uart(0).rx, u0_2.rx, signal_ref}),
            3,
        )
        self.assertEqual(
            len(
                {
                    u0_1.tx,
                    daq1.uart(0).tx,
                    u0_2.tx,
                    ActionRef.from_str("uart0.tx"),
                }
            ),
            3,
        )
        self.assertEqual(
            len({occ1, dumped1[0], occ2, occ_ref}),
            3,
        )

        occ_map: dict[object, str] = {
            occ1: "daq1",
            occ2: "daq2",
            occ_ref: "ref",
        }
        self.assertEqual(occ_map[dumped1[0]], "daq1")
        self.assertEqual(occ_map[dumped2[0]], "daq2")
        self.assertEqual(
            occ_map[
                EventRef(
                    timestamp_ns=100,
                    endpoint=signal_ref,
                    value="q",
                    value_type=ValueType.CHAR,
                )
            ],
            "ref",
        )

    def test_cross_backend_validation(self) -> None:
        backend1 = MockBackend()
        backend2 = MockBackend()
        daq1 = Daq(backend1)
        daq2 = Daq(backend2)

        d0_1 = daq1.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        d1_1 = daq1.digital(1, direction=DigitalDirection.IN).create()
        d0_2 = daq2.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        d1_2 = daq2.digital(1, direction=DigitalDirection.IN).create()
        d2_2 = daq2.digital(2, direction=DigitalDirection.IN)
        u0_2 = daq2.uart(0, baudrate=115200)

        backend1.recorded_calls.clear()
        backend2.recorded_calls.clear()

        with self.assertRaises(DaqError):
            daq1.create(d2_2)
        with self.assertRaises(DaqError):
            daq1.create(u0_2, parity=UartParity.EVEN)
        with self.assertRaises(DaqError):
            daq1.destroy(d0_2)
        with self.assertRaises(DaqError):
            daq1.set(d1_2.pull, DigitalPull.UP)
        with self.assertRaises(DaqError):
            daq1.get(d1_2.pull)
        with self.assertRaises(DaqError):
            daq1.get(d0_2.pin)
        with self.assertRaises(DaqError):
            daq1.on(d1_2.change, d0_1.write)
        with self.assertRaises(DaqError):
            daq1.on(d1_1.change, d0_2.write)
        with self.assertRaises(DaqError):
            daq1.on(d1_2.change, d0_2.write)
        with self.assertRaises(DaqError):
            d1_1.change.on(d0_2.write)
        with self.assertRaises(DaqError):
            daq1.listen(d1_2.change)
        with self.assertRaises(DaqError):
            daq1.wait(d1_2.change)
        with self.assertRaises(DaqError):
            daq1.do(d0_2.toggle)
        with self.assertRaises(DaqError):
            daq1.do(d0_2.write, DigitalState.ACTIVE)

        self.assertEqual(backend1.recorded_calls, [])
        self.assertEqual(backend2.recorded_calls, [])
        self.assertIn(BlockRef(BlockType.DIGITAL, 0), backend1.blocks)
        self.assertIn(BlockRef(BlockType.DIGITAL, 0), backend2.blocks)
        self.assertNotIn(BlockRef(BlockType.DIGITAL, 2), backend1.blocks)
        self.assertNotIn(BlockRef(BlockType.DIGITAL, 2), backend2.blocks)
        self.assertNotIn(BlockRef(BlockType.UART, 0), backend1.blocks)
        self.assertNotIn(BlockRef(BlockType.UART, 0), backend2.blocks)
        self.assertEqual(
            backend2.blocks[BlockRef(BlockType.DIGITAL, 1)]["pull"],
            DigitalPull.NONE,
        )
        self.assertEqual(backend1.triggers, [])
        self.assertEqual(backend2.triggers, [])

    def test_event_timestamp_and_action_result_validation(self) -> None:
        d1 = self.daq.digital(1, direction=DigitalDirection.IN).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")
        rising_ref = SignalRef.from_str("digital1.risingedge")

        # Bool literals are statically ints in Python but rejected at runtime.
        for bool_ts in (True, False):
            with self.assertRaises(DaqError):
                EventRef(timestamp_ns=bool_ts, endpoint=rising_ref)
            with self.assertRaises(DaqError):
                Event(timestamp_ns=bool_ts, endpoint=d1.risingedge)

        bad_timestamps: tuple[typing.Any, ...] = (
            True,
            False,
            1.5,
            "10",
            None,
        )
        for bad_ts in bad_timestamps:
            with self.assertRaises(DaqError):
                EventRef(timestamp_ns=bad_ts, endpoint=rising_ref)
            with self.assertRaises(DaqError):
                EventRef(
                    timestamp_ns=bad_ts,
                    endpoint=rx_ref,
                    value="x",
                    value_type=ValueType.CHAR,
                )
            with self.assertRaises(DaqError):
                Event(timestamp_ns=bad_ts, endpoint=d1.risingedge)
            with self.assertRaises(DaqError):
                Event(
                    timestamp_ns=bad_ts,
                    endpoint=u0.rx,
                    value="x",
                    value_type=ValueType.CHAR,
                )

        invalid_vt: typing.Any = "char"
        with self.assertRaises(DaqError):
            EventRef(
                timestamp_ns=10,
                endpoint=rx_ref,
                value="x",
                value_type=invalid_vt,
            )
        with self.assertRaises(DaqError):
            Event(
                timestamp_ns=10,
                endpoint=u0.rx,
                value="x",
                value_type=invalid_vt,
            )

        # ActionResult requires bool `success` and non-None `value_type` when
        # `success` is True and `value` is not None.
        with self.assertRaises(DaqError):
            ActionResult(success=True, value=DigitalState.ACTIVE)
        with self.assertRaises(DaqError):
            ActionResult(
                success=True,
                value=DigitalState.ACTIVE,
                value_type=invalid_vt,
            )

        bad_success_values: tuple[typing.Any, ...] = (1, 0, "true", None)
        for bad_success in bad_success_values:
            with self.assertRaises(DaqError):
                ActionResult(success=bad_success)
            with self.assertRaises(DaqError):
                ActionResult(
                    success=bad_success,
                    value=DigitalState.ACTIVE,
                    value_type=ValueType.DIGITAL_STATE,
                )

        # When `not success` or `value is None`, `value` and `value_type`
        # normalize to None.
        ok_empty = ActionResult(
            success=True, value=None, value_type=ValueType.DIGITAL_STATE
        )
        self.assertTrue(ok_empty)
        self.assertIsNone(ok_empty.value)
        self.assertIsNone(ok_empty.value_type)

        failed_without_vt = ActionResult(
            success=False, value=DigitalState.ACTIVE
        )
        self.assertFalse(failed_without_vt)
        self.assertIsNone(failed_without_vt.value)
        self.assertIsNone(failed_without_vt.value_type)

        ok_active = ActionResult(
            success=True,
            value=DigitalState.ACTIVE,
            value_type=ValueType.DIGITAL_STATE,
        )
        ok_active_dup = ActionResult(
            success=True,
            value=DigitalState.ACTIVE,
            value_type=ValueType.DIGITAL_STATE,
        )
        self.assertEqual(ok_active, ok_active_dup)
        self.assertEqual(hash(ok_active), hash(ok_active_dup))
        self.assertEqual(
            len({ok_empty, failed_without_vt, ok_active, ok_active_dup}),
            3,
        )
        result_map = {ok_active: "active", ok_empty: "empty"}
        self.assertEqual(result_map[ok_active_dup], "active")

        # ActionResult instances are frozen.
        with self.assertRaises(FrozenInstanceError):
            ok_active.success = False  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            ok_active.value = DigitalState.INACTIVE  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            ok_active.value_type = ValueType.INT  # type: ignore[misc]

    def test_event_output_validation_in_listen_wait_and_dump(self) -> None:
        d1 = self.daq.digital(1, direction=DigitalDirection.IN).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        rx_ref = SignalRef.from_str("uart0.rx")
        rising_ref = SignalRef.from_str("digital1.risingedge")

        corrupt_cases: list[tuple[Signal[typing.Any], EventRef[typing.Any]]] = [
            (
                u0.rx,
                EventRef(
                    timestamp_ns=10,
                    endpoint=rx_ref,
                    value=42,
                    value_type=ValueType.INT,
                ),
            ),
            (
                u0.rx,
                EventRef(
                    timestamp_ns=20,
                    endpoint=rx_ref,
                    value="ab",
                    value_type=ValueType.CHAR,
                ),
            ),
            (
                d1.risingedge,
                EventRef(
                    timestamp_ns=30,
                    endpoint=rising_ref,
                    value=1,
                    value_type=ValueType.INT,
                ),
            ),
            (
                u0.rx,
                EventRef(
                    timestamp_ns=40,
                    endpoint=rx_ref,
                    value=None,
                ),
            ),
        ]

        captured_backend_streams: list[EventStream[EventRef[typing.Any]]] = []
        orig_listen = self.backend.listen

        def spy_corrupt_listen(
            signal_ref: SignalRef,
            count: int | None = None,
            timeout: float | None = None,
        ) -> EventStream[EventRef[typing.Any]]:
            b_stream = orig_listen(signal_ref, count=count, timeout=timeout)
            captured_backend_streams.append(b_stream)
            return b_stream

        for signal, bad_occ_ref in corrupt_cases:
            # Signal.listen() and Daq.listen() raise DaqError on invalid output
            # and close both the bound stream and underlying backend stream.
            self.daq.clear()
            captured_backend_streams.clear()
            self.backend.queue_event(bad_occ_ref)

            with mock.patch.object(
                self.backend, "listen", side_effect=spy_corrupt_listen
            ):
                bound_stream = signal.listen(count=1)
                with self.assertRaises(DaqError):
                    list(bound_stream)
                self.assertTrue(bound_stream.closed)
                self.assertEqual(len(captured_backend_streams), 1)
                self.assertTrue(captured_backend_streams[0].closed)
            with self.assertRaises(DaqError):
                list(self.daq.dump())

            self.daq.clear()
            self.backend.queue_event(bad_occ_ref)
            with self.assertRaises(DaqError):
                list(self.daq.listen(signal, count=1))

            # Signal.wait() and Daq.wait() raise DaqError (not DaqTimeoutError).
            self.daq.clear()
            self.backend.queue_event(bad_occ_ref)
            with self.assertRaises(DaqError) as wait_ctx:
                signal.wait(timeout=1.0)
            self.assertNotIsInstance(wait_ctx.exception, DaqTimeoutError)

            self.daq.clear()
            self.backend.queue_event(bad_occ_ref)
            with self.assertRaises(DaqError) as daq_wait_ctx:
                self.daq.wait(signal, timeout=1.0)
            self.assertNotIsInstance(daq_wait_ctx.exception, DaqTimeoutError)

        # Valid 1-char value tagged with ValueType.STR by a backend is
        # normalized to Signal.output_value_type (ValueType.CHAR) without
        # mutating the queued EventRef.
        self.daq.clear()
        str_tagged_ref = EventRef(
            timestamp_ns=50,
            endpoint=rx_ref,
            value="q",
            value_type=ValueType.STR,
        )
        self.backend.queue_event(str_tagged_ref)
        waited_occ = u0.rx.wait(timeout=1.0)
        self.assertEqual(waited_occ.value, "q")
        self.assertEqual(waited_occ.value_type, ValueType.CHAR)
        dumped_occs = list(self.daq.dump())
        self.assertEqual(dumped_occs, [waited_occ])
        self.assertEqual(dumped_occs[0].value_type, ValueType.CHAR)
        self.assertEqual(str_tagged_ref.value_type, ValueType.STR)

    def test_action_do_preserves_backend_action_results(self) -> None:
        d0 = self.daq.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        ).create()
        u0 = self.daq.uart(0, baudrate=115200).create()
        read_ref = ActionRef.from_str("digital0.read")
        write_ref = ActionRef.from_str("digital0.write")

        stored_read = ActionResult(
            success=True,
            value=DigitalState.ACTIVE,
            value_type=ValueType.DIGITAL_STATE,
        )
        self.backend.set_action_result(read_ref, stored_read)
        read_res = d0.read.do()
        self.assertEqual(read_res, stored_read)
        self.assertIs(self.backend.action_results[read_ref], stored_read)
        self.assertEqual(stored_read.value, DigitalState.ACTIVE)
        self.assertEqual(stored_read.value_type, ValueType.DIGITAL_STATE)

        # Action whose spec expects ValueType.CHAR normalizes a ValueType.STR
        # backend result by returning a new ActionResult without mutating the
        # stored instance.
        echo_action: Action[None, str] = Action(
            u0, "echo", ActionSpec(output_value_type=ValueType.CHAR)
        )
        echo_ref = ActionRef.from_str("uart0.echo")
        stored_echo = ActionResult(
            success=True,
            value="z",
            value_type=ValueType.STR,
        )
        self.backend.set_action_result(
            echo_ref, stored_echo, allow_custom_endpoint=True
        )
        echo_res = echo_action.do()
        self.assertIsNot(echo_res, stored_echo)
        self.assertEqual(
            echo_res,
            ActionResult(
                success=True,
                value="z",
                value_type=ValueType.CHAR,
            ),
        )
        self.assertIs(self.backend.action_results[echo_ref], stored_echo)
        self.assertEqual(stored_echo.value_type, ValueType.STR)

        # Invalid ActionResult outputs raise DaqError and leave stored results
        # untouched.
        bad_read_none = ActionResult(success=True)
        self.backend.set_action_result(read_ref, bad_read_none)
        with self.assertRaises(DaqError):
            d0.read.do()
        self.assertIs(self.backend.action_results[read_ref], bad_read_none)

        bad_read_int = ActionResult(
            success=True, value=1, value_type=ValueType.INT
        )
        self.backend.set_action_result(read_ref, bad_read_int)
        with self.assertRaises(DaqError):
            d0.read.do()
        self.assertIs(self.backend.action_results[read_ref], bad_read_int)

        bad_write_output = ActionResult(
            success=True,
            value=DigitalState.ACTIVE,
            value_type=ValueType.DIGITAL_STATE,
        )
        self.backend.set_action_result(write_ref, bad_write_output)
        with self.assertRaises(DaqError):
            d0.write.do(DigitalState.INACTIVE)
        self.assertIs(self.backend.action_results[write_ref], bad_write_output)


class DaqLifecycleAndBackendIntegrationTest(unittest.TestCase):
    """Tests for DaqBackend ABC defaults and Daq behavior with a backend."""

    def test_daq_backend_abc_lifecycle_and_context_manager(self) -> None:
        class MinimalBackend(DaqBackend):
            """Minimal DaqBackend exercising default lifecycle hooks."""

            def name(self) -> str:
                return "minimal"

            def create(
                self, block_ref: BlockRef, **attributes: typing.Any
            ) -> BlockRef:
                del attributes
                return block_ref

            def list(self) -> Iterable[BlockRef]:
                return ()

            def destroy(self, block_ref: BlockRef) -> None:
                del block_ref

            def set(self, attribute_ref: AttributeRef, value: Value) -> None:
                del attribute_ref, value

            def get(self, attribute_ref: AttributeRef) -> Value:
                del attribute_ref
                return None

            def on(
                self,
                signal_ref: SignalRef,
                action_ref: ActionRef,
                action_arg: Value = None,
            ) -> None:
                del signal_ref, action_ref, action_arg

            def listen(
                self,
                signal_ref: SignalRef,
                count: int | None = None,
                timeout: float | None = None,
            ) -> EventStream[EventRef[typing.Any]]:
                del signal_ref, count, timeout
                return EventStream(iter(()))

            def do(
                self, action_ref: ActionRef, action_arg: Value = None
            ) -> ActionResult[typing.Any]:
                del action_ref, action_arg
                return ActionResult(success=True)

            def dump(self) -> Iterable[EventRef[typing.Any]]:
                return ()

            def clear(self) -> None:
                pass

            def info(self) -> Mapping[str, typing.Any]:
                return {}

            def capabilities(self) -> Mapping[str, typing.Any]:
                return {}

            def reset(self) -> None:
                pass

        # Base configure(), open(), and close() are safe non-abstract no-ops,
        # and DaqBackend.__enter__ / __exit__ delegate to open() and close().
        minimal = MinimalBackend()
        self.assertEqual(minimal.name(), "minimal")
        minimal.configure({"device": "ignored"})
        with (
            mock.patch.object(minimal, "open", wraps=minimal.open) as m_open,
            mock.patch.object(minimal, "close", wraps=minimal.close) as m_close,
        ):
            with minimal as entered_minimal:
                self.assertIs(entered_minimal, minimal)
                m_open.assert_called_once_with()
                m_close.assert_not_called()
            m_close.assert_called_once_with()

        # Daq context manager delegates open() and close() even on exception.
        backend = MockBackend()
        daq = Daq(backend)
        with self.assertRaises(RuntimeError):
            with daq:
                self.assertTrue(backend.opened)
                self.assertFalse(backend.closed)
                raise RuntimeError("boom")
        self.assertTrue(backend.closed)

    def test_daq_info_capabilities_and_bed_constraints(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)
        self.assertEqual(daq.info()["device"], "mock")
        self.assertEqual(daq.info()["config"], "none")
        self.assertEqual(daq.capabilities(), DEFAULT_MOCK_CAPABILITIES)

        custom_caps: dict[str, typing.Any] = {
            "blocks": ["digital0", "uart0"],
            "resources": ["gp14", "gp4", "gp5"],
            "mappings": {"p0": "gp14", "p2": "gp4", "p3": "gp5"},
            "config": {
                "digital0": {
                    "attributes": {
                        "pin": "p0",
                        "direction": ["out", "opendrain"],
                        "pull": ["none", "up"],
                    }
                },
                "uart0": {
                    "attributes": {
                        "txpin": "p2",
                        "rxpin": "p3",
                        "baudrate": [9600, 115200],
                        "databits": [8],
                    }
                },
            },
        }
        backend.set_device_config(
            {
                "device": "mock_board",
                "config_path": "/tmp/daq.yaml",
                "capabilities": custom_caps,
            }
        )
        info = daq.info()
        self.assertEqual(info["device"], "mock_board")
        self.assertEqual(info["config"], str(Path("/tmp/daq.yaml")))
        self.assertEqual(daq.capabilities(), custom_caps)

        # BED constraints and virtual pin mappings are surfaced through Daq.
        with self.assertRaises(DaqError):
            daq.digital(0, direction=DigitalDirection.IN).create()
        d0 = daq.digital(
            0,
            direction=DigitalDirection.OPENDRAIN,
            pull=DigitalPull.UP,
        ).create()
        with self.assertRaises(DaqError):
            d0.pull.set(DigitalPull.DOWN)
        d0.pull.set(DigitalPull.NONE)
        self.assertIs(d0.pull.get(), DigitalPull.NONE)

        with self.assertRaises(DaqError):
            daq.uart(0, baudrate=57600).create()
        with self.assertRaises(DaqError):
            daq.uart(0, baudrate=115200, databits=7).create()
        u0 = daq.uart(0, baudrate=115200).create()
        self.assertEqual(u0.txpin.get(), "p2")
        self.assertEqual(u0.rxpin.get(), "p3")
        self.assertEqual(u0.databits.get(), 8)

    def test_daq_operations_require_created_blocks(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)
        d0 = daq.digital(0, direction=DigitalDirection.OUT)
        d1 = daq.digital(1, direction=DigitalDirection.IN)
        u0 = daq.uart(0, baudrate=115200)

        # Operations on uncreated blocks raise DaqError through Daq and bound
        # endpoints without mutating backend state.
        with self.assertRaises(DaqError):
            daq.get(d0.pin)
        with self.assertRaises(DaqError):
            daq.set(d1.pull, DigitalPull.UP)
        with self.assertRaises(DaqError):
            daq.do(d0.write, DigitalState.ACTIVE)
        with self.assertRaises(DaqError):
            d0.toggle()
        with self.assertRaises(DaqError):
            d0.read()
        with self.assertRaises(DaqError):
            u0.tx("a")
        with self.assertRaises(DaqError):
            daq.on(d1.change, d0.write)
        with self.assertRaises(DaqError):
            daq.listen(u0.rx)
        with self.assertRaises(DaqError):
            daq.wait(u0.rx, timeout=0.1)
        with self.assertRaises(DaqError):
            daq.destroy(d0)
        self.assertEqual(backend.pin_states, {})

        # Once created, valid operations succeed while mode-invalid endpoints
        # fail.
        d0.create()
        d1.create()
        u0.create()

        with self.assertRaises(DaqError):
            daq.set(d0.pull, DigitalPull.UP)
        with self.assertRaises(DaqError):
            d1.write(DigitalState.ACTIVE)
        with self.assertRaises(DaqError):
            d1.toggle()
        with self.assertRaises(DaqError):
            daq.listen(d0.risingedge)
        with self.assertRaises(DaqError):
            daq.on(d0.fallingedge, d0.toggle)

        # After destroy or reset, operations on torn-down blocks fail again.
        daq.destroy(d0)
        with self.assertRaises(DaqError):
            d0.read()
        daq.reset()
        with self.assertRaises(DaqError):
            u0.tx("a")

    def test_daq_conflicts_and_duplicate_block_creation(self) -> None:
        d0_ref = BlockRef(BlockType.DIGITAL, 0)
        d1_ref = BlockRef(BlockType.DIGITAL, 1)
        u0_ref = BlockRef(BlockType.UART, 0)

        caller_set = {d1_ref}
        backend = MockBackend(conflicts={d0_ref: caller_set})
        backend.add_conflict(d0_ref, u0_ref)
        self.assertEqual(caller_set, {d1_ref})
        caller_set.clear()

        daq = Daq(backend)
        d0 = daq.digital(0, direction=DigitalDirection.OUT)
        d1 = daq.digital(1, direction=DigitalDirection.IN)
        u0 = daq.uart(0, baudrate=115200)

        # Duplicate creation of an already-created block raises DaqError.
        daq.create(d1)
        with self.assertRaises(DaqError):
            daq.create(d1)

        # Bidirectional conflict: d1 blocks d0 until d1 is destroyed.
        with self.assertRaises(DaqError):
            daq.create(d0)
        daq.destroy(d1)
        daq.create(d0)
        with self.assertRaises(DaqError):
            daq.create(u0)

        # Shared hardware resources in capabilities block conflicting create().
        backend.set_capabilities(
            {
                "blocks": ["digital0", "uart0"],
                "resources": ["gp4", "gp5"],
                "mappings": {"p0": "gp4", "p1": "gp4", "p2": "gp5"},
                "config": {
                    "digital0": {"attributes": {"pin": "p0"}},
                    "uart0": {"attributes": {"txpin": "p1", "rxpin": "p2"}},
                },
            }
        )
        d0_in = daq.digital(0, direction=DigitalDirection.IN).create()
        with self.assertRaises(DaqError) as ctx:
            daq.uart(0, baudrate=115200).create()
        self.assertIn("digital0", str(ctx.exception))
        daq.destroy(d0_in)
        daq.uart(0, baudrate=115200).create()
        self.assertEqual([b.name for b in daq.list()], ["uart0"])

    def test_daq_propagates_unsupported_backend_commands(self) -> None:
        all_commands = {
            "create",
            "list",
            "destroy",
            "set",
            "get",
            "on",
            "listen",
            "do",
            "dump",
            "clear",
            "info",
            "capabilities",
            "reset",
        }
        backend = MockBackend(unsupported_commands=all_commands)
        backend.setup_block(
            "digital0", {"direction": DigitalDirection.OPENDRAIN}
        )
        daq = Daq(backend)
        d0 = daq.digital(0, direction=DigitalDirection.OPENDRAIN)
        d1 = daq.digital(1, direction=DigitalDirection.IN)

        calls: list[Callable[[], typing.Any]] = [
            lambda: daq.create(d1),
            daq.list,
            lambda: daq.destroy(d0),
            lambda: daq.set(d0.pull, DigitalPull.NONE),
            lambda: daq.get(d0.pin),
            lambda: daq.on(d0.change, d0.toggle),
            lambda: daq.listen(d0.change),
            lambda: daq.do(d0.toggle),
            daq.dump,
            daq.clear,
            daq.info,
            daq.capabilities,
            daq.reset,
        ]
        for fn in calls:
            with self.assertRaises(DaqError):
                fn()

    def test_daq_validates_corrupt_backend_attribute_state(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)

        # Pre-populate block state via test-only setup helpers without recording
        # 'create' calls, then exercise reads through Daq.
        backend.setup_block(
            "digital0",
            {"direction": DigitalDirection.IN, "pull": DigitalPull.UP},
            pin_state=DigitalState.ACTIVE,
        )
        self.assertEqual(backend.recorded_calls, [])
        d0 = daq.digital(0)
        self.assertEqual(daq.get(d0.pull), DigitalPull.UP)
        self.assertEqual(daq.do(d0.read).value, DigitalState.ACTIVE)

        backend.set_pin_state("digital0", DigitalState.INACTIVE)
        self.assertEqual(d0.read().value, DigitalState.INACTIVE)

        # Corrupt attribute values injected via test-only helpers are caught by
        # daq-lib output validation.
        backend.set_raw_attribute("digital0.pull", "invalid_raw_pull")
        with self.assertRaises(DaqError):
            d0.pull.get()
        with self.assertRaises(DaqError):
            daq.get(d0.pull)

        backend.setup_block(
            "uart0",
            {"txpin": "not_a_virtual_pin", "baudrate": "bad"},
            validate=False,
        )
        u0 = daq.uart(0)
        with self.assertRaises(DaqError):
            u0.txpin.get()
        with self.assertRaises(DaqError):
            u0.baudrate.get()

    def test_digital_opendrain_mode_via_daq_lib(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)

        # Defaults in OPENDRAIN mode: initial=INACTIVE, pull=NONE.
        od_default = daq.digital(
            0, direction=DigitalDirection.OPENDRAIN
        ).create()
        self.assertIs(od_default.direction.get(), DigitalDirection.OPENDRAIN)
        self.assertIs(od_default.initial.get(), DigitalState.INACTIVE)
        self.assertIs(od_default.pull.get(), DigitalPull.NONE)
        self.assertEqual(
            od_default.read(),
            ActionResult(
                success=True,
                value=DigitalState.INACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            ),
        )

        # Explicit initial + pull, runtime pull.set(), and write/toggle/read.
        od = daq.digital(
            1,
            direction=DigitalDirection.OPENDRAIN,
            initial=DigitalState.ACTIVE,
            pull=DigitalPull.UP,
        ).create()
        self.assertIs(od.initial.get(), DigitalState.ACTIVE)
        self.assertIs(od.pull.get(), DigitalPull.UP)
        self.assertEqual(od.read().value, DigitalState.ACTIVE)

        od.pull.set(DigitalPull.DOWN)
        self.assertIs(od.pull.get(), DigitalPull.DOWN)

        self.assertTrue(od.toggle())
        self.assertEqual(od.read().value, DigitalState.INACTIVE)
        self.assertTrue(od.write(DigitalState.ACTIVE))
        self.assertEqual(od.read().value, DigitalState.ACTIVE)


class LoggingReprAndImmutabilityTest(unittest.TestCase):
    """Tests for lib.py logging, __repr__, and immutable endpoint mappings."""

    def test_class_level_endpoint_mappings_are_immutable(self) -> None:
        for cls in (Block, DigitalBlock, UartBlock):
            for mapping_attr in ("ATTRIBUTES", "SIGNALS", "ACTIONS"):
                mapping = getattr(cls, mapping_attr)
                self.assertIsInstance(mapping, types.MappingProxyType)
                with self.assertRaises(TypeError):
                    mapping["mutated"] = None  # type: ignore[index]

    def test_repr_on_daq_block_and_endpoints(self) -> None:
        daq = Daq(MockBackend())
        d1 = daq.digital(1)
        self.assertEqual(repr(daq), "Daq(backend='mock')")
        self.assertEqual(repr(d1), "DigitalBlock(index=1, backend='mock')")
        self.assertEqual(
            repr(d1.pull), "Attribute('digital1.pull', backend='mock')"
        )
        self.assertEqual(
            repr(d1.change), "Signal('digital1.change', backend='mock')"
        )
        self.assertEqual(
            repr(d1.write), "Action('digital1.write', backend='mock')"
        )

    def test_lib_logging_levels_and_no_mock_backend_logger(self) -> None:
        self.assertFalse(hasattr(mock_backend_mod, "_LOG"))

        backend = MockBackend()
        daq = Daq(backend)
        logger_name = lib_mod.__name__

        with self.assertLogs(logger_name, level="DEBUG") as debug_logs:
            with daq:
                d0 = daq.digital(
                    0,
                    direction=DigitalDirection.OUT,
                    initial=DigitalState.INACTIVE,
                ).create()
                d1 = daq.digital(1, direction=DigitalDirection.IN).create()
                _ = d1.pull.get()
                d1.pull.set(DigitalPull.UP)
                d1.change.on(d0.write)
                d0.write(DigitalState.ACTIVE)
                daq.clear()
                daq.reset()

        joined = "\n".join(debug_logs.output)
        self.assertIn(f"INFO:{logger_name}:Opening DAQ backend 'mock'", joined)
        self.assertIn(f"INFO:{logger_name}:Creating block 'digital0'", joined)
        self.assertIn(
            f"INFO:{logger_name}:Setting attribute 'digital1.pull'", joined
        )
        self.assertIn(f"INFO:{logger_name}:Registering trigger", joined)
        self.assertIn(
            f"INFO:{logger_name}:Invoking action 'digital0.write'", joined
        )
        self.assertIn(f"INFO:{logger_name}:Clearing event logs", joined)
        self.assertIn(f"INFO:{logger_name}:Resetting DAQ device", joined)
        self.assertIn(f"INFO:{logger_name}:Closing DAQ backend 'mock'", joined)
        self.assertIn(f"DEBUG:{logger_name}:", joined)
        self.assertNotIn(f"ERROR:{logger_name}:", joined)

        # Action failure logs WARNING without raising DaqError.
        backend.setup_block("digital0", {"direction": DigitalDirection.OUT})
        backend.set_action_result("digital0.write", ActionResult(success=False))
        with self.assertLogs(logger_name, level="WARNING") as warn_logs:
            res = daq.digital(0).write(DigitalState.ACTIVE)
        self.assertFalse(res.success)
        self.assertTrue(
            any("reported failure" in msg for msg in warn_logs.output)
        )


if __name__ == "__main__":
    unittest.main()
