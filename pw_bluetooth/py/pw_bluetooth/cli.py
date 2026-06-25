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
"""Bluetooth CLI for Pigweed."""

import argparse
from pathlib import Path
import sys
import types
from typing import Optional

from pw_bluetooth.preferences import BluetoothPrefs

fuchsia_utils: Optional[types.ModuleType] = None
try:
    # pylint: disable=import-error
    import pw_bluetooth_sapphire.fuchsia_utils  # type: ignore[import-not-found]

    # pylint: enable=import-error
    fuchsia_utils = pw_bluetooth_sapphire.fuchsia_utils
except ImportError:
    pass


def main():
    """Entry point"""
    parser = argparse.ArgumentParser(prog='pw bluetooth', description=__doc__)
    subparsers = parser.add_subparsers(dest='subcommand', required=True)

    # config subcommand
    config_parser = subparsers.add_parser(
        'set-fuchsia-checkout',
        help='Persistently set the Fuchsia checkout path',
    )
    config_parser.add_argument(
        'path', type=Path, help='Path to the Fuchsia checkout'
    )

    def set_config(args):
        prefs = BluetoothPrefs()
        prefs.set_fuchsia_checkout_path(args.path)
        print(f"Fuchsia checkout path set to: {args.path}")
        return 0

    config_parser.set_defaults(func=set_config)

    # Automatically register subcommands from known modules.
    if fuchsia_utils is not None:
        try:
            fuchsia_utils.register_subcommands(subparsers)
        except ImportError:
            pass

    if len(sys.argv) == 1:
        parser.print_help()
        sys.exit(0)

    # We use parse_known_args to allow subcommands to have their own argument
    # parsing if they weren't registered with our subparsers (though they should
    # be).
    args, _ = parser.parse_known_args()
    if hasattr(args, 'func'):
        sys.exit(args.func(args) or 0)

    args = parser.parse_args()


if __name__ == '__main__':
    main()
