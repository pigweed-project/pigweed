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
"""Rust project generation and IDE configuration for pw_ide."""

from collections.abc import Callable
import contextlib
import json
import logging
import os
from pathlib import Path
import shutil
import subprocess
from typing import Any

_LOG = logging.getLogger(__name__)


@contextlib.contextmanager
def temp_remove_rust_project(workspace_root: Path, platform: str):
    """Temporarily backs up or removes root rust-project.json."""
    root_rust_project = workspace_root / "rust-project.json"
    symlink_target = None
    backup_file = None
    existed_before = False
    if root_rust_project.is_symlink():
        existed_before = True
        try:
            symlink_target = os.readlink(root_rust_project)
            root_rust_project.unlink()
        except OSError as e:
            _LOG.debug("Failed to read/unlink symlink: %s", e)
    elif root_rust_project.exists():
        existed_before = True
        backup_file = workspace_root / "rust-project.json.bak"
        if backup_file.exists():
            try:
                backup_file.unlink()
            except OSError as e:
                _LOG.debug("Failed to remove old backup: %s", e)
        try:
            root_rust_project.rename(backup_file)
        except OSError as e:
            _LOG.error("Failed to backup rust-project.json: %s", e)
    try:
        yield
    finally:
        if root_rust_project.exists() or root_rust_project.is_symlink():
            try:
                root_rust_project.unlink()
            except OSError as e:
                _LOG.debug("Failed to remove generated file: %s", e)

        if existed_before:
            if symlink_target:
                try:
                    root_rust_project.symlink_to(symlink_target)
                    _LOG.info(
                        "✅ Restored original root rust-project.json symlink"
                    )
                except OSError as e:
                    _LOG.error("Failed to restore symlink: %s", e)
            elif backup_file and backup_file.exists():
                try:
                    backup_file.rename(root_rust_project)
                    _LOG.info(
                        "✅ Restored original root rust-project.json file"
                    )
                except OSError as e:
                    _LOG.error("Failed to restore file: %s", e)
        else:
            relative_target = (
                Path(".compile_commands") / platform / "rust-project.json"
            )
            try:
                root_rust_project.symlink_to(relative_target)
                _LOG.info(
                    "✅ Symlinked root rust-project.json to %s", relative_target
                )
            except OSError as e:
                _LOG.error("Failed to symlink generated rust-project: %s", e)


def generate_ide_config(
    platform_dir: Path,
    rust_targets: list[str],
    rust_config: str | None = None,
) -> None:
    """Generates ide_config.json containing the check override command.

    Args:
        platform_dir: Path to the platform-specific output directory.
        rust_targets: List of Rust targets that are part of this platform.
        rust_config: Optional build configuration for the targets.
    """
    ide_config_path = platform_dir / "ide_config.json"

    check_args = [
        "--@rules_rust//:error_format=json",
        "--experimental_ui_max_stdouterr_bytes=10485760",
    ]

    rust_override_command = [
        "bazelisk",
        "build",
    ]
    if rust_config:
        rust_override_command.append(f"--config={rust_config}")

    rust_override_command.extend(check_args)
    rust_override_command.extend(rust_targets)

    ide_config = {"rust_analyzer_check_override_command": rust_override_command}
    with open(ide_config_path, "w") as f:
        json.dump(ide_config, f, indent=2)


def process_rust_project(
    platform: str,
    workspace_root: Path,
    platform_dir: Path,
    rust_targets: list[str],
    run_bazel_fn: Callable[..., Any],
    rust_config: str | None = None,
) -> None:
    """Generates rust-project.json for a platform.

    Args:
        platform: The platform name.
        workspace_root: Path to the workspace root.
        platform_dir: Path to the platform-specific output directory.
        rust_targets: List of Rust targets that are part of this platform.
        run_bazel_fn: Function to execute a Bazel command.
        rust_config: Optional build configuration for the targets.
    """
    _LOG.info("⏳ Generating rust-project.json...")

    rust_cmd = [
        "run",
        "@rules_rust//tools/rust_analyzer:gen_rust_project",
    ]
    if rust_config:
        rust_cmd.extend(["--", "--config", rust_config])
    else:
        rust_cmd.extend(["--"])
    rust_cmd.extend(rust_targets)

    with temp_remove_rust_project(workspace_root, platform):
        try:
            run_bazel_fn(
                rust_cmd,
                cwd=os.environ["BUILD_WORKING_DIRECTORY"],
                capture_output=True,
            )
            root_rust_project = workspace_root / "rust-project.json"
            platform_rust_project = platform_dir / "rust-project.json"
            if root_rust_project.exists():
                shutil.move(str(root_rust_project), str(platform_rust_project))
                _LOG.info("✅ Successfully generated rust-project.json")
            else:
                _LOG.error(
                    "rust-project.json was not found at workspace root "
                    "after generation"
                )
        except subprocess.CalledProcessError as e:
            _LOG.error("Failed to generate rust-project.json: %s", e)
            if hasattr(e, "stderr") and e.stderr:
                _LOG.error("Stderr: %s", e.stderr)

    generate_ide_config(
        platform_dir,
        rust_targets,
        rust_config,
    )
