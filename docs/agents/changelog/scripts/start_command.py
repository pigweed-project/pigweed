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
"""Start command for the changelog scripts."""

import shutil
import sys
import tomllib

import tomlkit

import common


def check_categories() -> None:
    """Checks that //docs/common/metadata/categories.toml is valid."""
    data = common.load_categories_data()
    if not data:
        sys.exit("[ERR] No top-level categories found in categories.toml")
    for category_id, info in data.items():
        if not info.get("title"):
            sys.exit(f"[ERR] Category '{category_id}' is missing a title")


def clean_resources_dir() -> None:
    """Deletes everything in the resources dir except README.md."""
    resources_dir = common.get_resources_dir()
    if not resources_dir.exists():
        return
    for item in resources_dir.iterdir():
        if item.name != "README.md":
            if item.is_dir():
                shutil.rmtree(item)
            else:
                item.unlink()


def create_data_toml(args) -> None:
    """Creates a data.toml file in the resources directory with year/month."""
    resources_dir = common.get_resources_dir()
    data_toml = resources_dir / "data.toml"

    doc = tomlkit.document()
    doc["year"] = args.year
    doc["month"] = args.month

    with open(data_toml, "w", encoding="utf-8") as f:
        f.write(tomlkit.dumps(doc))


def start_command(args) -> None:
    """CLI entrypoint for the changelog automation `start` subcommand."""
    data_file = common.get_resources_dir() / "data.toml"
    if data_file.exists():
        with open(data_file, "rb") as f:
            try:
                data = tomllib.load(f)
                cur_year = data.get("year")
                cur_month = data.get("month")
                if cur_year is not None and cur_month is not None:
                    if cur_year != args.year or cur_month != args.month:
                        if not getattr(args, "overwrite", False):
                            msg = (
                                f"[ERR] A changelog for "
                                f"{cur_year}-{cur_month:02d} is in progress. "
                                "To start a new one for "
                                f"{args.year}-{args.month:02d}, "
                                "pass --overwrite."
                            )
                            sys.exit(msg)
            except Exception:  # pylint: disable=broad-exception-caught
                pass

    check_categories()
    clean_resources_dir()
    create_data_toml(args)
