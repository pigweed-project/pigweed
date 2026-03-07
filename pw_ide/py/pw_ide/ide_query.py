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
"""
Generates ide_query.GeneratedFile textproto from deps_mapping.json.
"""

import argparse
import json
import logging
import os
from pathlib import Path
import sys

_LOG = logging.getLogger(__name__)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--file',
        required=True,
        help='The source file to query.',
    )
    parser.add_argument(
        '--compile-commands-dir',
        required=True,
        type=Path,
        help=(
            'Directory containing the compile_commands.json '
            'and deps_mapping.json.'
        ),
    )
    return parser.parse_args()


def _relativize_path(path: str | Path, start: Path) -> Path:
    """Relativizes a path against a start directory.

    If the path is already relative, returns it as-is.
    If the path cannot be relativized (e.g. different drives on Windows),
    returns the absolute path.
    """
    path_obj = Path(path)
    if not path_obj.is_absolute():
        return path_obj

    try:
        return path_obj.resolve().relative_to(start)
    except ValueError:
        return path_obj.resolve()


def _build_unit(
    curr_id: str,
    target_info: dict,
    compile_commands_map: dict[str, list[str]],
    workspace_root: Path,
) -> str:
    """Builds a single ide_query unit textproto."""
    # Format sources
    sources = target_info.get("srcs", []) + target_info.get("hdrs", [])
    relative_sources = [
        str(_relativize_path(p, workspace_root)) for p in sources
    ]

    # Gather compiler arguments
    compiler_arguments = []
    for src in sources:
        if src in compile_commands_map:
            compiler_arguments = compile_commands_map[src]
            break

    return f"""units {{
  id: "{curr_id}"
  language: LANGUAGE_CPP
  source_file_paths: {json.dumps(relative_sources)}
  compiler_arguments: {json.dumps(compiler_arguments)}
  dependency_ids: {json.dumps(target_info.get("deps", []))}
}}"""


def main() -> int:
    """Parses arguments and runs the IDE query."""
    args = _parse_args()
    logging.basicConfig(level=logging.INFO)

    workspace_root = Path(
        os.environ.get('BUILD_WORKSPACE_DIRECTORY', Path.cwd())
    ).resolve()
    file_to_query = _relativize_path(args.file, workspace_root)

    # Load deps_mapping.json
    deps_mapping_path = args.compile_commands_dir / "deps_mapping.json"
    if not deps_mapping_path.exists():
        _LOG.error("Could not find %s", deps_mapping_path)
        return 1

    with open(deps_mapping_path, "r") as f:
        deps_mapping = json.load(f)

    compile_commands_path = args.compile_commands_dir / "compile_commands.json"
    compile_commands_map = {}
    if compile_commands_path.exists():
        with open(compile_commands_path, "r") as f:
            for cmd in json.load(f):
                compile_commands_map[cmd["file"]] = cmd["arguments"]

    files_map = deps_mapping.get("files", {})
    targets_map = deps_mapping.get("targets", {})

    # Find unit IDs for the file
    # Note: A file might belong to multiple units.
    # The ide_query proto allows choosing the "most relevant" unit.
    # For now, we pick the first one.

    # Attempt to handle relative/absolute paths
    # The keys in deps_mapping are absolute paths if resolved correctly
    # in merger.py, but let's check.

    # Preprocess files_map to absolute resolved paths for O(1) lookup
    resolved_files_map: dict[Path, list[str]] = {}
    for f, labels in files_map.items():
        f_path = Path(f)
        if not f_path.is_absolute():
            f_path = workspace_root / f_path
        resolved_files_map[f_path.resolve()] = labels

    if not file_to_query.is_absolute():
        file_to_query = workspace_root / file_to_query

    target_labels = resolved_files_map.get(file_to_query.resolve())

    if not target_labels:
        _LOG.error("File %s not found in deps_mapping.", file_to_query)
        # For now, just return a not found status.
        print(
            f"""
results {{
  source_file_path: "{_relativize_path(file_to_query, workspace_root)}"
  status {{
    code: CODE_NOT_FOUND
    status_message: "File not found in compilation database."
  }}
}}
"""
        )
        return 0

    # Pick the first target
    unit_id = target_labels[0]

    # Helper to build units recursively
    visited_units = set()
    units_to_visit = [unit_id]
    units_output = []

    while units_to_visit:
        curr_id = units_to_visit.pop(0)
        if curr_id in visited_units:
            continue
        visited_units.add(curr_id)

        target_info = targets_map.get(curr_id)
        if not target_info:
            continue

        unit_str = _build_unit(
            curr_id, target_info, compile_commands_map, workspace_root
        )
        units_output.append(unit_str)

        # Add deps to visit
        units_to_visit.extend(target_info.get("deps", []))

    # Construct final output
    out_dir = _relativize_path(args.compile_commands_dir.parent, workspace_root)
    print(
        f"""
build_out_dir: "{out_dir}"
working_dir: "{workspace_root}"
results {{
  source_file_path: "{_relativize_path(file_to_query, workspace_root)}"
  status {{
    code: CODE_OK
  }}
  unit_id: "{unit_id}"
}}
{''.join(units_output)}
"""
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
