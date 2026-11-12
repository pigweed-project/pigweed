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
"""Common utilities for the changelog scripts."""

import datetime
import json
import os
import re
import sys
import tomllib
from pathlib import Path

import tomlkit

from pw_cli import git_repo
from pw_cli import tool_runner

# Changelog-only categories layered after the canonical top-level categories in
# //docs/common/metadata/categories.toml.
EXTRA_CATEGORIES: dict[str, dict[str, str]] = {
    "rust": {
        "title": "Rust",
        "description": "Rust crates, APIs, and tooling",
    },
}

_DIFF_HEADER_RE = re.compile(r"^diff --git a/\S+ b/(\S+)$", re.MULTILINE)


def get_workspace_root() -> Path:
    """Gets the root of the Pigweed workspace.

    Returns:
        The repository root path.
    """
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace is None:
        sys.exit("[ERR] $BUILD_WORKSPACE_DIRECTORY not found")
    return Path(workspace)


def get_changelog_dir() -> Path:
    """Gets the changelog output directory."""
    return get_workspace_root() / "docs" / "sphinx" / "changelog"


def get_categories_file() -> Path:
    """Gets the path to the canonical categories TOML file."""
    return (
        get_workspace_root()
        / "docs"
        / "common"
        / "metadata"
        / "categories.toml"
    )


def get_resources_dir() -> Path:
    """Gets the resources directory path."""
    return get_workspace_root() / "docs" / "agents" / "changelog" / "resources"


def load_json(path: Path) -> dict:
    """Loads a JSON file if it exists."""
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_raw_categories_data(categories_file: Path | None = None) -> dict:
    """Loads all raw category entries from categories.toml in file order."""
    if categories_file is None:
        categories_file = get_categories_file()
    if not categories_file.exists():
        sys.exit(f"[ERR] {categories_file} not found")
    with open(categories_file, "rb") as f:
        return tomllib.load(f)


def resolve_top_level_category(
    category_id: str, raw_categories: dict | None = None
) -> str | None:
    """Resolves any category or subcategory ID to its top-level category ID."""
    if category_id in EXTRA_CATEGORIES:
        return category_id
    if raw_categories is None:
        raw_categories = load_raw_categories_data()
    if category_id not in raw_categories:
        return None
    curr = category_id
    seen = {curr}
    while (parent := raw_categories[curr].get("parent")) is not None:
        if parent not in raw_categories or parent in seen:
            return None
        seen.add(parent)
        curr = parent
    return curr


def load_categories_data(categories_file: Path | None = None) -> dict:
    """Loads valid changelog categories in canonical display order.

    Only top-level categories from //docs/common/metadata/categories.toml
    (entries without a `parent` field) are included, in their categories.toml
    display order, followed by EXTRA_CATEGORIES (such as `rust`).
    """
    raw_categories = load_raw_categories_data(categories_file)
    categories: dict[str, dict] = {}
    for category_id, info in raw_categories.items():
        if isinstance(info, dict) and info.get("parent") is None:
            categories[category_id] = dict(info)
    for category_id, info in EXTRA_CATEGORIES.items():
        categories[category_id] = dict(info)
    return categories


def load_module_categories(root: Path | None = None) -> dict[str, str]:
    """Maps each module in PIGWEED_MODULES to its top-level category ID."""
    if root is None:
        root = get_workspace_root()
    modules_file = root / "PIGWEED_MODULES"
    if not modules_file.is_file():
        return {}
    raw_categories = load_raw_categories_data(
        root / "docs" / "common" / "metadata" / "categories.toml"
    )
    module_names = modules_file.read_text(encoding="utf-8").split()
    modules_toml: dict[str, dict] = {}
    for name in module_names:
        toml_path = root / name / "module.toml"
        if toml_path.is_file():
            with open(toml_path, "rb") as f:
                modules_toml[name] = tomllib.load(f)

    resolved: dict[str, str] = {}
    for name in module_names:
        curr_mod = name
        seen_mods = {curr_mod}
        cat_id: str | None = None
        while curr_mod in modules_toml:
            mod_data = modules_toml[curr_mod]
            if isinstance(mod_data.get("category"), dict):
                cat_id = mod_data["category"].get("id")
                break
            facade = mod_data.get("facade")
            if not isinstance(facade, dict):
                break
            next_mod = facade.get("module")
            if not next_mod or next_mod in seen_mods:
                break
            seen_mods.add(next_mod)
            curr_mod = next_mod
        if cat_id is not None:
            top_cat = resolve_top_level_category(cat_id, raw_categories)
            if top_cat is not None:
                resolved[name] = top_cat
    return resolved


def suggest_commit_categories(
    paths: list[str], module_categories: dict[str, str]
) -> list[str]:
    """Suggests valid changelog category IDs for a list of modified paths."""
    suggestions: list[str] = []

    def _add(cat: str) -> None:
        if cat not in suggestions:
            suggestions.append(cat)

    for path in paths:
        parts = path.split("/")
        if (
            path.endswith(".rs")
            or "rust" in parts
            or (parts and parts[0] == "pw_rust")
        ):
            _add("rust")
            break

    for path in paths:
        top = path.split("/", 1)[0]
        if top in module_categories:
            _add(module_categories[top])
        elif top == "targets":
            _add("hw")
        elif top in ("docs", "seed"):
            _add("devtools")
        elif top == "third_party":
            _add("build")

    return suggestions


def suggest_commit_categories_from_diff(
    diff_text: str, module_categories: dict[str, str]
) -> list[str]:
    """Extracts modified paths from a git diff and suggests category IDs."""
    paths = _DIFF_HEADER_RE.findall(diff_text)
    return suggest_commit_categories(paths, module_categories)


def get_repo() -> git_repo.GitRepo:
    """Initializes and returns a GitRepo instance."""
    return git_repo.GitRepo(
        get_workspace_root(), tool_runner.BasicSubprocessRunner()
    )


def get_date_range(
    year: int, month: int
) -> tuple[datetime.datetime, datetime.datetime]:
    """Computes the start and end of the specified month."""
    start = datetime.datetime(year, month, 1)
    end_year = year + 1 if month == 12 else year
    end_month = 1 if month == 12 else month + 1
    end = datetime.datetime(end_year, end_month, 1)
    return start, end


def get_shas(args, repo: git_repo.GitRepo) -> list[str]:
    """Gets the commit SHAs for the month specified in the cli arguments."""
    start, end = get_date_range(args.year, args.month)
    shas = [
        repo.commit_hash(sha, short=False) for sha in repo.commits(start, end)
    ]
    return list(reversed(shas))


def get_data_toml_file() -> Path:
    """Gets the path to the data.toml file for the changelog."""
    return get_resources_dir() / "data.toml"


def get_data(year: int, month: int) -> dict:
    """Loads changelog TOML data for the specified month/year."""
    data_file = get_data_toml_file()
    if not data_file.exists():
        data_file.write_text(f"year = {year}\nmonth = {month}\n\n# stories\n")
    with open(data_file, "rb") as f:
        data = tomllib.load(f)
        cur_year = data.get("year")
        cur_month = data.get("month")
        if cur_year is not None and cur_month is not None:
            if cur_year != year or cur_month != month:
                msg = (
                    f"[ERR] The data file is for "
                    f"{cur_year}-{cur_month:02d}. "
                    f"But you asked for {year}-{month:02d}. "
                    "Please use the correct year/month or run 'start' "
                    "to initialize a new one."
                )
                sys.exit(msg)
        return data


def build_toml_lines(data: dict) -> str:
    """Uses tomlkit to build the data.toml file programmatically."""
    doc = tomlkit.document()
    if "year" in data:
        doc["year"] = data["year"]
    if "month" in data:
        doc["month"] = data["month"]

    stories = data.get("stories", {})
    if not stories:
        return tomlkit.dumps(doc)

    stories_table = tomlkit.table()
    for category in sorted(stories.keys()):
        cat_table = tomlkit.table()
        for story_key in sorted(stories[category].keys()):
            story = stories[category][story_key]
            s_table = tomlkit.table()

            if "score" in story:
                s_table["score"] = story["score"]
            if "score_reason" in story:
                s_table["score_reason"] = story["score_reason"]

            s_table["title"] = story["title"]
            s_table["body"] = tomlkit.string(story["body"], multiline=True)
            s_table["highlight"] = tomlkit.string(
                story["highlight"], multiline=True
            )
            s_table["example"] = tomlkit.string(
                story["example"], multiline=True
            )
            if story.get("example_proof"):
                s_table["example_proof"] = tomlkit.string(
                    story["example_proof"], multiline=True
                )

            commits = story.get("commits", {})
            if commits:
                commits_table = tomlkit.table()
                for sha in sorted(commits.keys()):
                    commit = commits[sha]
                    c_table = tomlkit.table()
                    c_table["summary"] = tomlkit.string(
                        commit["summary"], multiline=True
                    )
                    c_table["date"] = commit.get("date") or ""
                    c_table["title"] = commit.get("title") or ""
                    c_table["url"] = commit.get("url") or ""
                    commits_table[sha] = c_table
                s_table["commits"] = commits_table

            cat_table[story_key] = s_table
        stories_table[category] = cat_table

    doc["stories"] = stories_table
    return tomlkit.dumps(doc)


def update_stories_categories(data: dict, valid_categories: dict) -> None:
    """Mutates stories data to enforce valid category routing."""
    stories = data.get("stories", {})
    new_stories: dict[str, dict] = {}
    for category, category_stories in list(stories.items()):
        if category not in valid_categories:
            sys.exit(
                f"[ERR] Invalid category used: '{category}'. "
                f"Must be a defined category."
            )

        if category not in new_stories:
            new_stories[category] = {}
        new_stories[category].update(category_stories)

    data["stories"] = new_stories


def fix_data(data: dict, file_path: Path) -> None:
    """Runs data linting over toml data and overwrites the contents."""
    categories_data = load_categories_data()
    valid_categories = {
        k: v.get("title", k)
        for k, v in categories_data.items()
        if v.get("show", True)
    }
    for k, v in categories_data.items():
        if not v.get("show", True):
            valid_categories[k] = None

    update_stories_categories(data, valid_categories)

    toml_content = build_toml_lines(data)
    file_path.write_text(toml_content)


def write_json_resource(filename: str, out_data: dict) -> None:
    """Serializes output dictionaries to JSON files in the resources dir."""
    resources_dir = get_resources_dir()
    resources_dir.mkdir(parents=True, exist_ok=True)
    json_path = resources_dir / filename
    json_path.write_text(json.dumps(out_data, indent=2) + "\n")
