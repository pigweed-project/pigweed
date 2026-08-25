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
"""Generates Kythe .kzip archives from compilation databases."""

import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

DEFAULT_KZIP_BIN = "/google/bin/releases/grok/tools/kzip"
DEFAULT_CORPUS = "pigweed.googlesource.com/pigweed/pigweed"
DEFAULT_BUCKET = "gs://pigweed-kythe/pigweed-pigweed-pigweed"


def get_git_revision(repo_dir: Path) -> str:
    """Returns the current git HEAD commit hash."""
    res = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo_dir,
        capture_output=True,
        text=True,
        check=True,
    )
    return res.stdout.strip()


def find_compilation_databases(workspace: Path) -> list[Path]:
    """Finds all compile_commands.json files in the workspace."""
    results = []
    for p in workspace.rglob("compile_commands.json"):
        # Ignore test fixture compilation databases
        if "test" not in p.parts:
            results.append(p)
    return results


def _resolve_header(
    header: str,
    src_path: Path,
    include_dirs: list[str],
    workspace: Path,
) -> Path | None:
    """Resolves an included header to an existing local file path."""
    cand = src_path.parent / header
    if cand.exists():
        return cand.resolve()
    for inc in include_dirs:
        inc_path = Path(inc)
        if not inc_path.is_absolute():
            inc_path = workspace / inc_path
        cand = inc_path / header
        if cand.exists():
            return cand.resolve()
    return None


def _find_required_headers(
    src_path: Path,
    include_dirs: list[str],
    workspace: Path,
) -> set[Path]:
    """Finds all locally resolvable headers included in the source file."""
    headers: set[Path] = set()
    try:
        content = src_path.read_text(errors="ignore")
    except Exception:  # pylint: disable=broad-except
        return headers

    for line in content.splitlines():
        line = line.strip()
        if not line.startswith("#include"):
            continue
        m = re.search(r'#include\s+["<]([^">]+)[">]', line)
        if not m:
            continue
        resolved = _resolve_header(
            m.group(1), src_path, include_dirs, workspace
        )
        if resolved:
            headers.add(resolved)
    return headers


def extract_single_command(
    entry: dict,
    idx: int,
    out_dir: Path,
    workspace: Path,
    corpus: str = DEFAULT_CORPUS,
    kzip_bin: str = DEFAULT_KZIP_BIN,
) -> Path | None:
    """Extracts a single compilation unit into a .kzip file."""
    src_file_str = entry.get("file")
    if not src_file_str:
        return None

    src_path = Path(src_file_str)
    if not src_path.is_absolute():
        src_path = (workspace / src_path).resolve()

    if not src_path.exists():
        return None

    args = []
    if "arguments" in entry:
        args = entry["arguments"]
    elif "command" in entry:
        args = shlex.split(entry["command"])

    if not args:
        return None

    # Collect include flags to locate dependent headers
    include_dirs: list[str] = []
    for i, arg in enumerate(args):
        if arg.startswith("-I") and len(arg) > 2:
            include_dirs.append(arg[2:])
        elif arg == "-I" and i + 1 < len(args):
            include_dirs.append(args[i + 1])
        elif arg.startswith("-isystem") and len(arg) > 8:
            include_dirs.append(arg[8:])
        elif arg == "-isystem" and i + 1 < len(args):
            include_dirs.append(args[i + 1])

    # Find required inputs (source file + referenced local headers)
    required_inputs: set[Path] = {src_path}
    required_inputs.update(
        _find_required_headers(src_path, include_dirs, workspace)
    )

    unit_kzip = out_dir / f"unit_{idx}.kzip"
    cmd = [
        kzip_bin,
        "create",
        f"-output={unit_kzip}",
        "-encoding=PROTO",
        f"-uri=kythe://{corpus}?lang=c%2B%2B",
        f"-working_directory={workspace}",
        f"-source_file={src_path}",
    ]

    for req in required_inputs:
        cmd.append(f"-required_input={req}")

    cmd.append("--")
    cmd.extend(args)

    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode == 0 and unit_kzip.exists():
        return unit_kzip
    return None


def generate_kzip(
    workspace: Path,
    compdb_paths: list[Path] | None = None,
    output_kzip: Path | None = None,
    corpus: str = DEFAULT_CORPUS,
    kzip_bin: str = DEFAULT_KZIP_BIN,
    max_workers: int = 16,
) -> Path:
    """Extracts compilation units from compilation databases and merges them."""
    workspace = workspace.resolve()
    if not compdb_paths:
        compdb_paths = find_compilation_databases(workspace)

    if not compdb_paths:
        raise FileNotFoundError(
            f"No compilation databases found in workspace: {workspace}"
        )

    all_entries = []
    for cdb in compdb_paths:
        with open(cdb, "r", encoding="utf-8") as f:
            entries = json.load(f)
            if isinstance(entries, list):
                all_entries.extend(entries)

    if not all_entries:
        raise ValueError("No compilation commands found to extract.")

    if not output_kzip:
        try:
            rev = get_git_revision(workspace)
        except Exception:  # pylint: disable=broad-except
            rev = "HEAD"
        output_kzip = workspace / f"{rev}.kzip"

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        unit_kzips: list[Path] = []

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=max_workers
        ) as executor:
            futures = [
                executor.submit(
                    extract_single_command,
                    entry,
                    i,
                    tmp_path,
                    workspace,
                    corpus,
                    kzip_bin,
                )
                for i, entry in enumerate(all_entries)
            ]
            for fut in concurrent.futures.as_completed(futures):
                unit_path = fut.result()
                if unit_path:
                    unit_kzips.append(unit_path)

        if not unit_kzips:
            raise RuntimeError("Failed to extract any compilation units.")

        merge_cmd = [
            kzip_bin,
            "merge",
            f"-output={output_kzip}",
            "-encoding=Proto",
            "-ignore_duplicate_cus=true",
        ] + [str(u) for u in unit_kzips]

        merge_proc = subprocess.run(merge_cmd, capture_output=True, text=True)
        if merge_proc.returncode != 0:
            raise RuntimeError(f"kzip merge failed: {merge_proc.stderr}")

    return output_kzip


def parse_args():
    """Parses command line arguments."""
    parser = argparse.ArgumentParser(
        description="Convert compile_commands.json to Kythe kzip"
    )
    parser.add_argument(
        "--compdb",
        type=Path,
        help=(
            "Path to compile_commands.json "
            "(defaults to searching .compile_commands/)"
        ),
    )
    parser.add_argument(
        "--workspace",
        type=Path,
        default=Path.cwd(),
        help="Root path of the Pigweed repository (default: current directory)",
    )
    parser.add_argument(
        "--output-kzip",
        type=Path,
        help=(
            "Output path for final merged .kzip "
            "(default: <commit_hash>.kzip in workspace)"
        ),
    )
    parser.add_argument(
        "--corpus",
        default=DEFAULT_CORPUS,
        help=f"Kythe corpus name (default: {DEFAULT_CORPUS})",
    )
    parser.add_argument(
        "--upload",
        action="store_true",
        help="Upload the merged kzip to Google Cloud Storage after generation",
    )
    parser.add_argument(
        "--gcs-bucket",
        default=DEFAULT_BUCKET,
        help=f"Target GCS destination bucket (default: {DEFAULT_BUCKET})",
    )
    parser.add_argument(
        "--max-workers",
        type=int,
        default=16,
        help="Parallel worker count for extraction",
    )
    return parser.parse_args()


def main():
    """CLI entrypoint."""
    args = parse_args()
    workspace = args.workspace.resolve()
    compdbs = [args.compdb] if args.compdb else None

    output_kzip = generate_kzip(
        workspace=workspace,
        compdb_paths=compdbs,
        output_kzip=args.output_kzip,
        corpus=args.corpus,
        max_workers=args.max_workers,
    )

    print(f"Successfully generated kzip: {output_kzip}")

    if args.upload:
        dest_url = f"{args.gcs_bucket}/{output_kzip.name}"
        print(f"Uploading to {dest_url}...")
        subprocess.run(["gsutil", "cp", str(output_kzip), dest_url], check=True)
        print(f"Upload complete: {dest_url}")


if __name__ == "__main__":
    main()
