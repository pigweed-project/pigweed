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
"""Mock DAQ backend implementation for testing pw_daq and daq-cli."""

from __future__ import annotations

from collections import defaultdict, deque
from collections.abc import Iterable, Iterator, Mapping, Sequence
import copy
from dataclasses import dataclass
import math
from pathlib import Path
import re
from typing import Any

from pw_daq.backend import DaqBackend
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
    EventRef,
    EventStream,
    SignalRef,
    Value,
    ValueType,
)
from pw_daq.lib import (
    AttributeMutability,
    AttributeSpec,
    Block,
    get_block_class,
)

_VIRT_PIN_PATTERN = re.compile(r"^p\d+$")
_REQUIRED_CAPABILITY_KEYS = frozenset(
    {"blocks", "resources", "mappings", "config"}
)

DEFAULT_MOCK_CAPABILITIES: dict[str, Any] = {
    "blocks": [
        "digital0",
        "digital1",
        "digital2",
        "digital3",
        "digital4",
        "uart0",
        "uart1",
    ],
    "resources": [f"dio{i}" for i in range(9)],
    "mappings": {f"p{i}": f"dio{i}" for i in range(9)},
    "config": {
        "digital0": {"attributes": {"pin": "p0"}},
        "digital1": {"attributes": {"pin": "p1"}},
        "digital2": {"attributes": {"pin": "p2"}},
        "digital3": {"attributes": {"pin": "p3"}},
        "digital4": {"attributes": {"pin": "p4"}},
        "uart0": {"attributes": {"txpin": "p5", "rxpin": "p6"}},
        "uart1": {"attributes": {"txpin": "p7", "rxpin": "p8"}},
    },
}


@dataclass(frozen=True)
class _MockBlockSpec:
    """Resolved virtual-to-hardware mapping and BED constraints for a block."""

    ref: BlockRef
    block_cls: type[Block]
    const_attributes: Mapping[str, Value]
    hw_resources: tuple[str, ...]
    bed_allowed_values: Mapping[str, frozenset[Value]]


def _is_sequence_not_str(val: Any) -> bool:
    return isinstance(val, Sequence) and not isinstance(
        val, (str, bytes, bytearray, Mapping)
    )


def _coerce_bed_value(
    attr_spec: AttributeSpec,
    raw_val: Any,
    *,
    context: str,
) -> Value:
    """Validates and normalizes a BED allowed-value entry for `attr_spec`."""
    expected_type = attr_spec.output_value_type
    if (
        expected_type is not None
        and expected_type.enum_cls is not None
        and isinstance(raw_val, str)
    ):
        norm = raw_val.strip().lower()
        try:
            raw_val = expected_type.enum_cls(norm)
        except ValueError as exc:
            valid = ", ".join(m.value for m in expected_type.enum_cls)
            raise DaqError(
                f"Invalid BED value {raw_val!r} for {context}. "
                f"Expected one of: {valid}."
            ) from exc
    return attr_spec.validate_input(raw_val, context=context)


def _parse_bed_allowed_values(
    block_ref: BlockRef,
    block_cls: type[Block],
    attr_name: str,
    raw_constraint: Any,
) -> frozenset[Value]:
    """Parses a single value or sequence of BED allowed values for an attr."""
    attr_spec = block_cls.ATTRIBUTES.get(attr_name)
    if attr_spec is None or attr_spec.mutability == AttributeMutability.CONST:
        raise DaqError(
            f"Unsupported attribute '{attr_name}' in "
            f"'capabilities.config.{block_ref}'."
        )
    context = f"'capabilities.config.{block_ref}.attributes.{attr_name}'"
    items: Sequence[Any]
    if _is_sequence_not_str(raw_constraint):
        if len(raw_constraint) == 0:
            raise DaqError(
                f"BED value list for {context} must contain at least one item."
            )
        items = raw_constraint
    else:
        items = (raw_constraint,)

    allowed: set[Value] = set()
    for item in items:
        coerced = _coerce_bed_value(attr_spec, item, context=context)
        if (
            block_ref.block_type == BlockType.UART
            and attr_name == "baudrate"
            and isinstance(coerced, int)
            and coerced <= 0
        ):
            raise DaqError(
                f"BED baudrate in {context} must be positive, got {coerced!r}."
            )
        allowed.add(coerced)
    return frozenset(allowed)


def _resolve_mapped_pin(
    block_ref: BlockRef,
    attr_name: str,
    raw_pin: Any,
    pin_mappings: Mapping[str, str],
) -> tuple[str, str]:
    """Resolves a virtual pin ('p<N>') to `(virt_pin, hw_resource)`."""
    if not isinstance(raw_pin, str):
        raise DaqError(
            f"Invalid virtual pin {raw_pin!r} for '{block_ref}.{attr_name}'. "
            "Expected 'p<N>' (e.g. 'p0')."
        )
    virt_pin = raw_pin.strip().lower()
    if not _VIRT_PIN_PATTERN.match(virt_pin):
        raise DaqError(
            f"Invalid virtual pin '{raw_pin}' for '{block_ref}.{attr_name}'. "
            "Expected 'p<N>' (e.g. 'p0')."
        )
    hw_res = pin_mappings.get(virt_pin)
    if hw_res is None:
        raise DaqError(
            f"Virtual pin '{virt_pin}' for '{block_ref}.{attr_name}' is not "
            "mapped in 'capabilities.mappings'."
        )
    return virt_pin, hw_res


def _parse_blocks(raw_blocks: Any) -> list[tuple[BlockRef, type[Block]]]:
    if not _is_sequence_not_str(raw_blocks):
        raise DaqError(
            "Invalid 'capabilities.blocks': expected a list of block names."
        )
    seen: set[BlockRef] = set()
    parsed: list[tuple[BlockRef, type[Block]]] = []
    for raw_blk in raw_blocks:
        if not isinstance(raw_blk, str) or not raw_blk.strip():
            raise DaqError(
                f"Invalid block entry {raw_blk!r} in 'capabilities.blocks'."
            )
        b_ref = BlockRef.from_str(raw_blk)
        block_cls = get_block_class(b_ref.block_type)
        if block_cls is None:
            raise DaqError(
                f"Unsupported block type '{b_ref.block_type.value}' for "
                f"MockBackend block '{b_ref}'."
            )
        if b_ref not in seen:
            seen.add(b_ref)
            parsed.append((b_ref, block_cls))
    return parsed


def _parse_resources(raw_resources: Any) -> set[str]:
    if not _is_sequence_not_str(raw_resources):
        raise DaqError(
            "Invalid 'capabilities.resources': expected a list of resource "
            "names."
        )
    resources: set[str] = set()
    for raw_res in raw_resources:
        if not isinstance(raw_res, str) or not raw_res.strip():
            raise DaqError(
                "Entries in 'capabilities.resources' must be non-empty strings."
            )
        resources.add(raw_res.strip().lower())
    return resources


def _parse_mappings(
    raw_mappings: Any,
    resources: set[str],
) -> dict[str, str]:
    if not isinstance(raw_mappings, Mapping):
        raise DaqError(
            "Invalid 'capabilities.mappings': expected a mapping of virtual "
            "pins to hardware resources."
        )
    pin_mappings: dict[str, str] = {}
    for raw_virt, raw_real in raw_mappings.items():
        if not isinstance(raw_virt, str) or not raw_virt.strip():
            raise DaqError(
                f"Invalid virtual pin key {raw_virt!r} in "
                "'capabilities.mappings'. Expected 'p<N>' (e.g. 'p0')."
            )
        virt_pin = raw_virt.strip().lower()
        if not _VIRT_PIN_PATTERN.match(virt_pin):
            raise DaqError(
                f"Unsupported virtual resource '{raw_virt}' in "
                "'capabilities.mappings'. Expected 'p<N>' (e.g. 'p0')."
            )
        if virt_pin in pin_mappings:
            raise DaqError(
                f"Duplicate virtual pin '{virt_pin}' in "
                "'capabilities.mappings'."
            )
        if not isinstance(raw_real, str) or not raw_real.strip():
            raise DaqError(
                f"Invalid hardware resource {raw_real!r} mapped from "
                f"'{virt_pin}' in 'capabilities.mappings'."
            )
        real_res = raw_real.strip().lower()
        if real_res not in resources:
            raise DaqError(
                f"Hardware resource '{raw_real}' mapped from '{virt_pin}' is "
                "not declared in 'capabilities.resources'."
            )
        pin_mappings[virt_pin] = real_res
    return pin_mappings


def _parse_block_spec(
    block_ref: BlockRef,
    block_cls: type[Block],
    attrs: Mapping[str, Any],
    pin_mappings: Mapping[str, str],
) -> _MockBlockSpec:
    const_attrs: dict[str, Value] = {}
    hw_resources_list: list[str] = []

    if block_ref.block_type == BlockType.DIGITAL:
        if "pin" not in attrs:
            raise DaqError(
                f"Block '{block_ref}' is missing required 'pin' attribute in "
                "'capabilities.config'."
            )
        virt_pin, hw_res = _resolve_mapped_pin(
            block_ref, "pin", attrs["pin"], pin_mappings
        )
        const_attrs["pin"] = virt_pin
        hw_resources_list.append(hw_res)
        const_keys = {"pin"}
    elif block_ref.block_type == BlockType.UART:
        for req_pin in ("txpin", "rxpin"):
            if req_pin not in attrs:
                raise DaqError(
                    f"Block '{block_ref}' is missing required '{req_pin}' "
                    "attribute in 'capabilities.config'."
                )
        virt_tx, hw_tx_res = _resolve_mapped_pin(
            block_ref, "txpin", attrs["txpin"], pin_mappings
        )
        virt_rx, hw_rx_res = _resolve_mapped_pin(
            block_ref, "rxpin", attrs["rxpin"], pin_mappings
        )
        if hw_tx_res == hw_rx_res:
            raise DaqError(
                f"Block '{block_ref}' cannot map both 'txpin' and 'rxpin' to "
                f"the same hardware resource '{hw_tx_res}'."
            )
        const_attrs["txpin"] = virt_tx
        const_attrs["rxpin"] = virt_rx
        hw_resources_list.extend((hw_tx_res, hw_rx_res))
        const_keys = {"txpin", "rxpin"}
    else:
        raise DaqError(
            f"Unsupported block type '{block_ref.block_type.value}'."
        )

    bed_allowed: dict[str, frozenset[Value]] = {}
    for attr_name, raw_constraint in attrs.items():
        if attr_name in const_keys:
            continue
        bed_allowed[attr_name] = _parse_bed_allowed_values(
            block_ref, block_cls, attr_name, raw_constraint
        )

    return _MockBlockSpec(
        ref=block_ref,
        block_cls=block_cls,
        const_attributes=const_attrs,
        hw_resources=tuple(hw_resources_list),
        bed_allowed_values=bed_allowed,
    )


def _parse_capabilities(
    caps: Mapping[str, Any],
) -> tuple[dict[str, Any], dict[BlockRef, _MockBlockSpec]]:
    """Validates and resolves a MockBackend device capabilities mapping."""
    if not isinstance(caps, Mapping):
        raise DaqError(
            "Expected 'capabilities' in MockBackend config to be a mapping, "
            f"got {type(caps).__name__}."
        )
    missing = _REQUIRED_CAPABILITY_KEYS - set(caps.keys())
    extra = set(caps.keys()) - _REQUIRED_CAPABILITY_KEYS
    if missing:
        raise DaqError(
            "Invalid MockBackend capabilities: missing required key(s) "
            f"{', '.join(sorted(missing))}."
        )
    if extra:
        raise DaqError(
            "Invalid MockBackend capabilities: unexpected key(s) "
            f"{', '.join(sorted(extra))}."
        )

    parsed_blocks = _parse_blocks(caps["blocks"])
    resources = _parse_resources(caps["resources"])
    pin_mappings = _parse_mappings(caps["mappings"], resources)

    raw_config = caps["config"]
    if not isinstance(raw_config, Mapping):
        raise DaqError(
            "Invalid 'capabilities.config': expected a mapping of block "
            "configurations."
        )

    blocks_by_ref = dict(parsed_blocks)
    normalized_cfg_attrs: dict[BlockRef, dict[str, Any]] = {}
    for raw_blk_name, blk_cfg in raw_config.items():
        b_ref = BlockRef.from_str(str(raw_blk_name))
        if b_ref not in blocks_by_ref:
            raise DaqError(
                f"Block '{b_ref}' in 'capabilities.config' is not listed in "
                "'capabilities.blocks'."
            )
        if not isinstance(blk_cfg, Mapping):
            raise DaqError(
                f"Invalid config entry for '{b_ref}' in 'capabilities.config': "
                "expected a mapping."
            )
        unknown_cfg_keys = set(blk_cfg.keys()) - {"attributes"}
        if unknown_cfg_keys:
            raise DaqError(
                f"Unsupported key(s) in 'capabilities.config.{b_ref}': "
                f"{', '.join(sorted(str(k) for k in unknown_cfg_keys))}."
            )
        raw_attrs = blk_cfg.get("attributes", {})
        if not isinstance(raw_attrs, Mapping):
            raise DaqError(
                f"Invalid 'attributes' for '{b_ref}' in 'capabilities.config': "
                "expected a mapping."
            )
        normalized_cfg_attrs[b_ref] = {
            str(k).strip().lower(): v for k, v in raw_attrs.items()
        }

    specs: dict[BlockRef, _MockBlockSpec] = {}
    for b_ref, block_cls in parsed_blocks:
        attrs = normalized_cfg_attrs.get(b_ref, {})
        specs[b_ref] = _parse_block_spec(b_ref, block_cls, attrs, pin_mappings)

    return copy.deepcopy(dict(caps)), specs


def _resolve_block_ref(block: BlockRef | str) -> BlockRef:
    if isinstance(block, BlockRef):
        return block
    if isinstance(block, str):
        return BlockRef.from_str(block)
    raise DaqError(
        f"Expected a BlockRef or block name string, got {type(block).__name__}."
    )


class MockBackend(
    DaqBackend
):  # pylint: disable=too-many-instance-attributes,too-many-public-methods
    """In-memory mock DAQ backend for testing pw_daq and daq-cli."""

    def __init__(  # pylint: disable=too-many-arguments
        self,
        *,
        device: str = "mock",
        config_path: Path | str | None = None,
        conflicts: Mapping[BlockRef, Iterable[BlockRef]] | None = None,
        unsupported_commands: Iterable[str] | None = None,
        log_capacity: int = 1024,
        capabilities: Mapping[str, Any] | None = None,
    ) -> None:
        self.config_path: Path | None = (
            Path(config_path) if config_path is not None else None
        )
        self.device: str = device
        self.config: dict[str, Any] = {"device": device}
        if self.config_path is not None:
            self.config["config_path"] = str(self.config_path)
        self.recorded_calls: list[tuple[str, Any]] = []
        self.blocks: dict[BlockRef, dict[str, Any]] = {}
        self.pin_states: dict[BlockRef, DigitalState] = {}
        self._resource_owners: dict[str, BlockRef] = {}
        self.action_results: dict[ActionRef, ActionResult[Any]] = {}
        self._custom_action_endpoints: set[ActionRef] = set()
        self.triggers: list[tuple[SignalRef, ActionRef, Value]] = []
        self.event_queues: dict[SignalRef, deque[EventRef[Any]]] = defaultdict(
            deque
        )
        self.event_log: list[EventRef[Any]] = []
        self.log_capacity: int = log_capacity

        initial_caps = (
            capabilities
            if capabilities is not None
            else DEFAULT_MOCK_CAPABILITIES
        )
        parsed_caps, specs = _parse_capabilities(initial_caps)
        self.capabilities_data: dict[str, Any] = parsed_caps
        self._block_specs: dict[BlockRef, _MockBlockSpec] = specs

        self.conflicts: dict[BlockRef, set[BlockRef]] = {}
        if conflicts is not None:
            for block_a, conflicting_set in conflicts.items():
                copied = set(conflicting_set)
                self.conflicts.setdefault(block_a, set()).update(copied)
                for block_b in copied:
                    self.conflicts.setdefault(block_b, set()).add(block_a)

        self.unsupported_commands: set[str] = (
            set(unsupported_commands)
            if unsupported_commands is not None
            else set()
        )
        self.opened = False
        self.closed = False

    def name(self) -> str:
        return "mock"

    def open(self) -> None:
        self.opened = True

    def close(self) -> None:
        self.closed = True

    def _check_supported(self, command: str) -> None:
        if command in self.unsupported_commands:
            raise DaqError(
                f"Command '{command}' is not supported by MockBackend."
            )

    def _lookup_block_spec(self, block_ref: BlockRef) -> _MockBlockSpec:
        spec = self._block_specs.get(block_ref)
        if spec is None:
            raise DaqError(
                f"Unsupported block '{block_ref}' for configured capabilities."
            )
        return spec

    def _require_created_block(self, block_ref: BlockRef) -> _MockBlockSpec:
        spec = self._lookup_block_spec(block_ref)
        if block_ref not in self.blocks:
            raise DaqError(f"Block '{block_ref}' does not exist.")
        return spec

    @staticmethod
    def _check_bed_constraint(
        spec: _MockBlockSpec,
        attr_name: str,
        value: Value,
    ) -> None:
        allowed = spec.bed_allowed_values.get(attr_name)
        if allowed is not None and value not in allowed:
            allowed_str = ", ".join(sorted(str(v) for v in allowed))
            raise DaqError(
                f"Unsupported value {value!r} for '{spec.ref}.{attr_name}'. "
                f"Supported values: {allowed_str}."
            )

    def configure(
        self,
        config: Mapping[str, Any] | None = None,
    ) -> None:
        if config is None:
            return
        self.set_device_config(config)

    # ------------------------------------------------------------------
    # Test-only setup & state injection helpers
    # ------------------------------------------------------------------

    def set_device_config(self, config: Mapping[str, Any]) -> None:
        """Test helper: configures mock device metadata and capabilities."""
        if not isinstance(config, Mapping):
            raise DaqError(
                "Expected a configuration mapping for MockBackend, got "
                f"{type(config).__name__}."
            )
        self.config = dict(config)
        if config.get("device") is not None:
            self.device = str(config["device"])
        if config.get("config_path") is not None:
            self.config_path = Path(str(config["config_path"]))
        elif config.get("config") is not None:
            self.config_path = Path(str(config["config"]))
        if "capabilities" in config and config["capabilities"] is not None:
            parsed_caps, specs = _parse_capabilities(config["capabilities"])
            self.capabilities_data = parsed_caps
            self._block_specs = specs
        elif "capabilities" in config and config["capabilities"] is None:
            raise DaqError(
                "Expected 'capabilities' in MockBackend config to be a "
                "mapping, got NoneType."
            )

    def set_capabilities(self, capabilities: Mapping[str, Any]) -> None:
        """Test helper: replaces device capabilities and clears active state."""
        parsed_caps, specs = _parse_capabilities(capabilities)
        self.capabilities_data = parsed_caps
        self._block_specs = specs
        self.blocks.clear()
        self.pin_states.clear()
        self._resource_owners.clear()
        self.triggers.clear()

    def setup_block(
        self,
        block: BlockRef | str,
        attributes: Mapping[str, Any] | None = None,
        *,
        pin_state: DigitalState | None = None,
        validate: bool = True,
    ) -> BlockRef:
        """Test helper: pre-populates a block without recording a `create` call.

        When `validate=True` (default), attributes and conflicts are validated
        against the abstract DAQ spec and configured capabilities just like
        `create()`. Pass `validate=False` to inject arbitrary/invalid block
        state for testing `daq-lib` defensive checks.
        """
        block_ref = _resolve_block_ref(block)
        raw_attrs = dict(attributes) if attributes is not None else {}
        if not validate:
            self.blocks[block_ref] = raw_attrs
            if pin_state is not None:
                self.pin_states[block_ref] = pin_state
            return block_ref

        spec = self._lookup_block_spec(block_ref)
        stored, init_pin_state = self._resolve_create_attributes(
            spec, raw_attrs
        )
        self._handle_conflicts(block_ref, spec.hw_resources)
        self.blocks[block_ref] = stored
        for res in spec.hw_resources:
            self._resource_owners[res] = block_ref
        if block_ref.block_type == BlockType.DIGITAL:
            self.pin_states[block_ref] = (
                pin_state if pin_state is not None else init_pin_state
            )
        return block_ref

    def set_raw_attribute(
        self,
        attribute: AttributeRef | str,
        value: Any,
    ) -> None:
        """Test helper: injects an unvalidated attribute value on a block."""
        attr_ref = (
            AttributeRef.from_str(attribute)
            if isinstance(attribute, str)
            else attribute
        )
        if not isinstance(attr_ref, AttributeRef):
            raise DaqError(
                "Expected an AttributeRef or '<block>.<attr>' string, got "
                f"{type(attr_ref).__name__}."
            )
        if attr_ref.block not in self.blocks:
            raise DaqError(f"Block '{attr_ref.block}' does not exist.")
        self.blocks[attr_ref.block][attr_ref.attribute] = value

    def set_pin_state(
        self,
        block: BlockRef | str,
        state: DigitalState,
    ) -> None:
        """Test helper: sets the simulated pin state of a digital block."""
        block_ref = _resolve_block_ref(block)
        if not isinstance(state, DigitalState):
            raise DaqError(
                f"Expected a DigitalState instance, got {type(state).__name__}."
            )
        self.pin_states[block_ref] = state

    def get_pin_state(self, block: BlockRef | str) -> DigitalState:
        """Test helper: returns the simulated pin state of a digital block."""
        block_ref = _resolve_block_ref(block)
        if block_ref not in self.pin_states:
            raise DaqError(f"No pin state recorded for '{block_ref}'.")
        return self.pin_states[block_ref]

    def set_action_result(
        self,
        action: ActionRef | str,
        result: ActionResult[Any] | None,
        *,
        allow_custom_endpoint: bool = False,
    ) -> None:
        """Test helper: stubs or clears the ActionResult returned by `do()`."""
        action_ref = (
            ActionRef.from_str(action) if isinstance(action, str) else action
        )
        if not isinstance(action_ref, ActionRef):
            raise DaqError(
                "Expected an ActionRef or '<block>.<action>' string, got "
                f"{type(action_ref).__name__}."
            )
        if result is None:
            self.action_results.pop(action_ref, None)
            self._custom_action_endpoints.discard(action_ref)
            return
        self.action_results[action_ref] = result
        if allow_custom_endpoint:
            self._custom_action_endpoints.add(action_ref)
        else:
            self._custom_action_endpoints.discard(action_ref)

    def add_conflict(
        self,
        block_a: BlockRef | str,
        block_b: BlockRef | str,
    ) -> None:
        """Test helper: registers a bidirectional conflict between blocks."""
        ref_a = _resolve_block_ref(block_a)
        ref_b = _resolve_block_ref(block_b)
        self.conflicts.setdefault(ref_a, set()).add(ref_b)
        self.conflicts.setdefault(ref_b, set()).add(ref_a)

    def queue_event(self, event: EventRef[Any]) -> None:
        """Test helper: enqueues an EventRef for `listen` and `dump`."""
        self.event_queues[event.endpoint].append(event)
        self.event_log.append(event)

    # ------------------------------------------------------------------
    # Internal validation & conflict helpers
    # ------------------------------------------------------------------

    def _handle_conflicts(
        self,
        block_ref: BlockRef,
        hw_resources: Iterable[str],
    ) -> None:
        conflicting: list[BlockRef] = []
        if block_ref in self.blocks:
            conflicting.append(block_ref)

        for res in hw_resources:
            owner = self._resource_owners.get(res)
            if (
                owner is not None
                and owner != block_ref
                and owner not in conflicting
            ):
                conflicting.append(owner)

        for other in self.conflicts.get(block_ref, set()):
            if other in self.blocks and other not in conflicting:
                conflicting.append(other)

        if conflicting:
            names = ", ".join(str(b) for b in conflicting)
            raise DaqError(
                f"Cannot create '{block_ref}': conflicts with existing "
                f"block(s) {names}."
            )

    def _resolve_create_attributes(
        self,
        spec: _MockBlockSpec,
        attributes: Mapping[str, Any],
    ) -> tuple[dict[str, Any], DigitalState]:
        block_instance = spec.block_cls(spec.ref.index, self)
        validated = block_instance.validate_create_attributes(attributes)

        stored: dict[str, Any] = dict(spec.const_attributes)
        init_pin_state = DigitalState.INACTIVE

        if spec.ref.block_type == BlockType.DIGITAL:
            direction: DigitalDirection = validated["direction"]
            self._check_bed_constraint(spec, "direction", direction)
            stored["direction"] = direction

            if direction in (
                DigitalDirection.OUT,
                DigitalDirection.OPENDRAIN,
            ):
                raw_init = validated.get("initial")
                initial: DigitalState = (
                    raw_init
                    if raw_init is not None
                    else spec.block_cls.ATTRIBUTES["initial"].default
                )
                self._check_bed_constraint(spec, "initial", initial)
                stored["initial"] = initial
                init_pin_state = initial

            if direction in (
                DigitalDirection.IN,
                DigitalDirection.OPENDRAIN,
            ):
                raw_pull = validated.get("pull")
                pull: DigitalPull = (
                    raw_pull
                    if raw_pull is not None
                    else spec.block_cls.ATTRIBUTES["pull"].default
                )
                self._check_bed_constraint(spec, "pull", pull)
                stored["pull"] = pull

            return stored, init_pin_state

        for attr_name, attr_spec in spec.block_cls.ATTRIBUTES.items():
            if attr_spec.mutability == AttributeMutability.CONST:
                continue
            val = validated.get(attr_name)
            if val is None:
                allowed = spec.bed_allowed_values.get(attr_name)
                if (
                    allowed is not None
                    and attr_spec.default not in allowed
                    and len(allowed) == 1
                ):
                    val = next(iter(allowed))
                else:
                    val = attr_spec.default
            self._check_bed_constraint(spec, attr_name, val)
            stored[attr_name] = val

        return stored, init_pin_state

    # ------------------------------------------------------------------
    # DaqBackend implementation
    # ------------------------------------------------------------------

    def create(
        self,
        block_ref: BlockRef,
        **attributes: Any,
    ) -> BlockRef:
        self._check_supported("create")
        if not isinstance(block_ref, BlockRef):
            raise DaqError(
                f"Expected a BlockRef instance, got {type(block_ref).__name__}."
            )
        spec = self._lookup_block_spec(block_ref)
        stored_attrs, init_state = self._resolve_create_attributes(
            spec, attributes
        )
        self._handle_conflicts(block_ref, spec.hw_resources)
        self.blocks[block_ref] = stored_attrs
        for res in spec.hw_resources:
            self._resource_owners[res] = block_ref
        if block_ref.block_type == BlockType.DIGITAL:
            self.pin_states[block_ref] = init_state
        self.recorded_calls.append(("create", (block_ref, dict(attributes))))
        return block_ref

    def list(self) -> list[BlockRef]:
        self._check_supported("list")
        self.recorded_calls.append(("list", None))
        return list(self.blocks.keys())

    def destroy(self, block_ref: BlockRef) -> None:
        self._check_supported("destroy")
        if not isinstance(block_ref, BlockRef):
            raise DaqError(
                f"Expected a BlockRef instance, got {type(block_ref).__name__}."
            )
        self._require_created_block(block_ref)
        self.blocks.pop(block_ref, None)
        self.pin_states.pop(block_ref, None)
        for res, owner in list(self._resource_owners.items()):
            if owner == block_ref:
                del self._resource_owners[res]
        self.recorded_calls.append(("destroy", block_ref))

    def set(
        self,
        attribute_ref: AttributeRef,
        value: Value,
    ) -> None:
        self._check_supported("set")
        if not isinstance(attribute_ref, AttributeRef):
            raise DaqError(
                "Expected an AttributeRef instance, got "
                f"{type(attribute_ref).__name__}."
            )
        resolved_ref = attribute_ref.block
        attr_name = attribute_ref.attribute
        spec = self._lookup_block_spec(resolved_ref)
        attr_spec = spec.block_cls.ATTRIBUTES.get(attr_name)
        if attr_spec is None:
            raise DaqError(
                f"Unknown attribute '.{attr_name}' on block '{resolved_ref}'."
            )
        if resolved_ref not in self.blocks:
            raise DaqError(f"Block '{resolved_ref}' does not exist.")

        if attr_spec.mutability == AttributeMutability.CONST:
            raise DaqError(
                f"Attribute '{attribute_ref}' is const and cannot "
                "be modified."
            )
        if attr_spec.mutability == AttributeMutability.INIT:
            raise DaqError(
                f"Attribute '{attribute_ref}' is init-only and "
                "cannot be modified at run-time."
            )
        if (
            resolved_ref.block_type == BlockType.DIGITAL
            and attr_name == "pull"
            and self.blocks[resolved_ref].get("direction")
            == DigitalDirection.OUT
        ):
            raise DaqError(
                f"Attribute '.pull' is not valid in 'out' mode for "
                f"'{resolved_ref}'."
            )
        validated_val = attr_spec.validate_input(
            value, context=f"Attribute '{attribute_ref}'"
        )
        self._check_bed_constraint(spec, attr_name, validated_val)
        self.blocks[resolved_ref][attr_name] = validated_val
        self.recorded_calls.append(("set", (attribute_ref, validated_val)))

    def get(
        self,
        attribute_ref: AttributeRef,
    ) -> Value:
        self._check_supported("get")
        if not isinstance(attribute_ref, AttributeRef):
            raise DaqError(
                "Expected an AttributeRef instance, got "
                f"{type(attribute_ref).__name__}."
            )
        resolved_ref = attribute_ref.block
        attr_name = attribute_ref.attribute
        spec = self._lookup_block_spec(resolved_ref)
        if attr_name not in spec.block_cls.ATTRIBUTES:
            raise DaqError(
                f"Unknown attribute '.{attr_name}' on block '{resolved_ref}'."
            )
        if resolved_ref not in self.blocks:
            raise DaqError(f"Block '{resolved_ref}' does not exist.")
        block_attrs = self.blocks[resolved_ref]
        if attr_name not in block_attrs:
            raise DaqError(
                f"Attribute '{attribute_ref}' is not active in "
                "current block mode."
            )
        self.recorded_calls.append(("get", attribute_ref))
        return block_attrs[attr_name]

    def _validate_signal_endpoint(
        self, signal_ref: SignalRef
    ) -> _MockBlockSpec:
        spec = self._require_created_block(signal_ref.block)
        if signal_ref.signal not in spec.block_cls.SIGNALS:
            raise DaqError(f"Unknown Signal endpoint '{signal_ref}'.")
        if (
            signal_ref.block.block_type == BlockType.DIGITAL
            and signal_ref.signal in ("risingedge", "fallingedge")
            and self.blocks[signal_ref.block].get("direction")
            != DigitalDirection.IN
        ):
            raise DaqError(
                f"Signal '.{signal_ref.signal}' on '{signal_ref.block}' is "
                "only valid when direction is 'in'."
            )
        return spec

    def _validate_action_endpoint(
        self,
        action_ref: ActionRef,
        action_arg: Value,
        *,
        validate_arg: bool = True,
    ) -> tuple[_MockBlockSpec, Value]:
        spec = self._require_created_block(action_ref.block)
        if action_ref in self._custom_action_endpoints:
            return spec, action_arg
        act_spec = spec.block_cls.ACTIONS.get(action_ref.action)
        if act_spec is None:
            raise DaqError(f"Unknown Action endpoint '{action_ref}'.")
        if (
            action_ref.block.block_type == BlockType.DIGITAL
            and action_ref.action in ("write", "toggle")
            and self.blocks[action_ref.block].get("direction")
            == DigitalDirection.IN
        ):
            raise DaqError(
                f"Cannot invoke '.{action_ref.action}' on "
                f"'{action_ref.block}' when direction is 'in'."
            )
        if not validate_arg:
            return spec, action_arg
        validated_arg = act_spec.validate_input(
            action_arg, context=f"Action '{action_ref}'"
        )
        return spec, validated_arg

    def on(
        self,
        signal_ref: SignalRef,
        action_ref: ActionRef,
        action_arg: Value = None,
    ) -> None:
        self._check_supported("on")
        if not isinstance(signal_ref, SignalRef):
            raise DaqError(
                "Expected a SignalRef instance, got "
                f"{type(signal_ref).__name__}."
            )
        if not isinstance(action_ref, ActionRef):
            raise DaqError(
                "Expected an ActionRef instance, got "
                f"{type(action_ref).__name__}."
            )
        sig_blk_spec = self._validate_signal_endpoint(signal_ref)
        sig_spec = sig_blk_spec.block_cls.SIGNALS[signal_ref.signal]

        act_blk_spec = self._require_created_block(action_ref.block)
        act_spec = act_blk_spec.block_cls.ACTIONS.get(action_ref.action)
        if act_spec is None:
            raise DaqError(f"Unknown Action endpoint '{action_ref}'.")
        self._validate_action_endpoint(
            action_ref, action_arg, validate_arg=False
        )

        if action_arg is not None or act_spec.input_value_type is None:
            validated_arg = act_spec.validate_input(
                action_arg, context=f"Action '{action_ref}'"
            )
        else:
            if not sig_spec.matches_input(act_spec):
                raise DaqError(
                    f"Type mismatch between Signal '{signal_ref}' and Action "
                    f"'{action_ref}'."
                )
            validated_arg = None

        self.triggers.append((signal_ref, action_ref, validated_arg))
        self.recorded_calls.append(
            ("on", (signal_ref, action_ref, validated_arg))
        )

    def _iter_events(
        self,
        signal_ref: SignalRef,
        count: int | None,
    ) -> Iterator[EventRef[Any]]:
        queue = self.event_queues.get(signal_ref)
        if not queue:
            return
        yielded = 0
        while queue:
            if count is not None and yielded >= count:
                break
            yield queue.popleft()
            yielded += 1

    def listen(
        self,
        signal_ref: SignalRef,
        count: int | None = None,
        timeout: float | None = None,
    ) -> EventStream[EventRef[Any]]:
        self._check_supported("listen")
        if not isinstance(signal_ref, SignalRef):
            raise DaqError(
                "Expected a SignalRef instance, got "
                f"{type(signal_ref).__name__}."
            )
        if count is not None and (
            not isinstance(count, int) or isinstance(count, bool) or count <= 0
        ):
            raise DaqError("'count' must be a positive integer.")
        if timeout is not None and (
            not isinstance(timeout, (int, float))
            or isinstance(timeout, bool)
            or not math.isfinite(timeout)
            or timeout <= 0
        ):
            raise DaqError("'timeout' must be a positive number.")
        self._validate_signal_endpoint(signal_ref)
        self.recorded_calls.append(("listen", (signal_ref, count, timeout)))
        return EventStream(self._iter_events(signal_ref, count))

    def do(
        self,
        action_ref: ActionRef,
        action_arg: Value = None,
    ) -> ActionResult[Any]:
        self._check_supported("do")
        if not isinstance(action_ref, ActionRef):
            raise DaqError(
                "Expected an ActionRef instance, got "
                f"{type(action_ref).__name__}."
            )
        _, validated_arg = self._validate_action_endpoint(
            action_ref, action_arg
        )
        self.recorded_calls.append(("do", (action_ref, validated_arg)))
        if action_ref in self.action_results:
            return self.action_results[action_ref]

        if action_ref.block.block_type == BlockType.DIGITAL:
            if action_ref.action == "write":
                assert isinstance(validated_arg, DigitalState)
                self.pin_states[action_ref.block] = validated_arg
                return ActionResult(success=True)
            if action_ref.action == "toggle":
                current = self.pin_states.get(
                    action_ref.block, DigitalState.INACTIVE
                )
                self.pin_states[action_ref.block] = (
                    DigitalState.INACTIVE
                    if current == DigitalState.ACTIVE
                    else DigitalState.ACTIVE
                )
                return ActionResult(success=True)
            if action_ref.action == "read":
                state = self.pin_states.get(
                    action_ref.block, DigitalState.INACTIVE
                )
                return ActionResult(
                    success=True,
                    value=state,
                    value_type=ValueType.DIGITAL_STATE,
                )

        if (
            action_ref.block.block_type == BlockType.UART
            and action_ref.action == "tx"
        ):
            return ActionResult(success=True)

        raise DaqError(f"Unsupported Action endpoint '{action_ref}'.")

    def dump(self) -> Iterable[EventRef[Any]]:
        self._check_supported("dump")
        self.recorded_calls.append(("dump", None))
        return list(self.event_log)

    def clear(self) -> None:
        self._check_supported("clear")
        self.event_log.clear()
        self.event_queues.clear()
        self.recorded_calls.append(("clear", None))

    def info(self) -> Mapping[str, Any]:
        self._check_supported("info")
        self.recorded_calls.append(("info", None))
        return {
            "backend": "mock",
            "device": self.device if self.device is not None else "mock",
            "config": (
                str(self.config_path)
                if self.config_path is not None
                else "none"
            ),
            "used": len(self.event_log),
            "capacity": self.log_capacity,
        }

    def capabilities(self) -> Mapping[str, Any]:
        self._check_supported("capabilities")
        self.recorded_calls.append(("capabilities", None))
        return copy.deepcopy(self.capabilities_data)

    def reset(self) -> None:
        self._check_supported("reset")
        self.blocks.clear()
        self.pin_states.clear()
        self._resource_owners.clear()
        self.triggers.clear()
        self.event_queues.clear()
        self.event_log.clear()
        self.recorded_calls.append(("reset", None))
