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
"""Unit tests for pw_daq backend, daq-lib, daq-cli, and mock backend."""

# pylint: disable=missing-function-docstring,protected-access

from __future__ import annotations

from collections.abc import Iterator, Mapping
import contextlib
import importlib.metadata
import io
import json
import logging
import os
from pathlib import Path
import tempfile
from typing import Any
import unittest
from unittest import mock

from pw_cli import log as pw_cli_log, plugins
from pw_daq import (
    backend_registry as backend_registry_mod,
    cli as cli_mod,
    cli_parsing as cli_parsing_mod,
    config as config_mod,
    lib as lib_mod,
)
from pw_daq.backend import DaqBackend
from pw_daq.backend_registry import (
    ENTRY_POINT_GROUP,
    BackendRegistry,
    BackendTarget,
)
from pw_daq.backends.mock_daq import mock_backend as mock_backend_mod
from pw_daq.backends.mock_daq.mock_backend import MockBackend
from pw_daq.cli import run_cli
from pw_daq.cli_parsing import (
    format_event_cli,
    format_event_json,
    parse_action,
    parse_attribute,
    parse_block,
    parse_enum_member,
    parse_signal,
)
from pw_daq.common import (
    ActionRef,
    ActionResult,
    AttributeRef,
    BlockRef,
    BlockType,
    DaqError,
    DigitalDirection,
    DigitalPull,
    DigitalState,
    SignalRef,
    EventRef,
    EventStream,
    UartParity,
    ValueType,
)
from pw_daq.lib import (
    Action,
    Attribute,
    Block,
    Daq,
    DigitalBlock,
    Signal,
    Event,
    UartBlock,
)
import yaml


def _run_cli_captured(
    argv: list[str],
    backend: DaqBackend | None = None,
    *,
    registry: BackendRegistry | None = None,
    extra_backends: Mapping[str, BackendTarget] | None = None,
) -> tuple[int, str, str]:
    """Runs run_cli and returns (return_code, stdout, stderr)."""
    out = io.StringIO()
    err = io.StringIO()
    prev_stream = pw_cli_log._STDERR_HANDLER.setStream(err)
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = run_cli(
                argv,
                backend=backend,
                registry=registry,
                extra_backends=extra_backends,
            )
    finally:
        if prev_stream is not None:
            pw_cli_log._STDERR_HANDLER.setStream(prev_stream)
    return rc, out.getvalue(), err.getvalue()


_MINIMAL_MOCK_CAPABILITIES: dict[str, Any] = {
    "blocks": ["digital0"],
    "resources": ["dio0"],
    "mappings": {"p0": "dio0"},
    "config": {
        "digital0": {"attributes": {"pin": "p0"}},
    },
}


class CustomPluginBackend(MockBackend):
    """Test backend loaded dynamically via module and class name."""

    last_instance: CustomPluginBackend | None = None

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        CustomPluginBackend.last_instance = self


class PreconfiguredDigitalOutBackend(CustomPluginBackend):
    """Test backend with 'digital0' pre-created as an output block."""

    def configure(self, config: Any = None) -> None:
        super().configure(config)
        self.setup_block("digital0", {"direction": DigitalDirection.OUT})


class ConfigRequiredCapsBackend(MockBackend):
    """Backend whose capabilities require configure() but not open()."""

    def capabilities(self) -> dict[str, Any]:
        if self.device is None:
            raise DaqError("Backend not configured yet.")
        if self.opened:
            raise DaqError("capabilities() must not require open().")
        return dict(super().capabilities())


class DevRequiredOnOpenBackend(MockBackend):
    """Backend that requires 'dev' in configuration when open() is called."""

    def open(self) -> None:
        if not self.config.get("dev"):
            raise DaqError("Missing 'dev' connection configuration on open().")
        super().open()


class BackendAndLibLayerTest(unittest.TestCase):
    """Tests for Layer 3 (backend) and Layer 2 (daq-lib)."""

    def test_abstract_daq_backend_cannot_be_instantiated_directly(self) -> None:
        class IncompleteBackend(DaqBackend):
            pass

        with self.assertRaises(TypeError):
            # pylint: disable=abstract-class-instantiated
            IncompleteBackend()  # type: ignore[abstract]
            # pylint: enable=abstract-class-instantiated

    def test_open_and_close_are_non_abstract_noops(self) -> None:
        daq = Daq(MockBackend())
        with daq as entered:
            self.assertIs(entered, daq)

    def test_parse_valid_blocks(self) -> None:
        d0 = BlockRef.from_str("digital0")
        self.assertIsInstance(d0, BlockRef)
        self.assertEqual(d0, BlockRef(BlockType.DIGITAL, 0))
        self.assertEqual(hash(d0), hash(BlockRef(BlockType.DIGITAL, 0)))
        self.assertEqual(d0.block_type, BlockType.DIGITAL)
        self.assertEqual(d0.index, 0)
        self.assertEqual(str(d0), "digital0")

        u12 = BlockRef.from_str("uart12")
        self.assertIsInstance(u12, BlockRef)
        self.assertEqual(u12, BlockRef(BlockType.UART, 12))
        self.assertEqual(hash(u12), hash(BlockRef(BlockType.UART, 12)))
        self.assertEqual(u12.block_type, BlockType.UART)
        self.assertEqual(u12.index, 12)
        self.assertEqual(str(u12), "uart12")

    def test_ref_models_and_daq_calls(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)
        d1_block = daq.digital(
            1,
            direction=DigitalDirection.IN,
            pull=DigitalPull.DOWN,
        ).create()
        d1 = BlockRef(BlockType.DIGITAL, 1)
        pull_ref = AttributeRef(d1, "pull")
        self.assertEqual(str(pull_ref), "digital1.pull")
        self.assertEqual(daq.get(d1_block.pull), DigitalPull.DOWN)
        daq.set(d1_block.pull, DigitalPull.UP)
        self.assertEqual(daq.get(d1_block.pull), DigitalPull.UP)
        self.assertIn(
            ("set", (pull_ref, DigitalPull.UP)), backend.recorded_calls
        )
        change_ref = SignalRef(d1, "change")
        self.assertEqual(str(change_ref), "digital1.change")
        self.assertEqual(change_ref.endpoint, "change")
        write_ref = ActionRef(d1, "write")
        self.assertEqual(str(write_ref), "digital1.write")
        self.assertEqual(write_ref.endpoint, "write")
        read_ref = ActionRef(d1, "read")
        self.assertEqual(str(read_ref), "digital1.read")
        self.assertEqual(read_ref.endpoint, "read")

        u0_block = daq.uart(0, baudrate=115200).create()
        u0 = BlockRef(BlockType.UART, 0)
        baud_ref = AttributeRef(u0, "baudrate")
        self.assertEqual(daq.get(u0_block.baudrate), 115200)
        self.assertIn(("get", baud_ref), backend.recorded_calls)
        rx_ref = SignalRef(u0, "rx")
        self.assertEqual(rx_ref.endpoint, "rx")
        tx_ref = ActionRef(u0, "tx")
        self.assertEqual(tx_ref.endpoint, "tx")

        with self.assertRaises(DaqError):
            parse_enum_member(
                DigitalState, "invalid_state", context="digital state"
            )

    def test_daq_lib_programmatic_api(self) -> None:
        backend = MockBackend()
        daq = Daq(backend)
        self.assertIs(daq.backend, backend)
        self.assertIsInstance(
            daq._bind(BlockRef(BlockType.DIGITAL, 0)), DigitalBlock
        )
        self.assertIsInstance(daq._bind(BlockRef(BlockType.UART, 0)), UartBlock)

        d1 = daq.digital(
            1,
            direction=DigitalDirection.IN,
            pull=DigitalPull.DOWN,
        ).create()
        self.assertIsInstance(d1, DigitalBlock)
        self.assertIsInstance(d1, Block)
        self.assertIsInstance(d1.pull, Attribute)
        self.assertIsInstance(d1.change, Signal)
        self.assertIsInstance(d1.write, Action)
        self.assertEqual(d1.pin.get(), "p1")
        self.assertEqual(d1.direction.get(), DigitalDirection.IN)
        self.assertEqual(d1.pull.get(), DigitalPull.DOWN)

        d1.pull.set(DigitalPull.UP)
        self.assertEqual(d1.pull.get(), DigitalPull.UP)

        # Const and init attributes cannot be modified at runtime via set().
        with self.assertRaises(DaqError):
            d1.pin.set("p99")
        with self.assertRaises(DaqError):
            d1.direction.set(DigitalDirection.OUT)

        d0 = daq.digital(
            0,
            direction=DigitalDirection.OUT,
            initial=DigitalState.INACTIVE,
        )
        d0.create()
        self.assertIsInstance(d0, DigitalBlock)
        self.assertTrue(d0.write.do(DigitalState.ACTIVE))
        self.assertEqual(
            d0.read(),
            ActionResult(
                success=True,
                value=DigitalState.ACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            ),
        )

        # Connect d1.change -> d0.write via Signal.on().
        d1.change.on(d0.write)
        self.assertEqual(
            backend.recorded_calls[-1],
            ("on", (d1.change._ref, d0.write._ref, None)),
        )

        u0 = daq.uart(0, baudrate=115200, loopback=True).create()
        self.assertIsInstance(u0, UartBlock)
        self.assertEqual(u0.baudrate.get(), 115200)
        self.assertTrue(u0.loopback.get())
        self.assertTrue(u0.tx.do("x"))

        # Type mismatch in Signal.on() raises DaqError.
        with self.assertRaises(DaqError):
            u0.rx.on(d0.write)

        # Listing and destroying blocks via Daq wrapper.
        blocks = daq.list()
        self.assertEqual(
            [str(b) for b in blocks], ["digital1", "digital0", "uart0"]
        )
        d1.destroy()
        self.assertEqual([str(b) for b in daq.list()], ["digital0", "uart0"])

    def test_parse_todo_and_invalid_blocks(self) -> None:
        daq = Daq(MockBackend())
        for todo_block in (
            "adc0",
            "dac0",
            "pwm1",
            "spi0",
            "i2c2",
            "timer0",
            "stopwatch0",
            "lut0",
        ):
            ref = BlockRef.from_str(todo_block)
            self.assertEqual(str(ref), todo_block)
            with self.assertRaises(DaqError):
                parse_block(daq, todo_block)

        for invalid in (
            "digital",
            "uart",
            "analog0",
            "gpio0",
            "foo0",
            "digital-1",
            "",
        ):
            with self.assertRaises(DaqError):
                BlockRef.from_str(invalid)

    def test_parse_valid_and_invalid_attributes(self) -> None:
        daq = Daq(MockBackend())
        attr1 = parse_attribute(daq, "digital1.pull")._ref
        self.assertEqual(attr1, AttributeRef.from_str("digital1.pull"))
        self.assertEqual(attr1.block_name, "digital1")
        self.assertEqual(attr1.attribute, "pull")
        self.assertEqual(str(attr1), "digital1.pull")

        attr2 = parse_attribute(daq, "uart0.baudrate")._ref
        self.assertEqual(attr2, AttributeRef.from_str("uart0.baudrate"))
        self.assertEqual(attr2.block_name, "uart0")
        self.assertEqual(attr2.attribute, "baudrate")

        for invalid in (
            "digital0",
            "digital0.unknown",
            "digital0.slew",
            "digital0.strength",
            "uart0.format",
            ".pull",
            "digital0.",
            "",
        ):
            with self.assertRaises(DaqError):
                parse_attribute(daq, invalid)

    def test_parse_valid_endpoints(self) -> None:
        daq = Daq(MockBackend())
        ev1 = parse_signal(daq, "uart0.rx")
        self.assertEqual(ev1._ref, SignalRef.from_str("uart0.rx"))
        self.assertEqual(ev1._ref.block_name, "uart0")
        self.assertEqual(ev1._ref.signal, "rx")
        self.assertEqual(str(ev1._ref), "uart0.rx")
        self.assertIsNone(ev1.input_value_type)
        self.assertEqual(ev1.output_value_type, ValueType.CHAR)

        ev2 = parse_signal(daq, "digital1.change")
        self.assertEqual(ev2._ref, SignalRef.from_str("digital1.change"))
        self.assertEqual(ev2._ref.block_name, "digital1")
        self.assertEqual(ev2._ref.signal, "change")
        self.assertIsNone(ev2.input_value_type)
        self.assertEqual(ev2.output_value_type, ValueType.DIGITAL_STATE)

        # digital.read is an Action (not a Signal) that returns DigitalState.
        read_action = parse_action(daq, "digital0.read")
        self.assertEqual(read_action._ref, ActionRef.from_str("digital0.read"))
        self.assertIsNone(read_action.input_value_type)
        self.assertEqual(read_action.output_value_type, ValueType.DIGITAL_STATE)
        with self.assertRaises(DaqError):
            parse_signal(daq, "digital0.read")
        with self.assertRaises(DaqError):
            parse_action(daq, "digital1.change")

    def test_parse_invalid_endpoints(self) -> None:
        daq = Daq(MockBackend())
        for invalid in (
            "uart0",
            "digital0.unknown",
            "digital0.tx",
            "uart0.write",
            "gpio0.set",
            ".rx",
            "uart0.",
            "",
        ):
            with self.assertRaises(DaqError):
                parse_signal(daq, invalid)
            with self.assertRaises(DaqError):
                parse_action(daq, invalid)

    def test_event_formatting(self) -> None:
        daq = Daq(MockBackend())
        occ_enum = Event(
            timestamp_ns=1234,
            endpoint=daq.digital(1).change,
            value=DigitalState.ACTIVE,
            value_type=ValueType.DIGITAL_STATE,
        )
        self.assertEqual(
            format_event_cli(occ_enum),
            "[1234] digital1.change: active",
        )
        self.assertEqual(
            json.loads(format_event_json(occ_enum)),
            {
                "timestamp_ns": 1234,
                "endpoint": "digital1.change",
                "value": "active",
                "value_type": "digital_state",
            },
        )

        occ_none = Event(
            timestamp_ns=1235,
            endpoint=daq.digital(2).risingedge,
        )
        self.assertEqual(
            format_event_cli(occ_none), "[1235] digital2.risingedge"
        )
        self.assertEqual(
            json.loads(format_event_json(occ_none)),
            {
                "timestamp_ns": 1235,
                "endpoint": "digital2.risingedge",
                "value": None,
                "value_type": None,
            },
        )

        occ_char = Event(
            timestamp_ns=1236,
            endpoint=daq.uart(0).rx,
            value="a",
            value_type=ValueType.CHAR,
        )
        self.assertEqual(
            format_event_cli(occ_char), "[1236] uart0.rx: 0x61 'a'"
        )
        self.assertEqual(
            json.loads(format_event_json(occ_char)),
            {
                "timestamp_ns": 1236,
                "endpoint": "uart0.rx",
                "value": 97,
                "value_type": "char",
            },
        )

        occ_nonprintable = Event(
            timestamp_ns=1237,
            endpoint=daq.uart(0).rx,
            value="\n",
            value_type=ValueType.CHAR,
        )
        self.assertEqual(
            format_event_cli(occ_nonprintable),
            "[1237] uart0.rx: 0x0a",
        )
        self.assertEqual(
            json.loads(format_event_json(occ_nonprintable)),
            {
                "timestamp_ns": 1237,
                "endpoint": "uart0.rx",
                "value": 10,
                "value_type": "char",
            },
        )

        occ_int = Event(
            timestamp_ns=1237,
            endpoint=daq.uart(0).error,
            value=42,
            value_type=ValueType.INT,
        )
        self.assertEqual(format_event_cli(occ_int), "[1237] uart0.error: 42")
        self.assertEqual(
            json.loads(format_event_json(occ_int)),
            {
                "timestamp_ns": 1237,
                "endpoint": "uart0.error",
                "value": 42,
                "value_type": "int",
            },
        )


class DaqCliFlagsAndPluginsTest(unittest.TestCase):
    """Tests top-level flags, YAML configuration, and plugin registration."""

    def setUp(self) -> None:
        CustomPluginBackend.last_instance = None
        self._tempdir = self.enterContext(tempfile.TemporaryDirectory())
        tmp = Path(self._tempdir)
        self.proj_dir = tmp / "project"
        self.home_dir = tmp / "home"
        self.proj_dir.mkdir()
        self.home_dir.mkdir()
        (self.proj_dir / ".pw_daq.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {},
                    "devices": {},
                }
            ),
            encoding="utf-8",
        )
        self.enterContext(
            mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(self.proj_dir),
                    "HOME": str(self.home_dir),
                    "USERPROFILE": str(self.home_dir),
                },
                clear=True,
            )
        )

    def test_mock_backend_registration_and_missing_device_error(self) -> None:
        registry = BackendRegistry()
        self.assertIsInstance(registry, plugins.Registry)
        self.assertIn("mock", list(registry.backend_names()))
        self.assertIsInstance(registry.create_backend("mock", {}), MockBackend)

        empty_registry = BackendRegistry(register_builtins=False)
        empty_registry.register("mock", MockBackend)
        self.assertIn("mock", list(empty_registry.backend_names()))

        rc, _, err = _run_cli_captured(["reset"])
        self.assertEqual(rc, 1)
        self.assertIn("No DAQ device selected", err)

    def test_device_flag_short_and_long(self) -> None:
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "devices": {
                        "mock_dev": {
                            "backend": "mock",
                            "dev": {"type": "mock"},
                            "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, _, _ = _run_cli_captured(["-d", "mock_dev", "reset"])
        self.assertEqual(rc, 0)

        rc, _, _ = _run_cli_captured(["--device=mock_dev", "reset"])
        self.assertEqual(rc, 0)

        # Global flags are also accepted after the subcommand.
        rc, _, _ = _run_cli_captured(["reset", "-d", "mock_dev", "-v"])
        self.assertEqual(rc, 0)

    def test_default_device_option_and_flag_selection(self) -> None:
        test_backends: dict[str, BackendTarget] = {
            "custom_opt": CustomPluginBackend,
            "dev_req": DevRequiredOnOpenBackend,
        }
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "my_dev"},
                    "devices": {
                        "my_dev": {
                            "backend": "custom_opt",
                            "dev": {"type": "mock"},
                            "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                            "device": "dev_from_config",
                        },
                        "other_dev": {
                            "backend": "custom_opt",
                            "dev": {"type": "mock"},
                            "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                            "device": "other_from_config",
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, _, _ = _run_cli_captured(["reset"], extra_backends=test_backends)
        self.assertEqual(rc, 0)
        self.assertIsNotNone(CustomPluginBackend.last_instance)
        assert CustomPluginBackend.last_instance is not None
        self.assertEqual(
            CustomPluginBackend.last_instance.device, "dev_from_config"
        )
        self.assertEqual(
            CustomPluginBackend.last_instance.recorded_calls,
            [("reset", None)],
        )

        # -d / --device flag overrides options.default_device.
        CustomPluginBackend.last_instance = None
        rc, _, _ = _run_cli_captured(
            ["-d", "other_dev", "reset"], extra_backends=test_backends
        )
        self.assertEqual(rc, 0)
        self.assertIsNotNone(CustomPluginBackend.last_instance)
        assert CustomPluginBackend.last_instance is not None
        self.assertEqual(
            CustomPluginBackend.last_instance.device, "other_from_config"
        )

        # Unknown device name fails with error.
        rc, _, err = _run_cli_captured(
            ["-d", "missing_dev", "reset"], extra_backends=test_backends
        )
        self.assertEqual(rc, 1)
        self.assertIn("Missing device configuration", err)

        # Device without required 'backend' or 'capabilities' fails schema
        # validation.
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "devices": {
                        "no_backend_dev": {
                            "dev": {"type": "mock"},
                            "capabilities": {},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, _, err = _run_cli_captured(
            ["-d", "no_backend_dev", "reset"], extra_backends=test_backends
        )
        self.assertEqual(rc, 1)
        self.assertIn("Invalid pw_daq configuration", err)

        # Device without 'dev' delegates connection requirements to the
        # backend: 'config' and 'capabilities' shortcut before open(), while
        # commands that open a connection invoke backend.open().
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "devices": {
                        "offline_dev": {
                            "backend": "dev_req",
                            "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                        },
                        "mock_no_dev": {
                            "backend": "mock",
                            "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, out, _ = _run_cli_captured(["config"], extra_backends=test_backends)
        self.assertEqual(rc, 0)
        self.assertIn("offline_dev", out)

        rc, out, _ = _run_cli_captured(
            ["-d", "offline_dev", "capabilities"],
            extra_backends=test_backends,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out), _MINIMAL_MOCK_CAPABILITIES)

        # MockBackend does not require 'dev', so commands succeed without it.
        rc, _, _ = _run_cli_captured(
            ["-d", "mock_no_dev", "reset"], extra_backends=test_backends
        )
        self.assertEqual(rc, 0)

        # Backends that require 'dev' raise DaqError on open().
        for comm_cmd in (
            ["-d", "offline_dev", "set", "digital0.pull", "up"],
            ["-d", "offline_dev", "listen", "digital0.change"],
            ["-d", "offline_dev", "info"],
        ):
            with self.subTest(comm_cmd=comm_cmd):
                rc, _, err = _run_cli_captured(
                    comm_cmd, extra_backends=test_backends
                )
                self.assertEqual(rc, 1)
                self.assertIn("Missing 'dev' connection configuration", err)

    def test_register_backend_via_extra_backends_and_entry_points(self) -> None:
        config_data = {
            "config_title": "pw_daq",
            "devices": {
                "acme_dev": {
                    "backend": "acme_custom",
                    "dev": {"type": "mock"},
                    "capabilities": _MINIMAL_MOCK_CAPABILITIES,
                }
            },
        }
        cfg_path = Path(self._tempdir) / "acme.yaml"
        cfg_path.write_text(yaml.safe_dump(config_data), encoding="utf-8")

        rc, out, _ = _run_cli_captured(
            [
                "--config-file",
                str(cfg_path),
                "-d",
                "acme_dev",
                "do",
                "digital0.write",
                "high",
            ],
            extra_backends={"acme_custom": PreconfiguredDigitalOutBackend},
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertIsNotNone(CustomPluginBackend.last_instance)
        assert CustomPluginBackend.last_instance is not None
        self.assertEqual(
            CustomPluginBackend.last_instance.recorded_calls,
            [
                (
                    "do",
                    (
                        ActionRef.from_str("digital0.write"),
                        DigitalState.ACTIVE,
                    ),
                ),
            ],
        )

        # Passing both registry and extra_backends fails with DaqError.
        custom_reg = BackendRegistry(
            register_builtins=False,
            discover_entry_points=False,
            extra_backends={"acme_custom": PreconfiguredDigitalOutBackend},
        )
        rc, _, err = _run_cli_captured(
            ["-c", str(cfg_path), "-d", "acme_dev", "reset"],
            registry=custom_reg,
            extra_backends={"acme_custom": PreconfiguredDigitalOutBackend},
        )
        self.assertEqual(rc, 1)
        self.assertIn("Cannot specify both", err)

        # Passing a pre-built registry works directly, including via cli.main().
        CustomPluginBackend.last_instance = None
        rc = cli_mod.main(
            ["-c", str(cfg_path), "-d", "acme_dev", "reset"],
            registry=custom_reg,
        )
        self.assertEqual(rc, 0)
        self.assertIsNotNone(CustomPluginBackend.last_instance)

        # Entry-point backends are discovered and loaded lazily on demand.
        ep_acme = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_acme.name = "acme_custom"
        ep_acme.value = f"{__name__}:PreconfiguredDigitalOutBackend"
        ep_acme.group = ENTRY_POINT_GROUP
        ep_acme.load.return_value = PreconfiguredDigitalOutBackend

        ep_broken = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_broken.name = "broken_ep"
        ep_broken.value = "missing_pkg:MissingBackend"
        ep_broken.group = ENTRY_POINT_GROUP
        ep_broken.load.side_effect = ImportError("No module named missing_pkg")

        CustomPluginBackend.last_instance = None
        with mock.patch(
            "importlib.metadata.entry_points",
            return_value=[ep_acme, ep_broken],
        ):
            rc, _, _ = _run_cli_captured(
                ["-c", str(cfg_path), "-d", "acme_dev", "reset"]
            )
            self.assertEqual(rc, 0)
            ep_acme.load.assert_called_once_with()
            ep_broken.load.assert_not_called()

    def test_invalid_and_missing_config_files(self) -> None:
        # Non-existent --config-file raises DaqError and exits with 1.
        missing_path = Path(self._tempdir) / "missing.yaml"
        rc, _, err = _run_cli_captured(
            ["--config-file", str(missing_path), "reset"]
        )
        self.assertEqual(rc, 1)
        self.assertIn("Configuration file not found", err)

        # Empty, comment-only, and malformed explicit configs (-c and
        # PW_DAQ_CONFIG) fail with exit code 1 even for offline 'config'.
        for case_name, bad_text in (
            ("empty", "   \n"),
            ("comment_only", "# only a comment\n"),
            ("bad_yaml", "{unterminated: [1, 2\n"),
        ):
            bad_path = Path(self._tempdir) / f"{case_name}.yaml"
            bad_path.write_text(bad_text, encoding="utf-8")
            with self.subTest(cli_flag=case_name):
                rc, _, err = _run_cli_captured(["-c", str(bad_path), "config"])
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)
            with (
                self.subTest(env_var=case_name),
                mock.patch.dict(os.environ, {"PW_DAQ_CONFIG": str(bad_path)}),
            ):
                rc, _, err = _run_cli_captured(["config"])
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)

        # Unknown backend or disallowed 'backends' section exits with 1.
        unknown_backend_path = Path(self._tempdir) / "unknown_backend.yaml"
        unknown_backend_path.write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "bad_dev"},
                    "devices": {
                        "bad_dev": {
                            "backend": "unregistered_backend",
                            "dev": {"type": "mock"},
                            "capabilities": {},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, _, err = _run_cli_captured(
            ["-c", str(unknown_backend_path), "reset"]
        )
        self.assertEqual(rc, 1)
        self.assertIn("Unknown DAQ backend 'unregistered_backend'", err)

        disallowed_backends_path = Path(self._tempdir) / "has_backends.yaml"
        disallowed_backends_path.write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "backends": {
                        "evil": {
                            "module": "evil_pkg",
                            "class": "EvilBackend",
                        }
                    },
                    "devices": {},
                }
            ),
            encoding="utf-8",
        )
        rc, _, err = _run_cli_captured(
            ["-c", str(disallowed_backends_path), "config"]
        )
        self.assertEqual(rc, 1)
        self.assertIn("Invalid pw_daq configuration", err)

        # Malformed default config logs a warning without breaking --help.
        (self.home_dir / ".pw_daq.yaml").write_bytes(b"\xff\xfe\x00\x00")
        out = io.StringIO()
        err_stream = io.StringIO()
        prev_stream = pw_cli_log._STDERR_HANDLER.setStream(err_stream)
        try:
            with (
                contextlib.redirect_stdout(out),
                contextlib.redirect_stderr(err_stream),
                self.assertRaises(SystemExit) as cm,
            ):
                run_cli(["--help"])
        finally:
            if prev_stream is not None:
                pw_cli_log._STDERR_HANDLER.setStream(prev_stream)
        self.assertEqual(cm.exception.code, 0)
        self.assertIn("usage: daq-cli", out.getvalue())
        self.assertIn("WRN", err_stream.getvalue())

    def test_config_command_prints_yaml_config(self) -> None:
        # Works even when no device is configured.
        rc, out, _ = _run_cli_captured(["config"])
        self.assertEqual(rc, 0)
        parsed = yaml.safe_load(out)
        self.assertEqual(parsed["config_title"], "pw_daq")
        self.assertEqual(parsed["options"], {})
        self.assertNotIn("backends", parsed)
        self.assertEqual(parsed["devices"], {})

        # Reflects configured devices and options without CLI flag mutation.
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "mock_dev"},
                    "devices": {
                        "mock_dev": {
                            "backend": "mock",
                            "dev": {"type": "mock"},
                            "capabilities": {},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, out, _ = _run_cli_captured(["-v", "config"])
        self.assertEqual(rc, 0)
        parsed = yaml.safe_load(out)
        self.assertEqual(parsed["config_title"], "pw_daq")
        self.assertEqual(parsed["options"], {"default_device": "mock_dev"})
        self.assertIn("mock_dev", parsed["devices"])

    def test_config_file_exclusively_overrides_default_files(self) -> None:
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "proj_dev"},
                    "devices": {
                        "proj_dev": {
                            "backend": "mock",
                            "dev": {"type": "mock"},
                            "capabilities": {},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        cli_cfg = Path(self._tempdir) / "cli_cfg.yaml"
        cli_cfg.write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "cli_dev"},
                    "devices": {
                        "cli_dev": {
                            "backend": "mock",
                            "dev": {"type": "mock"},
                            "capabilities": {},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        rc, out, _ = _run_cli_captured(
            ["--config-file", str(cli_cfg), "config"]
        )
        self.assertEqual(rc, 0)
        parsed = yaml.safe_load(out)
        self.assertEqual(parsed["options"], {"default_device": "cli_dev"})
        self.assertNotIn("proj_dev", parsed["devices"])
        self.assertIn("cli_dev", parsed["devices"])

    def test_layered_config_deep_merge_across_files(self) -> None:
        (self.proj_dir / ".pw_daq.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {},
                    "devices": {
                        "split_dev": {
                            "backend": "custom",
                            "device": "board_default",
                            "capabilities": {
                                "blocks": ["digital0"],
                                "resources": ["gp14"],
                                "mappings": {"p0": "gp14"},
                                "config": {
                                    "digital0": {
                                        "attributes": {"pin": "p0"},
                                    },
                                },
                            },
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        (self.proj_dir / ".pw_daq.user.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "options": {"default_device": "split_dev"},
                    "devices": {
                        "split_dev": {
                            "dev": {
                                "type": "serial",
                                "path": "/dev/ttyUSB0",
                                "baudrate": 115200,
                            },
                            "capabilities": {
                                "blocks": ["digital0", "uart0"],
                                "resources": ["gp14", "gp4", "gp5"],
                                "mappings": {
                                    "p0": "gp14",
                                    "p2": "gp4",
                                    "p3": "gp5",
                                },
                                "config": {
                                    "uart0": {
                                        "attributes": {
                                            "txpin": "p2",
                                            "rxpin": "p3",
                                        },
                                    },
                                },
                            },
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        (self.home_dir / ".pw_daq.yaml").write_text(
            yaml.safe_dump(
                {
                    "config_title": "pw_daq",
                    "devices": {
                        "split_dev": {
                            "device": "user_override_dev",
                            "dev": {"baudrate": 460800},
                        }
                    },
                }
            ),
            encoding="utf-8",
        )

        rc, out, _ = _run_cli_captured(
            ["config"], extra_backends={"custom": CustomPluginBackend}
        )
        self.assertEqual(rc, 0)
        parsed = yaml.safe_load(out)
        self.assertEqual(parsed["options"], {"default_device": "split_dev"})
        self.assertEqual(
            parsed["devices"]["split_dev"],
            {
                "backend": "custom",
                "device": "user_override_dev",
                "dev": {
                    "type": "serial",
                    "path": "/dev/ttyUSB0",
                    "baudrate": 460800,
                },
                "capabilities": {
                    "blocks": ["digital0", "uart0"],
                    "resources": ["gp14", "gp4", "gp5"],
                    "mappings": {
                        "p0": "gp14",
                        "p2": "gp4",
                        "p3": "gp5",
                    },
                    "config": {
                        "digital0": {
                            "attributes": {"pin": "p0"},
                        },
                        "uart0": {
                            "attributes": {
                                "txpin": "p2",
                                "rxpin": "p3",
                            },
                        },
                    },
                },
            },
        )

        rc, _, _ = _run_cli_captured(
            ["reset"], extra_backends={"custom": CustomPluginBackend}
        )
        self.assertEqual(rc, 0)
        self.assertIsNotNone(CustomPluginBackend.last_instance)
        assert CustomPluginBackend.last_instance is not None
        self.assertEqual(
            CustomPluginBackend.last_instance.device, "user_override_dev"
        )
        self.assertEqual(
            CustomPluginBackend.last_instance.recorded_calls,
            [("reset", None)],
        )


class _IsolatedBackendCliTestCase(unittest.TestCase):
    """Base test case isolating os.environ and host config files for run_cli."""

    def setUp(self) -> None:
        self._tempdir = self.enterContext(tempfile.TemporaryDirectory())
        tmp = Path(self._tempdir)
        self.proj_dir = tmp / "project"
        self.home_dir = tmp / "home"
        self.proj_dir.mkdir()
        self.home_dir.mkdir()
        self.enterContext(
            mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(self.proj_dir),
                    "HOME": str(self.home_dir),
                    "USERPROFILE": str(self.home_dir),
                },
                clear=True,
            )
        )
        self.backend = MockBackend()


class DaqCliBlockCommandsTest(_IsolatedBackendCliTestCase):
    """Tests for block commands: create, list, destroy, set, and get."""

    def test_environment_isolates_emoji_and_daq_config_vars(self) -> None:
        self.assertNotIn("PW_EMOJI", os.environ)
        self.assertNotIn("PW_DAQ_CONFIG", os.environ)
        rc, _, err = _run_cli_captured(
            ["destroy", "digital99"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("ERR", err)

    def test_create_digital_out_and_defaults(self) -> None:
        rc, _, _ = _run_cli_captured(
            [
                "create",
                "digital0",
                "--direction=out",
                "--initial=high",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.DIGITAL, 0),
                    {
                        "direction": DigitalDirection.OUT,
                        "initial": DigitalState.ACTIVE,
                    },
                ),
            ),
        )
        self.assertEqual(
            self.backend.blocks[BlockRef(BlockType.DIGITAL, 0)],
            {
                "pin": "p0",
                "direction": DigitalDirection.OUT,
                "initial": DigitalState.ACTIVE,
            },
        )

        # Default initial is inactive when omitted in out mode.
        rc, _, _ = _run_cli_captured(
            ["create", "digital2", "--direction=out"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.DIGITAL, 2),
                    {
                        "direction": DigitalDirection.OUT,
                    },
                ),
            ),
        )
        self.assertEqual(
            self.backend.blocks[BlockRef(BlockType.DIGITAL, 2)],
            {
                "pin": "p2",
                "direction": DigitalDirection.OUT,
                "initial": DigitalState.INACTIVE,
            },
        )

    def test_create_digital_in_and_opendrain(self) -> None:
        rc, _, _ = _run_cli_captured(
            [
                "create",
                "digital1",
                "--direction=in",
                "--pull=down",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.DIGITAL, 1),
                    {
                        "direction": DigitalDirection.IN,
                        "pull": DigitalPull.DOWN,
                    },
                ),
            ),
        )
        self.assertEqual(
            self.backend.blocks[BlockRef(BlockType.DIGITAL, 1)],
            {
                "pin": "p1",
                "direction": DigitalDirection.IN,
                "pull": DigitalPull.DOWN,
            },
        )

        rc, _, _ = _run_cli_captured(
            [
                "create",
                "digital3",
                "--direction=opendrain",
                "--initial=active",
                "--pull=up",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.DIGITAL, 3),
                    {
                        "direction": DigitalDirection.OPENDRAIN,
                        "initial": DigitalState.ACTIVE,
                        "pull": DigitalPull.UP,
                    },
                ),
            ),
        )

        # Default initial=inactive and pull=none in opendrain mode.
        rc, _, _ = _run_cli_captured(
            ["create", "digital4", "--direction=opendrain"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.DIGITAL, 4),
                    {
                        "direction": DigitalDirection.OPENDRAIN,
                    },
                ),
            ),
        )
        self.assertEqual(
            self.backend.blocks[BlockRef(BlockType.DIGITAL, 4)],
            {
                "pin": "p4",
                "direction": DigitalDirection.OPENDRAIN,
                "initial": DigitalState.INACTIVE,
                "pull": DigitalPull.NONE,
            },
        )

    def test_create_digital_invalid_combinations(self) -> None:
        invalid_cases = [
            # Missing required --direction flag.
            ["create", "digital0"],
            ["create", "digital0", "--initial=low"],
            # --initial provided in 'in' mode.
            ["create", "digital0", "--direction=in", "--initial=high"],
            ["create", "digital0", "--direction=in", "--initial=low"],
            # --pull provided in 'out' mode.
            ["create", "digital0", "--direction=out", "--pull=up"],
            # Removed/unknown flags or positional arguments.
            ["create", "digital0", "out", "low"],
            ["create", "digital0", "--direction=out", "--slew=fast"],
            ["create", "digital0", "--direction=out", "--strength=high"],
            # Invalid direction or initial values.
            ["create", "digital0", "--direction=inout"],
            ["create", "digital0", "--direction=out", "--initial=floating"],
        ]
        for args in invalid_cases:
            with self.subTest(args=args):
                rc, _, err = _run_cli_captured([*args], backend=self.backend)
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)

    def test_create_uart_valid_and_invalid(self) -> None:
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
        rc, _, _ = _run_cli_captured(
            [
                "create",
                "uart1",
                "--baudrate=115200",
                "--loopback",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.UART, 1),
                    {
                        "baudrate": 115200,
                        "loopback": True,
                    },
                ),
            ),
        )
        self.assertEqual(
            self.backend.blocks[BlockRef(BlockType.UART, 1)],
            {
                "txpin": "p2",
                "rxpin": "p3",
                "baudrate": 115200,
                "databits": 8,
                "parity": UartParity.NONE,
                "stopbits": 1,
                "loopback": True,
            },
        )

        rc, _, _ = _run_cli_captured(
            [
                "create",
                "uart0",
                "--baudrate=9600",
                "--databits=7",
                "--parity=even",
                "--stopbits=2",
                "--no-loopback",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "create",
                (
                    BlockRef(BlockType.UART, 0),
                    {
                        "baudrate": 9600,
                        "databits": 7,
                        "parity": UartParity.EVEN,
                        "stopbits": 2,
                        "loopback": False,
                    },
                ),
            ),
        )

        for bad_uart in (
            ["create", "uart0"],
            ["create", "uart0", "--baudrate=0"],
            ["create", "uart0", "--baudrate=-115200"],
            ["create", "uart0", "--baudrate=not_a_number"],
            ["create", "uart0", "--baudrate=115200", "--databits=6"],
            ["create", "uart0", "--baudrate=115200", "--parity=mark"],
            ["create", "uart0", "--baudrate=115200", "--stopbits=3"],
            ["create", "uart0", "115200"],
        ):
            with self.subTest(bad_uart=bad_uart):
                rc, _, err = _run_cli_captured(
                    [*bad_uart], backend=self.backend
                )
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)

    def test_create_parser_and_backend_capability_validation(self) -> None:
        custom_caps = {
            "blocks": ["digital0", "digital1"],
            "resources": ["dio0", "dio1"],
            "mappings": {
                "p0": "dio0",
                "p1": "dio1",
            },
            "config": {
                "digital0": {
                    "attributes": {
                        "pin": "p0",
                        "direction": ["out"],
                    },
                },
                "digital1": {
                    "attributes": {
                        "pin": "p1",
                        "direction": ["in"],
                    },
                },
            },
        }
        custom_backend = MockBackend()
        custom_backend.set_capabilities(custom_caps)

        # digital0 supports --direction=out and --initial, and backend
        # rejects BED-disallowed direction='in' or unconfigured block instances.
        rc, _, _ = _run_cli_captured(
            [
                "create",
                "digital0",
                "--direction=out",
                "--initial=high",
            ],
            backend=custom_backend,
        )
        self.assertEqual(rc, 0)

        rc, _, err = _run_cli_captured(
            [
                "create",
                "digital0",
                "--direction=in",
                "--pull=up",
            ],
            backend=custom_backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("Unsupported value", err)

        rc, _, _ = _run_cli_captured(
            ["create", "digital1", "--direction=in", "--pull=up"],
            backend=custom_backend,
        )
        self.assertEqual(rc, 0)

        # Block instance not listed in capabilities["blocks"] is rejected.
        rc, _, err = _run_cli_captured(
            ["create", "digital2", "--direction=in"],
            backend=custom_backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("Unsupported block 'digital2'", err)

    def test_create_help_descriptions_without_backend_connection(self) -> None:
        d0_help = cli_mod._build_block_create_parser(
            DigitalBlock, "digital0"
        ).format_help()
        self.assertIn("The direction of the digital block. (required)", d0_help)
        self.assertIn("(default: inactive)", d0_help)
        self.assertIn("(default: none)", d0_help)

        u0_help = cli_mod._build_block_create_parser(
            UartBlock, "uart0"
        ).format_help()
        self.assertIn("The baud rate to configure. (required)", u0_help)
        self.assertIn("(default: 8)", u0_help)
        self.assertIn("(default: false)", u0_help)

        # `create --help` and `create digital0 --help` succeed without
        # querying backend capabilities or requiring a connected device.
        class FailingCapsBackend(MockBackend):
            def capabilities(self) -> dict[str, Any]:
                raise DaqError("Device not configured")

        for help_args, expected_text in (
            (["create", "--help"], "<block>"),
            (["create", "digital0", "--help"], "--direction"),
        ):
            out = io.StringIO()
            err = io.StringIO()
            prev_stream = pw_cli_log._STDERR_HANDLER.setStream(err)
            try:
                with (
                    contextlib.redirect_stdout(out),
                    contextlib.redirect_stderr(err),
                    self.assertRaises(SystemExit) as cm,
                ):
                    run_cli(help_args, backend=FailingCapsBackend())
            finally:
                if prev_stream is not None:
                    pw_cli_log._STDERR_HANDLER.setStream(prev_stream)

            self.assertEqual(cm.exception.code, 0)
            self.assertIn(expected_text, out.getvalue())

    def test_create_conflict(self) -> None:
        uart0 = BlockRef.from_str("uart0")
        digital1 = BlockRef.from_str("digital1")
        self.backend.add_conflict(uart0, digital1)

        rc, _, _ = _run_cli_captured(
            ["create", "uart0", "--baudrate=115200"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertIn(uart0, self.backend.blocks)

        # Creating conflicting digital1 fails.
        rc, _, err = _run_cli_captured(
            ["create", "digital1", "--direction=in"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("conflicts with existing block(s)", err)

        # Destroying uart0 allows digital1 to be created.
        rc, _, _ = _run_cli_captured(
            ["destroy", "uart0"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        rc, _, _ = _run_cli_captured(
            ["create", "digital1", "--direction=in"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertNotIn(uart0, self.backend.blocks)
        self.assertIn(digital1, self.backend.blocks)

    def test_list_and_destroy(self) -> None:
        rc, out, _ = _run_cli_captured(["list"], backend=self.backend)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")

        _run_cli_captured(
            ["create", "digital1", "--direction=in"],
            backend=self.backend,
        )
        _run_cli_captured(
            ["create", "uart0", "--baudrate=115200"],
            backend=self.backend,
        )

        rc, out, _ = _run_cli_captured(["list"], backend=self.backend)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "digital1\nuart0\n")

        rc, _, _ = _run_cli_captured(
            ["destroy", "digital1"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

        rc, out, _ = _run_cli_captured(["list"], backend=self.backend)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "uart0\n")

        # Invalid block name on destroy fails.
        rc, _, err = _run_cli_captured(
            ["destroy", "invalid_block"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("ERR", err)

    def test_set_and_get_attributes(self) -> None:
        _run_cli_captured(
            [
                "create",
                "digital1",
                "--direction=in",
                "--pull=down",
            ],
            backend=self.backend,
        )
        _run_cli_captured(
            [
                "create",
                "uart0",
                "--baudrate=115200",
                "--loopback",
            ],
            backend=self.backend,
        )

        # Get const, init, and mutable attributes.
        rc, out, _ = _run_cli_captured(
            ["get", "digital1.pin"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "p1\n")

        rc, out, _ = _run_cli_captured(
            ["get", "digital1.direction"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "in\n")

        rc, out, _ = _run_cli_captured(
            ["get", "digital1.pull"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "down\n")

        rc, out, _ = _run_cli_captured(
            ["get", "uart0.baudrate"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "115200\n")

        rc, out, _ = _run_cli_captured(
            ["get", "uart0.loopback"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "true\n")

        # Set runtime-mutable attribute (digital1.pull).
        rc, _, _ = _run_cli_captured(
            ["set", "digital1.pull", "up"], backend=self.backend
        )
        self.assertEqual(rc, 0)

        rc, out, _ = _run_cli_captured(
            ["get", "digital1.pull"], backend=self.backend
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "up\n")

        # Setting const or init attributes fails.
        for bad_set in (
            ["set", "digital1.pin", "p5"],
            ["set", "digital1.direction", "out"],
            ["set", "uart0.txpin", "p10"],
            ["set", "uart0.baudrate", "9600"],
            ["set", "uart0.loopback", "false"],
            ["set", "digital1.pull", "invalid_pull"],
            ["set", "digital9.pull", "up"],
        ):
            with self.subTest(bad_set=bad_set):
                rc, _, err = _run_cli_captured([*bad_set], backend=self.backend)
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)

        # Passing const or unknown attributes directly to Block.create fails.
        daq = Daq(self.backend)
        with self.assertRaises(DaqError):
            daq.digital(
                0,
                direction=DigitalDirection.IN,
            ).create(pin="p14")
        with self.assertRaises(DaqError):
            daq.digital(
                0,
                direction=DigitalDirection.IN,
            ).create(unknown_attr=123)
        with self.assertRaises(DaqError):
            daq.digital(0).create()
        with self.assertRaises(DaqError):
            daq.digital(0, direction=None).create()


class DaqCliOnAndDoTest(_IsolatedBackendCliTestCase):
    """Tests for 'on' and 'do' commands."""

    def setUp(self) -> None:
        super().setUp()
        self.backend.set_capabilities(
            {
                "blocks": [
                    "digital0",
                    "digital1",
                    "digital2",
                    "uart0",
                    "uart1",
                ],
                "resources": [f"dio{i}" for i in range(7)],
                "mappings": {f"p{i}": f"dio{i}" for i in range(7)},
                "config": {
                    "digital0": {"attributes": {"pin": "p0"}},
                    "digital1": {"attributes": {"pin": "p1"}},
                    "digital2": {"attributes": {"pin": "p2"}},
                    "uart0": {"attributes": {"txpin": "p3", "rxpin": "p4"}},
                    "uart1": {"attributes": {"txpin": "p5", "rxpin": "p6"}},
                },
            }
        )
        # Set up digital0, digital1, uart0, and uart1 as existing blocks while
        # leaving digital2 uncreated to test error handling on uncreated blocks.
        self.backend.setup_block(
            "digital0", {"direction": DigitalDirection.OUT}
        )
        self.backend.setup_block("digital1", {"direction": DigitalDirection.IN})
        self.backend.setup_block("uart0", {"baudrate": 115200})
        self.backend.setup_block("uart1", {"baudrate": 115200})

    def test_on_matching_types_without_action_args(self) -> None:
        rc, _, _ = _run_cli_captured(
            ["on", "digital1.change", "do", "digital0.write"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "on",
                (
                    SignalRef.from_str("digital1.change"),
                    ActionRef.from_str("digital0.write"),
                    None,
                ),
            ),
        )

        rc, _, _ = _run_cli_captured(
            ["on", "uart0.rx", "do", "uart1.tx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

    def test_on_valueless_action_ignores_signal_value(self) -> None:
        # Valueless signal -> valueless action.
        rc, _, _ = _run_cli_captured(
            ["on", "digital1.risingedge", "do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

        # Valued signal -> valueless action (allowed, signal value is ignored).
        rc, _, _ = _run_cli_captured(
            ["on", "digital1.change", "do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

        rc, _, _ = _run_cli_captured(
            ["on", "uart0.rx", "do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

    def test_on_with_explicit_action_arg(self) -> None:
        rc, _, _ = _run_cli_captured(
            [
                "on",
                "digital1.risingedge",
                "do",
                "digital0.write",
                "high",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "on",
                (
                    SignalRef.from_str("digital1.risingedge"),
                    ActionRef.from_str("digital0.write"),
                    DigitalState.ACTIVE,
                ),
            ),
        )

        rc, _, _ = _run_cli_captured(
            ["on", "digital1.fallingedge", "do", "uart1.tx", "a"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "on",
                (
                    SignalRef.from_str("digital1.fallingedge"),
                    ActionRef.from_str("uart1.tx"),
                    "a",
                ),
            ),
        )

        # Hex character input is supported for CHAR actions.
        rc, _, _ = _run_cli_captured(
            [
                "on",
                "digital1.fallingedge",
                "do",
                "uart1.tx",
                "0x0a",
            ],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "on",
                (
                    SignalRef.from_str("digital1.fallingedge"),
                    ActionRef.from_str("uart1.tx"),
                    "\n",
                ),
            ),
        )

    def test_on_invalid_cases(self) -> None:
        invalid_cases = [
            # Missing 'do' keyword.
            ["on", "digital1.change", "not_do", "digital0.write"],
            # Triggering on an Action instead of a Signal.
            ["on", "digital1.write", "do", "digital0.write", "high"],
            ["on", "digital1.read", "do", "digital0.write", "high"],
            ["on", "uart0.tx", "do", "digital0.read"],
            # Invoking an Signal instead of an Action.
            ["on", "digital1.change", "do", "digital0.change"],
            ["on", "uart0.rx", "do", "uart0.sent"],
            # Type mismatch when action_arg is omitted.
            ["on", "uart0.rx", "do", "digital0.write"],
            ["on", "digital1.change", "do", "uart0.tx"],
            ["on", "digital1.risingedge", "do", "digital0.write"],
            # Passing argument to valueless action.
            ["on", "digital1.risingedge", "do", "digital0.read", "high"],
            # Invalid digital state value.
            ["on", "digital1.risingedge", "do", "digital0.write", "floating"],
            # Multi-character string for char action.
            ["on", "digital1.risingedge", "do", "uart0.tx", "ab"],
            # Referencing an uncreated block.
            ["on", "digital2.change", "do", "digital0.write"],
            ["on", "digital1.change", "do", "digital2.write"],
        ]
        for args in invalid_cases:
            with self.subTest(args=args):
                rc, _, err = _run_cli_captured([*args], backend=self.backend)
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)

    def test_do_valid_and_invalid(self) -> None:
        rc, out, _ = _run_cli_captured(
            ["do", "digital0.write", "high"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            (
                "do",
                (
                    ActionRef.from_str("digital0.write"),
                    DigitalState.ACTIVE,
                ),
            ),
        )

        rc, out, _ = _run_cli_captured(
            ["do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "active\n")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("do", (ActionRef.from_str("digital0.read"), None)),
        )

        # 'active'/'inactive' and CLI truthy/falsy aliases ('high'/'low',
        # 'true'/'false', '1'/'0', 'on'/'off') convert to ACTIVE/INACTIVE.
        rc, out, _ = _run_cli_captured(
            ["do", "digital0.write", "inactive"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")

        rc, out, _ = _run_cli_captured(
            ["do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "inactive\n")

        for alias, expected in (
            ("low", DigitalState.INACTIVE),
            ("true", DigitalState.ACTIVE),
            ("false", DigitalState.INACTIVE),
            ("1", DigitalState.ACTIVE),
            ("0", DigitalState.INACTIVE),
            ("on", DigitalState.ACTIVE),
            ("off", DigitalState.INACTIVE),
        ):
            rc, out, _ = _run_cli_captured(
                ["do", "digital0.write", alias],
                backend=self.backend,
            )
            self.assertEqual(rc, 0)
            self.assertEqual(
                self.backend.recorded_calls[-1],
                ("do", (ActionRef.from_str("digital0.write"), expected)),
            )

        _run_cli_captured(
            ["do", "digital0.write", "inactive"],
            backend=self.backend,
        )

        rc, out, _ = _run_cli_captured(
            ["do", "digital0.toggle"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("do", (ActionRef.from_str("digital0.toggle"), None)),
        )

        rc, out, _ = _run_cli_captured(
            ["do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "active\n")

        rc, out, _ = _run_cli_captured(
            ["do", "uart1.tx", "a"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("do", (ActionRef.from_str("uart1.tx"), "a")),
        )

        # Hex character input is supported for CHAR actions.
        rc, out, _ = _run_cli_captured(
            ["do", "uart1.tx", "0x0a"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("do", (ActionRef.from_str("uart1.tx"), "\n")),
        )

        # Action reporting failure sets exit code to 1 and prints nothing.
        self.backend.set_action_result(
            "uart1.tx",
            ActionResult(success=False),
        )
        rc, out, _ = _run_cli_captured(
            ["do", "uart1.tx", "a"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")

        self.backend.set_action_result(
            "digital0.read",
            ActionResult(
                success=False,
                value=DigitalState.ACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            ),
        )
        rc, out, _ = _run_cli_captured(
            ["do", "digital0.read"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.backend.set_action_result("uart1.tx", None)
        self.backend.set_action_result("digital0.read", None)

        for bad_do in (
            # Signals are not valid for 'do'.
            ["do", "digital0.change"],
            ["do", "digital0.risingedge"],
            ["do", "uart1.rx"],
            # Invoking 'do' on an uncreated block fails.
            ["do", "digital2.write", "high"],
            ["do", "digital2.read"],
            # Missing required argument.
            ["do", "digital0.write"],
            ["do", "uart1.tx"],
            # Invalid digital state value.
            ["do", "digital0.write", "floating"],
            ["do", "digital0.write", "2"],
            # Extra argument for valueless action.
            ["do", "digital0.read", "high"],
            # Multi-char or out-of-range hex argument for char action.
            ["do", "uart1.tx", "hello"],
            ["do", "uart1.tx", "0x100"],
            ["do", "uart1.tx", "0xzz"],
            ["do", "uart1.tx", "0x"],
            # Extra positional argument.
            ["do", "uart1.tx", "a", "b"],
        ):
            with self.subTest(bad_do=bad_do):
                rc, _, err = _run_cli_captured([*bad_do], backend=self.backend)
                self.assertEqual(rc, 1)
                self.assertIn("ERR", err)


class DaqCliListenLogAndDeviceCommandsTest(_IsolatedBackendCliTestCase):
    """Tests for listen, dump, clear, info, capabilities, and reset."""

    def setUp(self) -> None:
        super().setUp()
        self.backend = MockBackend(log_capacity=512)

    def test_listen_with_count_and_keyboard_interrupt(self) -> None:
        self.backend.setup_block("uart1", {"baudrate": 115200})
        uart1_rx = SignalRef.from_str("uart1.rx")
        for i, ch in enumerate(("a", "b", "c"), start=1):
            self.backend.queue_event(
                EventRef(
                    timestamp_ns=i * 100,
                    endpoint=uart1_rx,
                    value=ch,
                    value_type=ValueType.CHAR,
                )
            )

        rc, out, _ = _run_cli_captured(
            ["listen", "-n", "2", "-t", "1.5", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            out,
            "[100] uart1.rx: 0x61 'a'\n[200] uart1.rx: 0x62 'b'\n",
        )
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (uart1_rx, 2, 1.5)),
        )

        # --json outputs NDJSON with integer char values.
        rc, out, _ = _run_cli_captured(
            ["listen", "--json", "-n", "1", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            json.loads(out),
            {
                "timestamp_ns": 300,
                "endpoint": "uart1.rx",
                "value": 99,
                "value_type": "char",
            },
        )

        # Simulate ^C (KeyboardInterrupt) during streaming listen.
        def _interrupting_iter(
            signal_ref: SignalRef,
        ) -> Iterator[EventRef[str]]:
            yield EventRef(
                timestamp_ns=500,
                endpoint=signal_ref,
                value="z",
                value_type=ValueType.CHAR,
            )
            raise KeyboardInterrupt

        def _interrupting_listen(
            signal_ref: SignalRef,
            count: int | None = None,
            timeout: float | None = None,
        ) -> EventStream[EventRef[str]]:
            del count, timeout
            return EventStream(_interrupting_iter(signal_ref))

        with mock.patch.object(
            self.backend, "listen", side_effect=_interrupting_listen
        ):
            rc, out, _ = _run_cli_captured(
                ["listen", "uart1.rx"],
                backend=self.backend,
            )
            self.assertEqual(rc, 0)
            self.assertEqual(out, "[500] uart1.rx: 0x7a 'z'\n")

        # Invalid -n or -t value or listening to an Action fails.
        rc, _, err = _run_cli_captured(
            ["listen", "-n", "0", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("positive integer", err)

        rc, _, err = _run_cli_captured(
            ["listen", "-t", "0", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("positive number", err)

        rc, _, err = _run_cli_captured(
            ["listen", "--timeout=-2.0", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("positive number", err)

        rc, _, err = _run_cli_captured(
            ["listen", "uart1.tx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("not a Signal", err)

        # Listening on an uncreated block fails.
        rc, _, err = _run_cli_captured(
            ["listen", "-n", "1", "uart0.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("Block 'uart0' does not exist", err)

    def test_wait_command(self) -> None:
        self.backend.setup_block("uart1", {"baudrate": 115200})
        uart1_rx = SignalRef.from_str("uart1.rx")
        self.backend.queue_event(
            EventRef(
                timestamp_ns=123,
                endpoint=uart1_rx,
                value="w",
                value_type=ValueType.CHAR,
            )
        )
        self.backend.queue_event(
            EventRef(
                timestamp_ns=456,
                endpoint=uart1_rx,
                value="x",
                value_type=ValueType.CHAR,
            )
        )

        rc, out, _ = _run_cli_captured(
            ["wait", "-t", "2.0", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, "[123] uart1.rx: 0x77 'w'\n")
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (uart1_rx, 1, 2.0)),
        )

        # Second wait receives the next event without -t (and with --json).
        rc, out, _ = _run_cli_captured(
            ["wait", "--json", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            json.loads(out),
            {
                "timestamp_ns": 456,
                "endpoint": "uart1.rx",
                "value": 120,
                "value_type": "char",
            },
        )
        self.assertEqual(
            self.backend.recorded_calls[-1],
            ("listen", (uart1_rx, 1, None)),
        )

        # When no event arrives before the stream ends, wait exits with 1.
        rc, out, err = _run_cli_captured(
            ["wait", "-t", "0.5", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("Timed out waiting", err)

        # wait does not accept -n / --number.
        rc, _, err = _run_cli_captured(
            ["wait", "-n", "1", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("unrecognized arguments", err)

        # Invalid timeout or waiting on an Action fails.
        rc, _, err = _run_cli_captured(
            ["wait", "-t", "0", "uart1.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("positive number", err)

        rc, _, err = _run_cli_captured(
            ["wait", "uart1.tx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("not a Signal", err)

        # Waiting on an uncreated block fails.
        rc, _, err = _run_cli_captured(
            ["wait", "-t", "0.5", "uart0.rx"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("Block 'uart0' does not exist", err)

    def test_dump_clear_and_info(self) -> None:
        self.backend.queue_event(
            EventRef(
                timestamp_ns=1234,
                endpoint=SignalRef.from_str("uart0.error"),
                value=123,
                value_type=ValueType.INT,
            )
        )
        self.backend.queue_event(
            EventRef(
                timestamp_ns=1235,
                endpoint=SignalRef.from_str("digital2.risingedge"),
            )
        )
        self.backend.queue_event(
            EventRef(
                timestamp_ns=1236,
                endpoint=SignalRef.from_str("digital1.change"),
                value=DigitalState.INACTIVE,
                value_type=ValueType.DIGITAL_STATE,
            )
        )

        rc, out, _ = _run_cli_captured(
            ["info"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        info_before = json.loads(out)
        self.assertEqual(info_before["used"], 3)
        self.assertEqual(info_before["capacity"], 512)

        expected_ndjson = (
            '{"timestamp_ns": 1234, "endpoint": "uart0.error", '
            '"value": 123, "value_type": "int"}\n'
            '{"timestamp_ns": 1235, "endpoint": "digital2.risingedge", '
            '"value": null, "value_type": null}\n'
            '{"timestamp_ns": 1236, "endpoint": "digital1.change", '
            '"value": "inactive", "value_type": "digital_state"}\n'
        )

        rc, out, _ = _run_cli_captured(
            ["dump"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, expected_ndjson)

        # dump does not accept a file or '-' positional argument.
        for extra_arg in ("-", "events.ndjson"):
            with self.subTest(extra_arg=extra_arg):
                rc, out, err = _run_cli_captured(
                    ["dump", extra_arg],
                    backend=self.backend,
                )
                self.assertEqual(rc, 1)
                self.assertEqual(out, "")
                self.assertIn("unrecognized arguments", err)

        rc, _, _ = _run_cli_captured(
            ["clear"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)

        rc, out, _ = _run_cli_captured(
            ["info"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        info_after = json.loads(out)
        self.assertEqual(info_after["used"], 0)
        self.assertEqual(info_after["capacity"], 512)

    def test_info_capabilities_and_reset(self) -> None:
        scenario_caps = {
            "blocks": ["digital0", "uart0"],
            "resources": ["dio0", "dio1", "dio2"],
            "mappings": {
                "p0": "dio0",
                "p1": "dio1",
                "p2": "dio2",
            },
            "config": {
                "digital0": {"attributes": {"pin": "p0"}},
                "uart0": {
                    "attributes": {
                        "txpin": "p1",
                        "rxpin": "p2",
                        "baudrate": [9600, 115200],
                    }
                },
            },
        }
        self.backend.set_device_config(
            {
                "device": "sim42",
                "capabilities": scenario_caps,
            }
        )
        rc, out, _ = _run_cli_captured(
            ["info"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        parsed_info = json.loads(out)
        self.assertEqual(parsed_info["backend"], "mock")
        self.assertEqual(parsed_info["device"], "sim42")
        self.assertEqual(parsed_info["used"], 0)
        self.assertEqual(parsed_info["capacity"], 512)

        rc, out, _ = _run_cli_captured(
            ["capabilities"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        parsed_caps = json.loads(out)
        self.assertEqual(parsed_caps, scenario_caps)

        _run_cli_captured(
            ["create", "uart0", "--baudrate=115200"],
            backend=self.backend,
        )
        self.assertIn(BlockRef.from_str("uart0"), self.backend.blocks)

        rc, _, _ = _run_cli_captured(
            ["reset"],
            backend=self.backend,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(self.backend.blocks, {})

    def test_unsupported_backend_command_returns_1(self) -> None:
        restricted_backend = MockBackend(unsupported_commands={"dump"})
        rc, _, err = _run_cli_captured(
            ["dump"],
            backend=restricted_backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("not supported", err)

    def test_capabilities_called_after_configure_and_without_open(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            cfg_path = Path(tempdir) / "caps_cfg.yaml"
            cfg_path.write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {},
                        "devices": {
                            "caps_dev": {
                                "backend": "cfg_caps",
                                "device": "sim99",
                                "dev": {"type": "mock"},
                                "capabilities": (
                                    mock_backend_mod.DEFAULT_MOCK_CAPABILITIES
                                ),
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            rc, out, _ = _run_cli_captured(
                [
                    "--config-file",
                    str(cfg_path),
                    "-d",
                    "caps_dev",
                    "capabilities",
                ],
                extra_backends={"cfg_caps": ConfigRequiredCapsBackend},
            )
        self.assertEqual(rc, 0)
        parsed = json.loads(out)
        self.assertEqual(parsed, mock_backend_mod.DEFAULT_MOCK_CAPABILITIES)

    def test_logger_hierarchy_prints_from_all_modules(self) -> None:
        self.assertFalse(hasattr(mock_backend_mod, "_LOG"))

        def _emit_logs_on_reset() -> None:
            logging.getLogger(cli_mod.__name__).info("cli-log-msg")
            logging.getLogger(cli_parsing_mod.__name__).debug("parsing-log-msg")
            logging.getLogger(lib_mod.__name__).info("lib-log-msg")
            logging.getLogger(backend_registry_mod.__name__).info(
                "backend-log-msg"
            )
            logging.getLogger(config_mod.__name__).info("config-log-msg")

        with mock.patch.object(
            self.backend, "reset", side_effect=_emit_logs_on_reset
        ):
            rc, _, err = _run_cli_captured(
                ["-vv", "reset"],
                backend=self.backend,
            )
        self.assertEqual(rc, 0)
        self.assertIn("cli-log-msg", err)
        self.assertIn("parsing-log-msg", err)
        self.assertIn("lib-log-msg", err)
        self.assertIn("backend-log-msg", err)
        self.assertIn("config-log-msg", err)

        # When -vv is passed and a command fails, a debug traceback is logged.
        rc, _, err = _run_cli_captured(
            ["-vv", "destroy", "invalid_block"],
            backend=self.backend,
        )
        self.assertEqual(rc, 1)
        self.assertIn("Command failed", err)
        self.assertIn("Traceback (most recent call last):", err)


if __name__ == "__main__":
    unittest.main()
