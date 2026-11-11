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

"""pw_rp2350 debug probe command line interface."""

from __future__ import annotations

import argparse
import logging
import sys
from typing import TYPE_CHECKING, Any, Protocol

import pw_cli.log
import usb.core  # type: ignore[import-untyped]
from pw_cli.interactive_prompts import interactive_index_select
from rp2040_utils import device_detector

from pw_rp2350_debug_probe.debug_probe import DebugProbe, SwdTarget

if TYPE_CHECKING:
    from collections.abc import Sequence

_LOG = logging.getLogger('debug_probe_cli')
_MAX_RGB_CHANNEL = 255


class _SubParserFactory(Protocol):
    """Protocol for registering subcommand parsers on an ArgumentParser."""

    def add_parser(
        self,
        name: str,
        **kwargs: Any,  # noqa: ANN401
    ) -> argparse.ArgumentParser:
        """Registers and returns a subcommand ArgumentParser."""


def _parse_rgb_channel(value: str) -> int:
    """Parses a single RGB channel value in the inclusive range 0..255."""
    try:
        channel = int(value, 0)
    except ValueError as err:
        msg = f'Invalid RGB channel value {value!r}; expected an integer.'
        raise argparse.ArgumentTypeError(msg) from err

    if not 0 <= channel <= _MAX_RGB_CHANNEL:
        msg = (
            f'RGB channel value {channel} out of range; '
            f'expected 0..{_MAX_RGB_CHANNEL}.'
        )
        raise argparse.ArgumentTypeError(msg)

    return channel


def _format_board(board: device_detector.PicoBoardInfo) -> str:
    """Formats a detected board's USB location and identity for display."""
    return (
        f'bus {board.bus}, port {board.port}'
        f' ({board.manufacturer} - {board.product} -'
        f' {board.current_serial_number()})'
    )


def add_debug_probe_args(
    parser: argparse.ArgumentParser,
    *,
    suppress_defaults: bool = False,
) -> None:
    """Adds shared debug probe selection and logging flags to a parser."""
    parser.add_argument(
        '--usb-bus',
        '--bus',
        dest='usb_bus',
        type=int,
        default=argparse.SUPPRESS if suppress_defaults else None,
        help='The USB bus of the debug probe. Typically something like: 1.',
    )
    parser.add_argument(
        '--usb-port',
        '--port',
        dest='usb_port',
        type=str,
        default=argparse.SUPPRESS if suppress_defaults else None,
        help=(
            'The USB port of the debug probe. Typically something like: "1:4".'
        ),
    )
    parser.add_argument(
        '--interactive',
        action=argparse.BooleanOptionalAction,
        default=argparse.SUPPRESS if suppress_defaults else True,
        help='Enable interactive mode.',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        dest='_subcommand_verbose' if suppress_defaults else 'verbose',
        default=argparse.SUPPRESS if suppress_defaults else 0,
        action='count',
        help='Increase log verbosity. May be specified multiple times.',
    )


def add_list_args(
    subparser: _SubParserFactory,
    *,
    parents: Sequence[argparse.ArgumentParser] = (),
) -> None:
    """Adds the `list` subcommand parser."""
    subparser.add_parser(
        'list',
        parents=list(parents),
        help='List attached debug probes.',
    )


def add_target_args(
    subparser: _SubParserFactory,
    *,
    parents: Sequence[argparse.ArgumentParser] = (),
) -> None:
    """Adds the `target` subcommand parser."""
    target_parser = subparser.add_parser(
        'target',
        parents=list(parents),
        help='Set target via debug probe.',
    )
    target_parser.add_argument(
        'select',
        choices=['dut', 'sim', 'buddy'],
        help='Which device to target with the debug probe.',
    )


def add_led_args(
    subparser: _SubParserFactory,
    *,
    parents: Sequence[argparse.ArgumentParser] = (),
) -> None:
    """Adds the `led` subcommand parser."""
    led_parser = subparser.add_parser(
        'led',
        parents=list(parents),
        help='Set the debug probe LED color.',
    )
    led_parser.add_argument(
        '-r', '--red', action='store_true', help='Turn on the red LED.'
    )
    led_parser.add_argument(
        '-g', '--green', action='store_true', help='Turn on the green LED.'
    )
    led_parser.add_argument(
        '-b', '--blue', action='store_true', help='Turn on the blue LED.'
    )
    led_parser.add_argument(
        '--off', action='store_true', help='Turn the LED off.'
    )
    led_parser.add_argument(
        '--rgb',
        type=_parse_rgb_channel,
        nargs=3,
        metavar=('R', 'G', 'B'),
        help='Set the LED using RGB channel values (0-255 or 0x00-0xFF).',
    )


def add_info_args(
    subparser: _SubParserFactory,
    *,
    parents: Sequence[argparse.ArgumentParser] = (),
) -> None:
    """Adds the `info` subcommand parser."""
    subparser.add_parser(
        'info',
        parents=list(parents),
        help='Query debug probe and connected test target info.',
    )


def _build_parser() -> argparse.ArgumentParser:
    """Constructs the top-level CLI ArgumentParser and subparsers."""
    common_parser = argparse.ArgumentParser(add_help=False)
    add_debug_probe_args(common_parser)

    subcommand_common_parser = argparse.ArgumentParser(add_help=False)
    add_debug_probe_args(subcommand_common_parser, suppress_defaults=True)

    parser = argparse.ArgumentParser(
        prog='debug_probe_cli',
        description='CLI for interacting with the pw_rp2350 debug probe.',
        parents=[common_parser],
    )
    subparsers = parser.add_subparsers(dest='command', required=True)
    add_list_args(subparsers, parents=[subcommand_common_parser])
    add_target_args(subparsers, parents=[subcommand_common_parser])
    add_led_args(subparsers, parents=[subcommand_common_parser])
    add_info_args(subparsers, parents=[subcommand_common_parser])
    return parser


def _select_usb_port_device(
    usb_bus: int,
    usb_port: str,
) -> device_detector.PicoDebugProbeBoardInfo | None:
    """Selects a debug probe board at an explicit USB bus and port."""
    try:
        board = device_detector.board_from_usb_port(
            usb_bus,
            usb_port,
            include_picos=True,
            include_debug_probes=True,
        )
    except (ValueError, usb.core.USBError) as err:
        _LOG.error('%s', err)
        return None

    if not isinstance(board, device_detector.PicoDebugProbeBoardInfo):
        _LOG.error(
            'Device on bus %d port %s is not a debug probe.',
            usb_bus,
            usb_port,
        )
        return None
    return board


def _select_device(
    args: argparse.Namespace,
) -> device_detector.PicoDebugProbeBoardInfo | None:
    """Selects a debug probe board based on CLI arguments."""
    if args.usb_bus is not None and args.usb_port is not None:
        return _select_usb_port_device(args.usb_bus, args.usb_port)

    boards = [
        board
        for board in device_detector.detect_boards(
            include_picos=False, include_debug_probes=True
        )
        if isinstance(board, device_detector.PicoDebugProbeBoardInfo)
    ]

    if not boards:
        _LOG.error('Could not find an attached debug probe.')
        return None
    if len(boards) == 1:
        _LOG.info('Only one debug probe detected.')
        return boards[0]
    if not args.interactive or not sys.stdin.isatty():
        _LOG.error(
            'Multiple debug probes detected in non-interactive mode. '
            'Please specify a probe using --usb-bus and --usb-port.'
        )
        return None

    print('Multiple debug probes detected. Please select one:')  # noqa: T201
    board_lines = [_format_board(board) for board in boards]
    user_input_index, _user_input_text = interactive_index_select(board_lines)
    return boards[user_input_index]


def _validate_args(
    parser: argparse.ArgumentParser,
    args: argparse.Namespace,
) -> None:
    """Validates cross-flag CLI invariants before USB device discovery."""
    if (args.usb_port is None) != (args.usb_bus is None):
        parser.error(
            'Must specify BOTH --usb-bus and --usb-port when manually'
            ' specifying a device'
        )

    if args.command == 'led':
        has_channel_flag = args.red or args.green or args.blue
        if sum([bool(args.off), args.rgb is not None, has_channel_flag]) != 1:
            parser.error('Specify exactly one of --off, --rgb, or -r/-g/-b.')


def _dispatch_led(args: argparse.Namespace, probe: DebugProbe) -> None:
    """Applies the requested LED color configuration to the debug probe."""
    if args.off:
        probe.set_rgb_led(0, 0, 0)
        return
    if args.rgb is not None:
        probe.set_rgb_led(*args.rgb)
        return
    probe.set_rgb_led(
        _MAX_RGB_CHANNEL if args.red else 0,
        _MAX_RGB_CHANNEL if args.green else 0,
        _MAX_RGB_CHANNEL if args.blue else 0,
    )


def _dispatch_command(args: argparse.Namespace, probe: DebugProbe) -> None:
    """Executes the selected CLI subcommand against an open debug probe."""
    if args.command == 'target':
        if args.select == 'dut':
            probe.set_target(SwdTarget.DUT)
        elif args.select in ('sim', 'buddy'):
            probe.set_target(SwdTarget.SIM)
        return

    if args.command == 'info':
        greeting = probe.hello()
        target_info = probe.get_target_info()
        capabilities = (
            ', '.join(cap.name for cap in target_info.capabilities)
            if target_info.capabilities
            else 'none'
        )
        print(f'Probe: {greeting}')  # noqa: T201
        print(f'Target: {target_info.name}')  # noqa: T201
        print(f'MCU: {target_info.mcu}')  # noqa: T201
        print(f'Capabilities: {capabilities}')  # noqa: T201
        return

    if args.command == 'led':
        _dispatch_led(args, probe)


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point for debug probe interactions."""
    parser = _build_parser()
    args = parser.parse_args(argv)
    args.verbose += vars(args).pop('_subcommand_verbose', 0)

    if args.verbose == 0:
        log_level = logging.WARNING
    elif args.verbose == 1:
        log_level = logging.INFO
    else:
        log_level = logging.DEBUG

    pw_cli.log.install(level=log_level, hide_timestamp=False)

    _validate_args(parser, args)

    try:
        if args.command == 'list':
            boards = device_detector.detect_boards(
                include_picos=False, include_debug_probes=True
            )
            for board in boards:
                print(_format_board(board))  # noqa: T201
            return 0

        device = _select_device(args)
        if device is None:
            return 1

        with DebugProbe(device.usb_device()) as probe:
            _dispatch_command(args, probe)
    except (ValueError, usb.core.USBError) as err:
        _LOG.error('Debug probe communication error: %s', err)
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
