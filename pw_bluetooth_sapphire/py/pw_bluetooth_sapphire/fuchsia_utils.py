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
"""Fuchsia-specific Bluetooth utilities."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from pw_bluetooth.preferences import BluetoothPrefs


def get_project_root() -> Path:
    if 'BUILD_WORKSPACE_DIRECTORY' in os.environ:
        return Path(os.environ['BUILD_WORKSPACE_DIRECTORY'])
    if 'PW_PROJECT_ROOT' in os.environ:
        return Path(os.environ['PW_PROJECT_ROOT'])
    if 'PW_ROOT' in os.environ:
        return Path(os.environ['PW_ROOT'])
    return Path.cwd()


def get_fuchsia_sdk_repo_name() -> str:
    try:
        result = subprocess.run(
            ['bazelisk', 'mod', 'dump_repo_mapping', ''],
            capture_output=True,
            text=True,
            check=True,
            cwd=get_project_root(),
        )
        mapping = json.loads(result.stdout)
        return mapping['fuchsia_sdk']
    except subprocess.CalledProcessError as e:
        print(
            f"Error running bazelisk mod dump_repo_mapping: {e}",
            file=sys.stderr,
        )
        print(f"Stdout: {e.stdout}", file=sys.stderr)
        print(f"Stderr: {e.stderr}", file=sys.stderr)
        raise


def get_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8192):
            h.update(chunk)
    return h.hexdigest()


def make_writable(path: Path):
    """Makes a file writable, prompting the user to run sudo if needed."""
    if os.access(path, os.W_OK):
        return

    if sys.platform == 'win32':
        raise RuntimeError("Cannot change permissions on Windows")

    try:
        # pylint: disable=import-outside-toplevel
        import pwd
        import grp

        # pylint: enable=import-outside-toplevel
    except ImportError:
        print(
            "Error: pwd and grp modules not available, "
            "cannot change permissions."
        )
        raise RuntimeError("Cannot change permissions on this platform")

    # pylint: disable=no-member
    username = pwd.getpwuid(os.getuid()).pw_name
    groupname = grp.getgrgid(os.getgid()).gr_name
    # pylint: enable=no-member

    cmds = [
        ['sudo', 'chmod', '700', str(path)],
        ['sudo', 'chown', f"{username}:{groupname}", str(path)],
    ]

    print(
        f"\n--- {path} is not writable; "
        "sudo is required to change permissions ---"
    )
    print("The following commands will be run to make it writable:")
    for cmd in cmds:
        print(f"  {' '.join(cmd)}")

    try:
        response = (
            input("Do you want to run these commands? [y/N]: ").strip().lower()
        )
    except EOFError:
        response = 'n'

    if response not in ('y', 'yes'):
        print("Permission denied by user. Aborting.")
        raise PermissionError(f"User refused to make {path} writable.")

    try:
        for cmd in cmds:
            subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Failed to change permissions for {path}: {e}")
        raise


def deploy_arch(
    arch: str, fuchsia_checkout: Path, extra_flags: list[str]
) -> bool:
    """Builds and deploys bt-host for a specific architecture."""
    print(
        f"--- Building and deploying bt-host for {arch} "
        f"to {fuchsia_checkout} ---"
    )

    target = f"//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.{arch}"

    # 1. Run cquery to find the path
    cquery_cmd = [
        'bazelisk',
        'cquery',
        '--config=fuchsia',
        *extra_flags,
        target,
        '--output=files',
    ]
    try:
        result = subprocess.run(
            cquery_cmd,
            capture_output=True,
            text=True,
            check=True,
            cwd=get_project_root(),
        )
        files = result.stdout.splitlines()
        pw_bt_host_far = None
        for f in files:
            if f.endswith('bt-host.far'):
                pw_bt_host_far = (get_project_root() / f).resolve()
                break

        if not pw_bt_host_far:
            print(
                f"Error: could not find bt-host.far in cquery output for {arch}"
            )
            return False

    except subprocess.CalledProcessError as e:
        print(f"Cquery failed: {e.stderr}")
        return False

    # 2. Build target
    build_cmd = ['bazelisk', 'build', '--config=fuchsia', *extra_flags, target]
    try:
        subprocess.run(build_cmd, check=True, cwd=get_project_root())
    except subprocess.CalledProcessError as e:
        print(f"Build failed for {target}: {e}")
        return False

    # 3. Build debug symbols
    debug_target = (
        f"//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.{arch}.debug_symbols"
    )
    build_debug_cmd = [
        'bazelisk',
        'build',
        '--config=fuchsia',
        *extra_flags,
        debug_target,
    ]
    try:
        subprocess.run(build_debug_cmd, check=True, cwd=get_project_root())
    except subprocess.CalledProcessError as e:
        print(f"Debug symbols build failed for {debug_target}: {e}")
        return False

    # 4. Deploy
    fuchsia_bt_host_far = (
        fuchsia_checkout
        / 'prebuilt'
        / 'connectivity'
        / 'bluetooth'
        / 'bt-host'
        / arch
        / 'bt-host'
    )

    if not fuchsia_bt_host_far.exists():
        print(f"Error: Target path does not exist: {fuchsia_bt_host_far}")
        return False

    try:
        make_writable(fuchsia_bt_host_far)
    except (RuntimeError, OSError):
        return False

    try:
        before_sha = get_sha256(fuchsia_bt_host_far)
        print(f"Before ({arch}): {before_sha}  {fuchsia_bt_host_far}")

        shutil.copy(pw_bt_host_far, fuchsia_bt_host_far)

        after_sha = get_sha256(fuchsia_bt_host_far)
        print(f"After ({arch}):  {after_sha}  {fuchsia_bt_host_far}")
    except OSError as e:
        print(f"Failed to copy file: {e}")
        return False

    return True


def bt_host_deploy(args: argparse.Namespace) -> int:
    """Implements the bt-host-deploy subcommand."""
    fuchsia_checkout = args.fuchsia_checkout
    if not fuchsia_checkout:
        prefs = BluetoothPrefs()
        fuchsia_checkout = prefs.fuchsia_checkout_path
        if not fuchsia_checkout:
            print(
                "Error: Fuchsia checkout path not provided "
                "and not set in config."
            )
            print("Set it with: pw bluetooth set-fuchsia-checkout <PATH>")
            return 1

    fuchsia_checkout = Path(fuchsia_checkout).resolve()

    if args.local_fuchsia_sdk:
        print(f"--- Building local Fuchsia SDK in {fuchsia_checkout} ---")
        # Try running 'fx' from path, then fallback to relative paths
        sdk_built = False
        for fx_cmd in ['fx', './tools/fx', './scripts/fx']:
            try:
                subprocess.run(
                    [fx_cmd, 'build', '//sdk:final_fuchsia_sdk'],
                    cwd=fuchsia_checkout,
                    check=True,
                )
                sdk_built = True
                break
            except (subprocess.CalledProcessError, FileNotFoundError):
                continue

        if not sdk_built:
            print("Failed to build local Fuchsia SDK")
            return 1

    try:
        fuchsia_sdk_repo_name = get_fuchsia_sdk_repo_name()
    except (subprocess.SubprocessError, json.JSONDecodeError, KeyError) as e:
        print(f"Failed to get fuchsia_sdk repo name: {e}")
        return 1

    extra_flags = []
    if args.local_fuchsia_sdk:
        extra_flags = [
            "--@fuchsia_sdk//flags:fuchsia_api_level=HEAD",
            f"--override_repository={fuchsia_sdk_repo_name}="
            f"{fuchsia_checkout}/out/default/obj/sdk/final_fuchsia_sdk",
        ]

    success = True
    for arch in ['arm64', 'x64']:
        if not deploy_arch(arch, fuchsia_checkout, extra_flags):
            print(f"Failed to deploy for {arch}")
            success = False
            break

    return 0 if success else 1


def register_subcommands(subparsers: argparse._SubParsersAction):
    """Registers Fuchsia-specific Bluetooth subcommands."""
    deploy_parser = subparsers.add_parser(
        'bt-host-deploy',
        help=(
            'Build and deploy bt-host.far to a Fuchsia checkout '
            '(arm64 and x64)'
        ),
    )
    deploy_parser.add_argument(
        '--local-fuchsia-sdk',
        action='store_true',
        help='Build the local Fuchsia SDK and use it for the build',
    )
    deploy_parser.add_argument(
        'fuchsia_checkout',
        type=Path,
        nargs='?',
        help='Path to the Fuchsia checkout (optional if set in config)',
    )
    deploy_parser.set_defaults(func=bt_host_deploy)


def main():
    """Main entry point for standalone use or as a top-level pw command."""
    parser = argparse.ArgumentParser(prog='pw bluetooth', description=__doc__)
    subparsers = parser.add_subparsers(dest='subcommand', required=True)
    register_subcommands(subparsers)

    args = parser.parse_args()
    if hasattr(args, 'func'):
        sys.exit(args.func(args))


if __name__ == '__main__':
    main()
