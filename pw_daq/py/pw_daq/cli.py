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
"""Command-line interface (daq-cli) for configuring and controlling DAQ."""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from contextlib import nullcontext
import enum
import json
import logging
from pathlib import Path
import sys
from typing import Any, NoReturn

import pw_cli.log
from pw_daq.backend import DaqBackend
from pw_daq.backend_registry import BackendRegistry, BackendTarget
from pw_daq.cli_parsing import (
    enum_allowed_labels,
    format_endpoint_value,
    format_event_cli,
    format_event_json,
    parse_action,
    parse_attribute,
    parse_block,
    parse_endpoint_value,
    parse_enum_member,
    parse_signal,
)
from pw_daq.common import (
    BlockRef,
    DaqError,
    ValueType,
)
from pw_daq.config import DaqConfig
from pw_daq.lib import (
    SUPPORTED_BLOCK_TYPES,
    AttributeMutability,
    AttributeSpec,
    Block,
    Daq,
    get_block_class,
)
import yaml

_LOG = logging.getLogger(__name__)


class _BlockAttributeAction(argparse.Action):
    """Stores parsed block attribute flags in `namespace.block_attributes`."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: Any,
        option_string: str | None = None,
    ) -> None:
        block_attrs = getattr(namespace, "block_attributes", None)
        if block_attrs is None:
            block_attrs = {}
            setattr(namespace, "block_attributes", block_attrs)
        block_attrs[self.dest] = values


class _BooleanBlockAttributeAction(argparse.BooleanOptionalAction):
    """Stores boolean block attribute flags in `namespace.block_attributes`."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: Any,
        option_string: str | None = None,
    ) -> None:
        if option_string is not None and option_string in self.option_strings:
            block_attrs = getattr(namespace, "block_attributes", None)
            if block_attrs is None:
                block_attrs = {}
                setattr(namespace, "block_attributes", block_attrs)
            block_attrs[self.dest] = not option_string.startswith("--no-")


class _DaqArgumentParser(argparse.ArgumentParser):
    """ArgumentParser that raises DaqError instead of exiting with code 2.

    Raising DaqError rather than calling sys.exit(2) is necessary so that:
    1. All argument parsing errors are routed through ``pw_cli.log`` for
       consistent error formatting instead of writing raw text to stderr.
    2. During two-stage bootstrap parsing, preliminary ``parse_known_args()``
       calls can inspect ``-v``/``--verbose`` and ``-c``/``--config-file``
       before logging and backend flags are initialized without prematurely
       exiting.
    """

    def error(self, message: str) -> NoReturn:
        raise DaqError(message)


def _add_global_arguments(
    parser: argparse.ArgumentParser,
) -> None:
    """Adds global CLI arguments shared across all commands."""
    parser.add_argument(
        "-d",
        "--device",
        type=str,
        default=None,
        help="Selected device (default: Determined by config).",
    )
    parser.add_argument(
        "-c",
        "--config-file",
        type=Path,
        default=None,
        help=(
            "Path to a fully specified YAML config file overriding all other "
            "configuration files."
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="count",
        default=0,
        help="Increase log verbosity.",
    )


def _add_attribute_flag(
    block_parser: argparse.ArgumentParser,
    attr_name: str,
    attr_spec: AttributeSpec,
) -> None:
    """Adds a CLI flag for a block attribute based on its AttributeSpec."""
    value_type = attr_spec.input_value_type
    if value_type is None:
        return
    is_required = attr_spec.required_on_create and attr_spec.default is None
    default_val = attr_spec.default

    help_parts: list[str] = []
    if attr_spec.description:
        help_parts.append(attr_spec.description)
    if is_required:
        help_parts.append("(required)")
    elif default_val is not None:
        formatted_default = format_endpoint_value(
            default_val,
            attr_spec.output_value_type,
        )
        help_parts.append(f"(default: {formatted_default})")
    help_text = " ".join(help_parts) if help_parts else None

    if value_type == ValueType.BOOL:
        block_parser.add_argument(
            f"--{attr_name}",
            dest=attr_name,
            action=_BooleanBlockAttributeAction,
            default=argparse.SUPPRESS,
            required=is_required,
            help=help_text,
        )
        return

    if value_type == ValueType.INT:
        arg_type: Any = int
    elif value_type.enum_cls is not None or value_type == ValueType.PIN:
        arg_type = str.lower
    else:
        arg_type = str

    kwargs: dict[str, Any] = {
        "dest": attr_name,
        "action": _BlockAttributeAction,
        "type": arg_type,
        "default": argparse.SUPPRESS,
        "required": is_required,
        "help": help_text,
    }
    if value_type.enum_cls is not None:
        kwargs["choices"] = enum_allowed_labels(value_type.enum_cls)
    elif attr_spec.allowed_values is not None:
        kwargs["choices"] = sorted(
            arg_type(v) for v in attr_spec.allowed_values
        )

    block_parser.add_argument(f"--{attr_name}", **kwargs)


def _build_block_create_parser(
    block_cls: type[Block],
    block_name: str,
) -> _DaqArgumentParser:
    """Builds a parser for `daq-cli create <block>` from `block_cls`."""
    block_parser = _DaqArgumentParser(
        prog=f"daq-cli create {block_name}",
        description=f"Create '{block_name}' block.",
    )
    block_parser.set_defaults(block_attributes={})
    for attr_name, attr_spec in block_cls.ATTRIBUTES.items():
        if attr_spec.mutability == AttributeMutability.CONST:
            continue
        _add_attribute_flag(block_parser, attr_name, attr_spec)
    return block_parser


def _parse_create_block_args(args: argparse.Namespace) -> None:
    """Parses block-specific `create` flags against the `lib.py` Block class."""
    block_ref = BlockRef.from_str(args.block)
    block_cls = get_block_class(block_ref.block_type)
    if block_cls is None:
        raise DaqError(
            f"Block type '{block_ref.block_type.value}' in '{block_ref}' is "
            "not supported."
        )
    block_parser = _build_block_create_parser(block_cls, block_ref.name)
    parsed_block = block_parser.parse_args(getattr(args, "block_args", []))
    args.block_attributes = getattr(parsed_block, "block_attributes", {})


def build_parser() -> argparse.ArgumentParser:
    """Builds the argument parser for daq-cli."""
    parser = _DaqArgumentParser(
        prog="daq-cli",
        description="Command line interface for interacting with DAQ devices.",
    )
    _add_global_arguments(parser)

    subparsers = parser.add_subparsers(
        dest="command",
        required=True,
        parser_class=_DaqArgumentParser,
    )

    # create <block> [block_args]
    supported_examples = ", ".join(
        f"{bt.value}<N>"
        for bt in sorted(SUPPORTED_BLOCK_TYPES, key=lambda b: b.value)
    )
    create_parser = subparsers.add_parser(
        "create",
        help="Create a block.",
    )
    create_parser.add_argument(
        "block",
        metavar="<block>",
        type=str,
        help=f"Block instance to create ({supported_examples}).",
    )
    create_parser.add_argument(
        "block_args",
        nargs=argparse.REMAINDER,
        help=(
            "Block-specific creation flags "
            "(pass '<block> --help' for options)."
        ),
    )

    # list
    subparsers.add_parser(
        "list",
        help="List all current blocks.",
    )

    # destroy <block>
    destroy_parser = subparsers.add_parser(
        "destroy",
        help="Tear down the specified block.",
    )
    destroy_parser.add_argument(
        "block",
        metavar="<block>",
        type=str,
        help="Block to tear down (e.g. digital1).",
    )

    # set <block>.<attribute> <value>
    set_parser = subparsers.add_parser(
        "set",
        help="Set an attribute on a block.",
    )
    set_parser.add_argument(
        "attribute",
        metavar="<block>.<attribute>",
        type=str,
        help="Block attribute reference (e.g. digital1.pull).",
    )
    set_parser.add_argument(
        "value",
        type=str,
        help="New value for the attribute.",
    )

    # get <block>.<attribute>
    get_parser = subparsers.add_parser(
        "get",
        help="Get an attribute from a block.",
    )
    get_parser.add_argument(
        "attribute",
        metavar="<block>.<attribute>",
        type=str,
        help="Block attribute reference (e.g. digital1.pull).",
    )

    # on <block>.<signal> do <block>.<action> [action_arg]
    on_parser = subparsers.add_parser(
        "on",
        help=(
            "When the specified Signal emits an Event, invoke the supplied "
            "Action."
        ),
    )
    on_parser.add_argument(
        "signal",
        metavar="<block>.<signal>",
        type=str,
        help="Signal endpoint (e.g. digital1.change).",
    )
    on_parser.add_argument(
        "do_keyword",
        metavar="do",
        type=str,
        help="Literal 'do' keyword separating signal and action.",
    )
    on_parser.add_argument(
        "action",
        metavar="<block>.<action>",
        type=str,
        help="Action endpoint (e.g. digital0.write).",
    )
    on_parser.add_argument(
        "action_arg",
        nargs="?",
        default=None,
        help="Optional argument for the action.",
    )

    # listen [-n N] [-t TIMEOUT] [--json] <block>.<signal>
    listen_parser = subparsers.add_parser(
        "listen",
        help="Stream new Events from the given Signal endpoint.",
    )
    listen_parser.add_argument(
        "-n",
        "--number",
        type=int,
        default=None,
        help="Number of events to receive then exit.",
    )
    listen_parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=None,
        help="Timeout in seconds to wait for events before exiting.",
    )
    listen_parser.add_argument(
        "--json",
        action="store_true",
        default=False,
        help="Output events as NDJSON (JSON Lines).",
    )
    listen_parser.add_argument(
        "signal",
        metavar="<block>.<signal>",
        type=str,
        help="Signal endpoint (e.g. uart1.rx).",
    )

    # wait [-t TIMEOUT] [--json] <block>.<signal>
    wait_parser = subparsers.add_parser(
        "wait",
        help="Wait for one new Event from the given Signal endpoint.",
    )
    wait_parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=None,
        help="Timeout in seconds to wait for an event before exiting.",
    )
    wait_parser.add_argument(
        "--json",
        action="store_true",
        default=False,
        help="Output event as NDJSON (JSON Lines).",
    )
    wait_parser.add_argument(
        "signal",
        metavar="<block>.<signal>",
        type=str,
        help="Signal endpoint (e.g. uart1.rx).",
    )

    # do <block>.<action> [action_arg]
    do_parser = subparsers.add_parser(
        "do",
        help="Invoke the specified Action.",
    )
    do_parser.add_argument(
        "action",
        metavar="<block>.<action>",
        type=str,
        help="Action endpoint (e.g. uart1.tx).",
    )
    do_parser.add_argument(
        "action_arg",
        nargs="?",
        default=None,
        help="Optional argument for the action.",
    )

    # dump
    subparsers.add_parser(
        "dump",
        help="Dump all Event logs from the DAQ device.",
    )

    # clear
    subparsers.add_parser(
        "clear",
        help="Clear all Event logs.",
    )

    # info
    subparsers.add_parser(
        "info",
        help=(
            "List information about the connected DAQ device or backend, "
            "including log usage and capacity if available."
        ),
    )

    # capabilities
    subparsers.add_parser(
        "capabilities",
        help="List all capabilities of the DAQ.",
    )

    # reset
    subparsers.add_parser(
        "reset",
        help="Reset the DAQ device state.",
    )

    # config
    subparsers.add_parser(
        "config",
        help="Print information about the applied DAQ configuration.",
    )

    return parser


def _handle_create(args: argparse.Namespace, daq: Daq) -> None:
    if not hasattr(args, "block_attributes") and hasattr(args, "block_args"):
        _parse_create_block_args(args)
    block = parse_block(daq, args.block)
    raw_attrs: Mapping[str, Any] = getattr(args, "block_attributes", {})
    parsed_attrs: dict[str, Any] = {}
    for attr_name, raw_val in raw_attrs.items():
        if raw_val is None:
            parsed_attrs[attr_name] = None
            continue
        spec = block.ATTRIBUTES.get(attr_name)
        enum_cls = (
            spec.input_value_type.enum_cls
            if spec is not None and spec.input_value_type is not None
            else None
        )
        if enum_cls is not None and isinstance(raw_val, str):
            parsed_attrs[attr_name] = parse_enum_member(
                enum_cls,
                raw_val,
                context=f"Attribute '{block.name}.{attr_name}'",
            )
        else:
            parsed_attrs[attr_name] = raw_val

    daq.create(block, **parsed_attrs)


def _handle_dump(daq: Daq) -> None:
    for event in daq.dump():
        sys.stdout.write(f"{format_event_json(event)}\n")
    sys.stdout.flush()


def _handle_on(args: argparse.Namespace, daq: Daq) -> None:
    if args.do_keyword != "do":
        raise DaqError(
            f"Expected 'do' after signal '{args.signal}', got "
            f"'{args.do_keyword}'."
        )
    signal = parse_signal(daq, args.signal)
    action = parse_action(daq, args.action)
    parsed_arg = (
        parse_endpoint_value(
            action.spec, args.action_arg, context=f"Action '{action}'"
        )
        if args.action_arg is not None
        else None
    )
    daq.on(signal, action, parsed_arg)


def _handle_listen(args: argparse.Namespace, daq: Daq) -> None:
    signal = parse_signal(daq, args.signal)
    formatter = (
        format_event_json if getattr(args, "json", False) else format_event_cli
    )
    try:
        count = 0
        with daq.listen(
            signal, count=args.number, timeout=args.timeout
        ) as events:
            for event in events:
                print(formatter(event), flush=True)
                count += 1
                if args.number is not None and count >= args.number:
                    break
    except KeyboardInterrupt:
        pass


def _handle_wait(args: argparse.Namespace, daq: Daq) -> None:
    signal = parse_signal(daq, args.signal)
    formatter = (
        format_event_json if getattr(args, "json", False) else format_event_cli
    )
    try:
        event = daq.wait(signal, timeout=args.timeout)
        print(formatter(event), flush=True)
    except KeyboardInterrupt:
        pass


def _handle_do(args: argparse.Namespace, daq: Daq) -> int:
    action = parse_action(daq, args.action)
    raw_arg = args.action_arg
    parsed_arg = parse_endpoint_value(
        action.spec, raw_arg, context=f"Action '{action}'"
    )
    result = daq.do(action, parsed_arg)
    if not result.success:
        return 1
    if result.value is not None:
        print(
            format_endpoint_value(result.value, result.value_type),
            flush=True,
        )
    return 0


def _handle_info(daq: Daq) -> None:
    info_data = daq.info()
    if isinstance(info_data, Mapping):
        print(
            json.dumps(
                info_data,
                indent=2,
                default=lambda o: (
                    o.value if isinstance(o, enum.Enum) else str(o)
                ),
            ),
            flush=True,
        )
    else:
        print(info_data, flush=True)


def _handle_config(daq_config: DaqConfig | None = None) -> None:
    if daq_config is None:
        daq_config = DaqConfig()
    print(
        yaml.safe_dump(dict(daq_config.config), sort_keys=False),
        end="",
        flush=True,
    )


def _execute_command(
    args: argparse.Namespace,
    daq: Daq,
    daq_config: DaqConfig | None = None,
) -> int:
    if args.command == "create":
        _handle_create(args, daq)
    elif args.command == "list":
        for block in daq.list():
            print(block, flush=True)
    elif args.command == "destroy":
        daq.destroy(parse_block(daq, args.block))
    elif args.command == "set":
        attr = parse_attribute(daq, args.attribute)
        parsed_val = parse_endpoint_value(
            attr.spec,
            args.value,
            context=f"Attribute '{attr}'",
        )
        daq.set(attr, parsed_val)
    elif args.command == "get":
        attr = parse_attribute(daq, args.attribute)
        val = daq.get(attr)
        if val is not None:
            print(
                format_endpoint_value(val, attr.output_value_type),
                flush=True,
            )
    elif args.command == "on":
        _handle_on(args, daq)
    elif args.command == "listen":
        _handle_listen(args, daq)
    elif args.command == "wait":
        _handle_wait(args, daq)
    elif args.command == "do":
        return _handle_do(args, daq)
    elif args.command == "dump":
        _handle_dump(daq)
    elif args.command == "clear":
        daq.clear()
    elif args.command == "info":
        _handle_info(daq)
    elif args.command == "capabilities":
        caps = daq.capabilities()
        if isinstance(caps, Mapping):
            print(json.dumps(caps, indent=2), flush=True)
        else:
            print(caps, flush=True)
    elif args.command == "reset":
        daq.reset()
    elif args.command == "config":
        _handle_config(daq_config)
    else:
        raise DaqError(f"Unknown command '{args.command}'.")
    return 0


def _configure_logging(verbose: int = 0) -> None:
    log_level = logging.WARNING
    if verbose == 1:
        log_level = logging.INFO
    elif verbose >= 2:
        log_level = logging.DEBUG
    pw_cli.log.install(
        level=log_level,
        hide_timestamp=False,
    )


def run_cli(
    argv: Sequence[str] | None = None,
    backend: DaqBackend | None = None,
    *,
    registry: BackendRegistry | None = None,
    extra_backends: Mapping[str, BackendTarget] | None = None,
) -> int:
    """Parses CLI arguments and executes the command against a DAQ backend."""
    # Default logging to start
    _configure_logging()

    # Bootstrap parser is used to check for -c/--config-file, -d/--device, and
    # -v/--verbose before loading configuration.
    bootstrap_parser = _DaqArgumentParser(add_help=False)
    _add_global_arguments(bootstrap_parser)

    try:
        bootstrap_args, remaining_args = bootstrap_parser.parse_known_args(argv)
        _configure_logging(bootstrap_args.verbose)

        if registry is not None and extra_backends is not None:
            raise DaqError(
                "Cannot specify both 'registry' and 'extra_backends'."
            )
        active_registry = (
            registry
            if registry is not None
            else BackendRegistry(extra_backends=extra_backends)
        )

        daq_config = DaqConfig(config_file=bootstrap_args.config_file)

        parser = build_parser()
        args = parser.parse_args(remaining_args)
        if args.command == "create":
            _parse_create_block_args(args)

        # We've finished loading all configs, now validate
        _LOG.debug(daq_config.config)
        daq_config.validate(registry=active_registry)

        # Shortcut "config" commands so we don't need to connect to a device
        if args.command == "config":
            _handle_config(daq_config)
            return 0

        if backend is not None:
            active_backend = backend
        else:
            device_name = (
                bootstrap_args.device
                if bootstrap_args.device is not None
                else daq_config.options.get("default_device")
            )
            if device_name is None:
                raise DaqError(
                    "No DAQ device selected. Specify one with '-d/--device' "
                    "or set 'options.default_device' in configuration."
                )
            device_config = daq_config.get_device_config(
                device_name, registry=active_registry
            )
            backend_name = device_config["backend"]
            active_backend = active_registry.create_backend(
                backend_name, device_config
            )

        daq = Daq(active_backend)
        if args.command == "capabilities":
            return _execute_command(args, daq, daq_config)
        ctx = nullcontext(daq) if backend is not None else daq
        with ctx:
            return _execute_command(args, daq, daq_config)
    except DaqError as exc:
        _LOG.error("%s", exc)
        _LOG.debug("Command failed", exc_info=True)
        return 1


def main(
    argv: Sequence[str] | None = None,
    *,
    registry: BackendRegistry | None = None,
    extra_backends: Mapping[str, BackendTarget] | None = None,
) -> int:
    """Main entry point for daq-cli."""
    return run_cli(
        argv=argv,
        registry=registry,
        extra_backends=extra_backends,
    )
