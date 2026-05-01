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
import sys
import textwrap
import tomllib
from pathlib import Path

import json5
import tomlkit

from pw_cli import git_repo
from pw_cli import tool_runner


def get_workspace_root() -> Path:
    """Gets the roots of the Pigweed workspace.

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
    """Gets the path to the categories JSON file."""
    return get_resources_dir() / "categories.json5"


def get_resources_dir() -> Path:
    """Gets the resources directory path."""
    return Path(__file__).resolve().parent.parent / "resources"


def load_json(path: Path) -> dict:
    """Loads a JSON file if it exists."""
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_categories_data() -> dict:
    """Loads and returns the valid categories JSON5 data."""
    categories_file = get_categories_file()
    if not categories_file.exists():
        sys.exit(f"[ERR] {categories_file} not found")
    with open(categories_file, "r", encoding="utf-8") as f:
        return json5.load(f)


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


def wrap_text(text: str, width: int = 80) -> str:
    """Wraps text predictably across multiple lines."""
    paragraphs = text.split('\n\n')
    wrapped_paragraphs = [
        (
            '\n'.join(
                textwrap.wrap(
                    p,
                    width=width,
                    break_long_words=False,
                    break_on_hyphens=False,
                )
            )
            if p.strip()
            else ''
        )
        for p in paragraphs
    ]
    return '\n\n'.join(wrapped_paragraphs)


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

            if 'score' in story:
                s_table["score"] = story["score"]
            if 'score_reason' in story:
                s_table["score_reason"] = story["score_reason"]

            s_table["title"] = story["title"]
            s_table["body"] = tomlkit.string(
                wrap_text(story["body"].strip()), multiline=True
            )
            s_table["highlight"] = tomlkit.string(
                wrap_text(story["highlight"].strip()), multiline=True
            )

            commits = story.get("commits", {})
            if commits:
                commits_table = tomlkit.table()
                for sha in sorted(commits.keys()):
                    commit = commits[sha]
                    c_table = tomlkit.table()
                    c_table["summary"] = tomlkit.string(
                        wrap_text(commit["summary"].strip()), multiline=True
                    )
                    c_table["date"] = commit.get("date", "")
                    c_table["title"] = commit.get("title", "")
                    c_table["url"] = commit.get("url", "")
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
