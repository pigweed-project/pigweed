#!/usr/bin/env python3
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
"""Helper script to roll, verify, commit, and submit CIPD in Pigweed."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import List, Optional, Set


def get_repo_root() -> Path:
    """Find the git repository root."""
    try:
        out = (
            subprocess.check_output(
                ["git", "rev-parse", "--show-toplevel"],
                stderr=subprocess.DEVNULL,
            )
            .strip()
            .decode("utf-8")
        )
        return Path(out)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return Path(__file__).resolve().parents[4]


REPO_ROOT = get_repo_root()
WRAPPER_SCRIPT = (
    REPO_ROOT
    / "pw_env_setup"
    / "py"
    / "pw_env_setup"
    / "cipd_setup"
    / "wrapper.py"
)
VERSION_FILE = (
    REPO_ROOT
    / "pw_env_setup"
    / "py"
    / "pw_env_setup"
    / "cipd_setup"
    / ".cipd_version"
)
DIGESTS_FILE = (
    REPO_ROOT
    / "pw_env_setup"
    / "py"
    / "pw_env_setup"
    / "cipd_setup"
    / ".cipd_version.digests"
)

# Host platforms supported by Pigweed as defined in
# pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py (SUPPORTED_PLATFORMS).
SUPPORTED_PLATFORMS = (
    "aix-ppc64",
    "linux-386",
    "linux-amd64",
    "linux-arm64",
    "linux-armv6l",
    "linux-mips64",
    "linux-mips64le",
    "linux-mipsle",
    "linux-ppc64",
    "linux-ppc64le",
    "linux-s390x",
    "mac-amd64",
    "mac-arm64",
    "windows-386",
    "windows-amd64",
)

BOTTLENECK_PLATFORMS = [
    f"infra/tools/cipd/{platform}" for platform in SUPPORTED_PLATFORMS
]


def run_cipd(*args: str) -> subprocess.CompletedProcess:
    """Run CIPD via the Pigweed CIPD wrapper."""
    cmd = [sys.executable, str(WRAPPER_SCRIPT)] + list(args)
    return subprocess.run(
        cmd,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


def get_current_version() -> str:
    """Read the current version from .cipd_version."""
    if not VERSION_FILE.exists():
        return ""
    return VERSION_FILE.read_text().strip()


def get_tags_for_package(package: str) -> List[str]:
    """Get all git_revision tags for a package at latest ref in order."""
    res = run_cipd("describe", package, "-version", "latest")
    if res.returncode != 0:
        print(
            f"Warning: Failed to describe {package}:\n{res.stdout}",
            file=sys.stderr,
        )
        return []
    tags = re.findall(r"git_revision:([0-9a-fA-F]+)", res.stdout)
    return tags


def find_latest_common_revision() -> Optional[str]:
    """Find newest git_revision available on all supported platforms."""
    print("Querying CIPD packages for latest revisions across platforms...")
    common_tags: Optional[Set[str]] = None
    ordered_candidates: List[str] = []

    # Get ordered candidates from the primary platform (linux-amd64)
    primary_tags = get_tags_for_package("infra/tools/cipd/linux-amd64")
    if not primary_tags:
        print("Error: Could not retrieve tags for linux-amd64", file=sys.stderr)
        return None
    ordered_candidates = primary_tags
    common_tags = set(primary_tags)

    for platform in BOTTLENECK_PLATFORMS:
        if platform == "infra/tools/cipd/linux-amd64":
            continue
        plat_tags = set(get_tags_for_package(platform))
        if not plat_tags:
            print(f"Warning: No tags returned for {platform}", file=sys.stderr)
            continue
        common_tags &= plat_tags

    for rev in ordered_candidates:
        if rev in common_tags:
            return rev
    return None


def check_digests() -> bool:
    """Check if current digests are up-to-date."""
    print(f"Checking digests in {DIGESTS_FILE}...")
    res = run_cipd(
        "selfupdate-roll", "-version-file", str(VERSION_FILE), "-check"
    )
    print(res.stdout)
    return res.returncode == 0


def verify_cipd_functional() -> bool:
    """End-to-end test verifying rolled CIPD client bootstraps and works."""
    print("Performing end-to-end functionality check on rolled CIPD client...")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        env = os.environ.copy()
        env["CIPD_PY_INSTALL_DIR"] = str(tmp_path / "bin")

        # 1. Verify version output and bootstrap
        res_ver = subprocess.run(
            [sys.executable, str(WRAPPER_SCRIPT), "-version"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            check=False,
        )
        if res_ver.returncode != 0:
            print(
                f"Error: CIPD check failed:\n{res_ver.stdout}", file=sys.stderr
            )
            return False
        first_line = (
            res_ver.stdout.strip().splitlines()[0] if res_ver.stdout else ""
        )
        print(f"CIPD binary execution verified: {first_line}")

        # 2. Verify package installation in temporary directory
        ensure_file = tmp_path / "test.ensure"
        ensure_file.write_text("@Subdir\ninfra/tools/cipd/${platform} latest\n")
        res_ensure = subprocess.run(
            [
                sys.executable,
                str(WRAPPER_SCRIPT),
                "ensure",
                "-ensure-file",
                str(ensure_file),
                "-root",
                str(tmp_path / "pkg"),
                "-log-level",
                "warning",
            ],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            check=False,
        )
        if res_ensure.returncode != 0:
            print(
                f"Error: Ensure failed:\n{res_ensure.stdout}", file=sys.stderr
            )
            return False

    print("End-to-end functionality check PASSED!")
    return True


def roll_version(target_version: str, dry_run: bool = False) -> bool:
    """Roll CIPD to target version."""
    if not target_version.startswith("git_revision:"):
        target_tag = f"git_revision:{target_version}"
    else:
        target_tag = target_version

    current_ver = get_current_version()
    print(f"Current version: {current_ver}")
    print(f"Target version:  {target_tag}")

    if current_ver == target_tag:
        print("CIPD is already up-to-date with target version!")
        if not verify_cipd_functional():
            return False
        return True

    if dry_run:
        print("[DRY RUN] Would update .cipd_version and .cipd_version.digests.")
        return True

    print(f"Running selfupdate-roll for {target_tag}...")
    res = run_cipd(
        "selfupdate-roll",
        "-version-file",
        str(VERSION_FILE),
        "-version",
        target_tag,
    )
    print(res.stdout)
    if res.returncode != 0:
        print("Error: selfupdate-roll failed!", file=sys.stderr)
        return False

    print("Verifying updated digests...")
    if not check_digests():
        print("Error: Digest verification failed after roll.", file=sys.stderr)
        return False

    print("Running final functionality test...")
    if not verify_cipd_functional():
        print("Error: Functionality verification failed.", file=sys.stderr)
        return False

    print(f"Successfully rolled and verified CIPD to {target_tag}!")
    return True


def commit_roll(target_version: str) -> bool:
    """Stage and commit CIPD roll following Pigweed commit conventions."""
    print("Staging and committing CIPD roll...")
    files = [
        str(VERSION_FILE.relative_to(REPO_ROOT)),
        str(DIGESTS_FILE.relative_to(REPO_ROOT)),
    ]
    add_res = subprocess.run(["git", "add"] + files, cwd=REPO_ROOT, check=False)
    if add_res.returncode != 0:
        print("Error: Failed to git add CIPD files.", file=sys.stderr)
        return False

    commit_msg = (
        f"pw_env_setup: Roll cipd\n\n"
        f"Rolls CIPD client version to {target_version}.\n\n"
        f"Bug: 315378787\n"
    )
    commit_res = subprocess.run(
        ["git", "commit", "-m", commit_msg], cwd=REPO_ROOT, check=False
    )
    if commit_res.returncode != 0:
        print("Error: git commit failed.", file=sys.stderr)
        return False
    print("Commit created successfully.")
    return True


def push_roll(
    remote: str = "origin", branch: str = "main", autosubmit: bool = True
) -> bool:
    """Push commit to Gerrit."""
    ref = f"refs/for/{branch}%ready" if autosubmit else f"refs/for/{branch}"
    print(f"Submitting commit to {remote} ({ref})...")
    push_res = subprocess.run(
        ["git", "push", remote, f"HEAD:{ref}"], cwd=REPO_ROOT, check=False
    )
    if push_res.returncode != 0:
        print(
            "Error: git push failed. Check SSO status if needed.",
            file=sys.stderr,
        )
        return False
    print("Submitted successfully!")
    return True


def main() -> int:
    """CLI entry point to roll, verify, commit, and submit CIPD."""
    parser = argparse.ArgumentParser(
        description="Roll, verify, commit, and submit CIPD (b/315378787)"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Check digests and client functionality without modifying files.",
    )
    parser.add_argument(
        "--version",
        type=str,
        default=None,
        help="Specific git_revision or version to roll to.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Calculate target revision without modifying files.",
    )
    parser.add_argument(
        "--commit",
        action="store_true",
        help="Create git commit 'pw_env_setup: Roll cipd' after rolling.",
    )
    parser.add_argument(
        "--push",
        action="store_true",
        help="Submit/push the commit to Gerrit after committing.",
    )
    parser.add_argument(
        "--remote",
        type=str,
        default="origin",
        help="Git remote name to push to (default: origin).",
    )
    parser.add_argument(
        "--branch",
        type=str,
        default="main",
        help="Target branch (default: main).",
    )

    args = parser.parse_args()

    if args.check:
        if not check_digests():
            return 1
        return 0 if verify_cipd_functional() else 1

    target = args.version
    if not target:
        target = find_latest_common_revision()
        if not target:
            print(
                "Error: Could not determine latest common revision.",
                file=sys.stderr,
            )
            return 1

    if not target.startswith("git_revision:"):
        target_tag = f"git_revision:{target}"
    else:
        target_tag = target

    success = roll_version(target_tag, dry_run=args.dry_run)
    if not success:
        return 1

    if not args.dry_run and args.commit:
        if not commit_roll(target_tag):
            return 1
        if args.push:
            if not push_roll(remote=args.remote, branch=args.branch):
                return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
