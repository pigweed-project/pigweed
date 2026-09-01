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
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any
import zipfile

DEFAULT_KZIP_BIN = "/google/bin/releases/grok/tools/kzip"
DEFAULT_CORPUS = "pigweed.googlesource.com/pigweed/pigweed"
DEFAULT_BUCKET = "gs://pigweed-kythe/pigweed-pigweed-pigweed"

_INCLUDE_REGEX = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]')

# Caches shared across extraction workers:
# Maps resolved file path -> list of raw include header strings
_file_includes_cache: dict[Path, list[str]] = {}
# Maps (header_name, tuple(include_dirs)) -> resolved Path or None
_resolved_header_cache: dict[tuple[str, tuple[str, ...]], Path | None] = {}


def find_kzip_binary(fallback_bin: str = DEFAULT_KZIP_BIN) -> str:
    """Finds the kzip binary on the system or environment."""
    for env_var in ("PW_KYTHE_CIPD_INSTALL_DIR", "PW_PIGWEED_CIPD_INSTALL_DIR"):
        if cipd := os.environ.get(env_var):
            p = Path(cipd) / "tools" / "kzip"
            if p.exists():
                return str(p)
    if w := shutil.which("kzip"):
        return w
    if Path(fallback_bin).exists():
        return fallback_bin
    return "kzip"


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


get_git_commit = get_git_revision


def find_compilation_databases(workspace: Path) -> list[Path]:
    """Finds all compile_commands.json files in the workspace."""
    results = []
    for p in workspace.rglob("compile_commands.json"):
        # Ignore test fixture compilation databases
        if "test" not in p.parts:
            results.append(p)
    return results


def _get_direct_includes(file_path: Path) -> list[str]:
    """Extracts raw included header names from a file with in-memory cache."""
    if file_path in _file_includes_cache:
        return _file_includes_cache[file_path]
    includes: list[str] = []
    try:
        content = file_path.read_text(encoding="utf-8", errors="ignore")
        for line in content.splitlines():
            line = line.strip()
            if line.startswith("#include"):
                m = _INCLUDE_REGEX.match(line)
                if m:
                    includes.append(m.group(1))
    except Exception:  # pylint: disable=broad-except
        pass
    _file_includes_cache[file_path] = includes
    return includes


def _resolve_header(
    header: str,
    src_path: Path,
    include_dirs: list[str],
    workspace: Path,
) -> Path | None:
    """Resolves an included header to an existing local file path with cache."""
    inc_dirs_tuple = tuple(include_dirs)
    key = (header, inc_dirs_tuple)
    if key in _resolved_header_cache:
        return _resolved_header_cache[key]

    cand = src_path.parent / header
    if cand.exists() and cand.is_file():
        resolved = cand.resolve()
        _resolved_header_cache[key] = resolved
        return resolved

    for inc in include_dirs:
        inc_path = Path(inc)
        if not inc_path.is_absolute():
            inc_path = workspace / inc_path
        cand = inc_path / header
        if cand.exists() and cand.is_file():
            resolved = cand.resolve()
            _resolved_header_cache[key] = resolved
            return resolved

    _resolved_header_cache[key] = None
    return None


def _find_required_headers(
    src_path: Path,
    include_dirs: list[str],
    workspace: Path,
    visited: set[Path] | None = None,
) -> set[Path]:
    """Finds all locally resolvable headers included in the source file
    recursively.
    """
    if visited is None:
        visited = set()
    if src_path in visited:
        return set()
    visited.add(src_path)

    headers: set[Path] = set()
    for inc_name in _get_direct_includes(src_path):
        resolved = _resolve_header(inc_name, src_path, include_dirs, workspace)
        if resolved and resolved not in visited:
            headers.add(resolved)
            headers.update(
                _find_required_headers(
                    resolved, include_dirs, workspace, visited
                )
            )
    return headers


def extract_single_command(
    entry: dict,
    idx: int,
    out_dir: Path,
    workspace: Path,
    corpus: str = DEFAULT_CORPUS,
    kzip_bin: str | None = None,
) -> Path | None:
    """Extracts a single compilation unit into a .kzip file."""
    if not kzip_bin:
        kzip_bin = find_kzip_binary()

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


def _merge_kzips(
    kzip_bin: str,
    output_kzip: Path,
    unit_kzips: list[Path],
    units_dir: Path | None = None,
) -> None:
    """Merges multiple unit .kzip files into a single .kzip file."""
    if not unit_kzips:
        raise ValueError("No unit kzips provided to merge.")

    # 1. Primary method: Native recursive merge over the units directory
    if units_dir and units_dir.exists():
        merge_cmd = [
            kzip_bin,
            "merge",
            f"-output={output_kzip}",
            "-recursive",
            str(units_dir),
        ]
        merge_proc = subprocess.run(merge_cmd, capture_output=True, text=True)
        if merge_proc.returncode == 0 and output_kzip.exists():
            return

    # 2. Native merge using input_file_list if supported
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".txt", delete=False
    ) as list_file:
        for uk in unit_kzips:
            list_file.write(f"{uk}\n")
        list_file_path = list_file.name

    try:
        merge_cmd = [
            kzip_bin,
            "merge",
            f"-output={output_kzip}",
            f"-input_file_list={list_file_path}",
        ]
        merge_proc = subprocess.run(merge_cmd, capture_output=True, text=True)
        if merge_proc.returncode == 0 and output_kzip.exists():
            return

        # 3. Direct positional arguments for small batches
        merge_cmd = [
            kzip_bin,
            "merge",
            f"-output={output_kzip}",
        ] + [str(u) for u in unit_kzips]
        merge_proc = subprocess.run(merge_cmd, capture_output=True, text=True)
        if merge_proc.returncode == 0 and output_kzip.exists():
            return
    finally:
        if os.path.exists(list_file_path):
            os.remove(list_file_path)

    # 4. Fallback: Python zipfile merge if native kzip binary failed
    print("Falling back to Python zipfile merge...", flush=True)
    seen_files: set[str] = set()
    with zipfile.ZipFile(
        output_kzip, "w", compression=zipfile.ZIP_DEFLATED
    ) as out_zf:
        for uk in unit_kzips:
            try:
                with zipfile.ZipFile(uk, "r") as in_zf:
                    for item in in_zf.infolist():
                        if item.filename not in seen_files:
                            seen_files.add(item.filename)
                            out_zf.writestr(item, in_zf.read(item.filename))
            except Exception as e:  # pylint: disable=broad-except
                print(
                    f"Warning: Failed to merge unit {uk}: {e}", file=sys.stderr
                )


def _load_compilation_entries(
    workspace: Path,
    compdb_paths: list[Path] | None = None,
) -> list[dict[str, Any]]:
    """Loads and deduplicates compilation entries from compilation databases."""
    if not compdb_paths:
        compdb_paths = find_compilation_databases(workspace)

    if not compdb_paths:
        raise FileNotFoundError(
            f"No compilation databases found in workspace: {workspace}"
        )

    all_entries = []
    seen_entries = set()
    for cdb in compdb_paths:
        with open(cdb, "r", encoding="utf-8") as f:
            entries = json.load(f)
            if isinstance(entries, list):
                for entry in entries:
                    key = (
                        entry.get("file"),
                        tuple(entry.get("arguments", [])),
                        entry.get("command"),
                        entry.get("directory"),
                    )
                    if key not in seen_entries:
                        seen_entries.add(key)
                        all_entries.append(entry)

    if not all_entries:
        raise ValueError("No compilation commands found to extract.")

    return all_entries


def _extract_all_units(
    all_entries: list[dict[str, Any]],
    tmp_path: Path,
    workspace: Path,
    corpus: str,
    kzip_bin: str,
    max_workers: int,
) -> list[Path]:
    """Extracts all compilation units concurrently with progress logging."""
    total_units = len(all_entries)
    print(
        f"Extracting {total_units} compilation units with "
        f"{max_workers} workers...",
        flush=True,
    )
    start_time = time.time()
    unit_kzips: list[Path] = []

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max_workers
    ) as executor:
        futures = {
            executor.submit(
                extract_single_command,
                entry,
                i,
                tmp_path,
                workspace,
                corpus,
                kzip_bin,
            ): i
            for i, entry in enumerate(all_entries)
        }
        log_interval = max(50, total_units // 20)
        for count, fut in enumerate(
            concurrent.futures.as_completed(futures), 1
        ):
            try:
                unit_path = fut.result()
                if unit_path and unit_path.exists():
                    unit_kzips.append(unit_path)
            except Exception as e:  # pylint: disable=broad-except
                print(f"Error extracting unit: {e}", flush=True)

            if count % log_interval == 0 or count == total_units:
                elapsed = time.time() - start_time
                rate = count / elapsed if elapsed > 0 else 0
                print(
                    f"[{count}/{total_units}] Extracted {len(unit_kzips)} "
                    f"units ({elapsed:.1f}s, {rate:.1f} units/s)",
                    flush=True,
                )

    if not unit_kzips:
        raise RuntimeError("Failed to extract any compilation units.")

    extract_elapsed = time.time() - start_time
    print(f"Extraction completed in {extract_elapsed:.2f}s.", flush=True)
    return unit_kzips


def generate_kzip(
    workspace: Path,
    compdb_paths: list[Path] | None = None,
    output_kzip: Path | None = None,
    corpus: str = DEFAULT_CORPUS,
    kzip_bin: str | None = None,
    max_workers: int = 16,
) -> Path:
    """Extracts compilation units from compilation databases and merges them."""
    if not kzip_bin:
        kzip_bin = find_kzip_binary()

    workspace = workspace.resolve()
    all_entries = _load_compilation_entries(workspace, compdb_paths)

    if not output_kzip:
        try:
            rev = get_git_revision(workspace)
        except Exception:  # pylint: disable=broad-except
            rev = "HEAD"
        output_kzip = workspace / f"{rev}.kzip"
    else:
        output_kzip = output_kzip.resolve()

    start_time = time.time()
    with tempfile.TemporaryDirectory(prefix="kythe_units_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        unit_kzips = _extract_all_units(
            all_entries,
            tmp_path,
            workspace,
            corpus,
            kzip_bin,
            max_workers,
        )

        print(
            f"Merging {len(unit_kzips)} units into {output_kzip.name}...",
            flush=True,
        )
        merge_start = time.time()
        _merge_kzips(kzip_bin, output_kzip, unit_kzips, units_dir=tmp_path)
        merge_elapsed = time.time() - merge_start

        print(
            f"Merge completed in {merge_elapsed:.2f}s. "
            f"Total time: {time.time() - start_time:.2f}s.",
            flush=True,
        )

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

    print(f"Successfully generated kzip: {output_kzip}", flush=True)

    if args.upload:
        dest_url = f"{args.gcs_bucket}/{output_kzip.name}"
        print(f"Uploading to {dest_url}...", flush=True)
        subprocess.run(["gsutil", "cp", str(output_kzip), dest_url], check=True)
        print(f"Upload complete: {dest_url}", flush=True)


if __name__ == "__main__":
    main()
