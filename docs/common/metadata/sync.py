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
"""Keeps the docs in sync with categories.toml and //<module>/module.toml.

``//docs/common/metadata/categories.toml`` defines the top-level documentation
categories and ``//<module>/module.toml`` defines each module's metadata
(tagline, status, languages, size, and category or facade assignment). The
following generated content is derived from them:

* ``//docs/sphinx/platform.rst``: the categorized module index, one section
  per category.
* ``//docs/sphinx/modules.rst``: the alphabetical index of every module.
* ``//docs/sphinx/index.rst``: the category cards on the homepage.
* Category landing pages such as ``//docs/sphinx/memory/index.rst``: a table
  of the category's modules and the ``toctree`` that nests them in the sidebar.
* ``//docs/sphinx/BUILD.bazel``: the list of category landing pages.
* ``//docs/doxygen/modules.h``: ``@defgroup category_<id>`` category
  declarations and top-level ``@defgroup <module>`` declarations for every
  module with Doxygen documentation in ``index.tag``.
* ``//pw_rust/rust/pigweed/src/lib.rs``: the categorized module list on the
  rustdoc landing page, for every module with ``"Rust"`` in ``languages`` and
  generated rustdoc output.

Generated content lives between ``categories-start`` and ``categories-end``
markers, so hand-written content elsewhere in these files is preserved. A
module's ``category`` is its canonical parent in the Sphinx ``toctree``;
backend modules may also specify a ``facade`` to be listed in that facade's
``backends.rst`` page.

Usage::

   bazelisk run //docs/common/metadata:sync             # Update the docs.
   bazelisk run //docs/common/metadata:sync -- --check  # Report drift only.

The Sphinx build (``bazelisk build //docs``) runs the same checks and fails if
any generated content is out of date. See ``validate.py``.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Collection
import dataclasses
import fnmatch
import functools
import json
import os
from pathlib import Path
import posixpath
import re
import sys
import tomllib
from typing import Any
import xml.etree.ElementTree as ET

import jsonschema  # type: ignore

CONTRIBUTOR_DOCS_URL = (
    "https://pigweed.dev/contributing/docs/website.html"
    "#contrib-docs-website-categories"
)
CATEGORIES_TOML_LABEL = "//docs/common/metadata/categories.toml"
CATEGORIES_SCHEMA_LABEL = "//docs/common/metadata/categories_schema.json"
MODULE_SCHEMA_LABEL = "//docs/common/metadata/module_schema.json"
SYNC_COMMAND = "bazelisk run //docs/common/metadata:sync"

# The number of categories, in categories.toml order, that get a card on the
# homepage (//docs/sphinx/index.rst).
HOMEPAGE_CATEGORY_CARDS = 12


class SyncError(Exception):
    """A managed file cannot be updated, e.g. because its markers are gone."""


_START_NOTICE_LINES = (
    "WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!",
    "",
    "To regenerate this section, run:",
    f"  {SYNC_COMMAND}",
    "",
    "Docs: https://pigweed.dev/contributing/docs/website.html",
)
_END_NOTICE_LINES = ("END OF AUTO-GENERATED SECTION",)
_NOTICE_WIDTH = max(
    len(line) for line in (*_START_NOTICE_LINES, *_END_NOTICE_LINES)
)


def _notice(prefix: str, suffix: str = "") -> tuple[str, ...]:
    """Returns the boxed comment lines that open every generated block."""
    return _box(_START_NOTICE_LINES, prefix, suffix)


def _end_notice(prefix: str, suffix: str = "") -> tuple[str, ...]:
    """Returns the boxed comment lines that close every generated block."""
    return _box(_END_NOTICE_LINES, prefix, suffix)


def _box(
    lines: tuple[str, ...], prefix: str, suffix: str = ""
) -> tuple[str, ...]:
    """Wraps comment lines in an ASCII box."""
    border = f"{prefix}+-{'-' * _NOTICE_WIDTH}-+{suffix}"
    body = tuple(
        f"{prefix}| {line.ljust(_NOTICE_WIDTH)} |{suffix}" for line in lines
    )
    return (border, *body, border)


@dataclasses.dataclass(frozen=True)
class Markers:
    """The comment lines that delimit a generated block in some file format."""

    start: str
    end: str
    notice: tuple[str, ...]
    end_notice: tuple[str, ...]

    def split(self, text: str | None, label: str) -> tuple[str, str]:
        """Returns the text before the start marker and after the end marker.

        Raises:
            SyncError: The file doesn't exist (`text` is None) or lacks markers.
        """
        if text is None:
            raise SyncError(f"{label} is missing.")
        start_match = re.search(
            rf"^[ \t]*{re.escape(self.start)}[ \t]*$", text, re.M
        )
        end_match = (
            re.search(
                rf"^[ \t]*{re.escape(self.end)}[ \t]*$",
                text[start_match.end() :],
                re.M,
            )
            if start_match
            else None
        )
        if not start_match or not end_match:
            raise SyncError(
                f"{label} is missing its '{self.start}' and/or "
                f"'{self.end}' markers, so `{SYNC_COMMAND}` cannot "
                "update it. Add both markers where the generated content "
                f"belongs (see {CONTRIBUTOR_DOCS_URL})."
            )
        before = text[: start_match.start()]
        after = text[start_match.end() + end_match.end() :]
        return before, after

    def indent(self, text: str | None) -> str:
        """Returns leading horizontal whitespace of the start marker line."""
        if text is None:
            return ""
        match = re.search(
            rf"^(?P<indent>[ \t]*){re.escape(self.start)}[ \t]*$", text, re.M
        )
        return match.group("indent") if match else ""


RST_MARKERS = Markers(
    ".. categories-start",
    ".. categories-end",
    _notice(".. "),
    _end_notice(".. "),
)
BAZEL_MARKERS = Markers(
    "# categories-start",
    "# categories-end",
    _notice("# "),
    _end_notice("# "),
)
CPP_MARKERS = Markers(
    "// categories-start",
    "// categories-end",
    _notice("// "),
    _end_notice("// "),
)
RUST_MARKERS = Markers(
    "//! <!-- categories-start -->",
    "//! <!-- categories-end -->",
    _notice("//! <!-- ", " -->"),
    _end_notice("//! <!-- ", " -->"),
)


# ==============================================================================
# Metadata
# ==============================================================================


@dataclasses.dataclass(frozen=True)
class Category:
    """A documentation category entry from categories.toml."""

    title: str
    description: str
    homepage: str
    icon: str | None = None
    parent: str | None = None
    pin: tuple[str, ...] = ()

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> Category:
        return cls(
            title=raw["title"],
            description=raw["description"],
            homepage=raw["homepage"],
            icon=raw.get("icon"),
            parent=raw.get("parent"),
            pin=tuple(raw.get("pin", ())),
        )


@dataclasses.dataclass(frozen=True)
class ModuleCategory:
    """A module's canonical category assignment in //<module>/module.toml."""

    id: str
    toctree: str


@dataclasses.dataclass(frozen=True)
class ModuleFacade:
    """A backend module's facade assignment in //<module>/module.toml."""

    module: str
    toctree: str


@dataclasses.dataclass(frozen=True)
class Module:
    """A Pigweed module entry from //<module>/module.toml."""

    status: str
    category: ModuleCategory | None = None
    facade: ModuleFacade | None = None
    tagline: str | None = None
    languages: list[str] = dataclasses.field(default_factory=list)
    size: str | None = None

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> Module:
        return cls(
            status=raw["status"],
            category=(
                ModuleCategory(
                    id=raw["category"]["id"],
                    toctree=raw["category"]["toctree"],
                )
                if "category" in raw
                else None
            ),
            facade=(
                ModuleFacade(
                    module=raw["facade"]["module"],
                    toctree=raw["facade"]["toctree"],
                )
                if "facade" in raw
                else None
            ),
            tagline=raw.get("tagline"),
            languages=list(raw.get("languages", [])),
            size=raw.get("size"),
        )


class Metadata:
    """The contents of PIGWEED_MODULES, categories.toml, and module.toml."""

    def __init__(
        self,
        pigweed_modules: list[str],
        categories_data: dict[str, Any],
        modules_data: dict[str, dict[str, Any]],
        categories_schema: dict[str, Any],
        module_schema: dict[str, Any],
    ):
        self.pigweed_modules = pigweed_modules
        # Category ID -> Category, in the order that categories are displayed.
        self.categories: dict[str, Category] = {}
        for category_id, raw in categories_data.items():
            try:
                self.categories[category_id] = Category.from_dict(raw)
            except (KeyError, TypeError):
                pass
        # Module name -> Module.
        self.modules: dict[str, Module] = {}
        for name, raw in modules_data.items():
            try:
                self.modules[name] = Module.from_dict(raw)
            except (KeyError, TypeError):
                pass
        self._categories_data = categories_data
        self._modules_data = modules_data
        self._categories_schema = categories_schema
        self._module_schema = module_schema

    @classmethod
    def load(cls, root: Path, metadata_dir: Path | None = None) -> Metadata:
        """Loads the metadata from a Pigweed checkout or Sphinx srcdir."""
        if metadata_dir is None:
            metadata_dir = root / "docs" / "common" / "metadata"
        pigweed_modules = (
            (root / "PIGWEED_MODULES").read_text(encoding="utf-8").split()
        )
        categories_data = tomllib.loads(
            (metadata_dir / "categories.toml").read_text(encoding="utf-8")
        )
        categories_schema = json.loads(
            (metadata_dir / "categories_schema.json").read_text(
                encoding="utf-8"
            )
        )
        module_schema = json.loads(
            (metadata_dir / "module_schema.json").read_text(encoding="utf-8")
        )
        modules_data: dict[str, dict[str, Any]] = {}
        for name in pigweed_modules:
            toml_path = root / name / "module.toml"
            if toml_path.is_file():
                modules_data[name] = tomllib.loads(
                    toml_path.read_text(encoding="utf-8")
                )
        return cls(
            pigweed_modules=pigweed_modules,
            categories_data=categories_data,
            modules_data=modules_data,
            categories_schema=categories_schema,
            module_schema=module_schema,
        )

    def validate(self) -> list[str]:
        """Returns the problems with categories.toml and module.toml files."""
        try:
            jsonschema.validate(self._categories_data, self._categories_schema)
        except jsonschema.ValidationError as exc:
            location = "/".join(str(key) for key in exc.absolute_path)
            at_loc = f" at '{location}'" if location else ""
            return [
                f"{CATEGORIES_TOML_LABEL} does not match "
                f"{CATEGORIES_SCHEMA_LABEL}{at_loc}: {exc.message}"
            ]

        issues: list[str] = []
        missing = sorted(set(self.pigweed_modules) - set(self._modules_data))
        for name in missing:
            issues.append(
                f"//{name}/module.toml is missing (listed in "
                "//PIGWEED_MODULES)."
            )
        extra = sorted(set(self._modules_data) - set(self.pigweed_modules))
        for name in extra:
            issues.append(
                f"//{name}/module.toml exists, but '{name}' is not in "
                "//PIGWEED_MODULES."
            )

        schema_issues: list[str] = []
        for name, raw in sorted(self._modules_data.items()):
            try:
                jsonschema.validate(raw, self._module_schema)
            except jsonschema.ValidationError as exc:
                location = "/".join(str(key) for key in exc.absolute_path)
                at_loc = f" at '{location}'" if location else ""
                schema_issues.append(
                    f"//{name}/module.toml does not match "
                    f"{MODULE_SCHEMA_LABEL}{at_loc}: {exc.message}"
                )
        if schema_issues:
            return issues + schema_issues

        issues.extend(self._validate_modules())
        issues.extend(self._validate_categories())
        return issues

    def _validate_modules(self) -> list[str]:
        """Validates module categories and facade/backend relationships."""
        issues: list[str] = []
        for name, module in self.modules.items():
            category_id = module.category.id if module.category else None
            if category_id and category_id not in self.categories:
                issues.append(
                    f"//{name}/module.toml lists category '{category_id}', "
                    f"which is not defined in {CATEGORIES_TOML_LABEL}."
                )
            if module.facade is not None:
                facade = module.facade.module
                if facade not in self.modules:
                    issues.append(
                        f"//{name}/module.toml lists facade '{facade}', "
                        "which is not defined in //PIGWEED_MODULES."
                    )
                elif self.modules[facade].facade is not None:
                    issues.append(
                        f"//{name}/module.toml lists facade '{facade}', "
                        "which is itself a backend."
                    )
                else:
                    facade_module = self.modules[facade]
                    facade_category = (
                        facade_module.category.id
                        if facade_module.category
                        else None
                    )
                    if category_id and category_id == facade_category:
                        issues.append(
                            f"//{name}/module.toml lists category "
                            f"'{category_id}', which is already on its facade "
                            f"'{facade}'."
                        )
        return issues

    def _validate_categories(self) -> list[str]:
        """Validates category homepages, hierarchy, and pins."""
        issues: list[str] = []
        homepages: dict[str, list[str]] = {}
        for category_id, category in self.categories.items():
            if category.homepage not in homepages:
                homepages[category.homepage] = []
            homepages[category.homepage].append(category_id)
            if category.parent is not None:
                if category.parent not in self.categories:
                    issues.append(
                        f"Category '{category_id}' in {CATEGORIES_TOML_LABEL} "
                        f"lists parent '{category.parent}', which is not "
                        f"defined in {CATEGORIES_TOML_LABEL}."
                    )
                else:
                    seen = {category_id}
                    curr: str | None = category.parent
                    depth = 0
                    while curr is not None and curr in self.categories:
                        if curr in seen:
                            issues.append(
                                f"Category '{category_id}' in "
                                f"{CATEGORIES_TOML_LABEL} has a circular "
                                "parent chain."
                            )
                            break
                        seen.add(curr)
                        depth += 1
                        curr = self.categories[curr].parent
                    if depth > 4:
                        issues.append(
                            f"Category '{category_id}' in "
                            f"{CATEGORIES_TOML_LABEL} is nested too deeply "
                            "(maximum reST heading level is H6)."
                        )
            if not self.category_has_any_modules(category_id):
                issues.append(
                    f"Category '{category_id}' in {CATEGORIES_TOML_LABEL} has "
                    "no modules."
                )
            direct_modules = {
                name
                for name, module in self.modules.items()
                if module.category is not None
                and module.category.id == category_id
            }
            direct_subs = {
                sub_id
                for sub_id, sub in self.categories.items()
                if sub.parent == category_id
            }
            for pinned in category.pin:
                if pinned not in direct_modules and pinned not in direct_subs:
                    issues.append(
                        f"Category '{category_id}' in {CATEGORIES_TOML_LABEL} "
                        f"pins '{pinned}', which is not a direct module or "
                        f"subcategory of '{category_id}'."
                    )
        for homepage, category_ids in homepages.items():
            if len(category_ids) > 1:
                issues.append(
                    f"Categories {', '.join(category_ids)} in "
                    f"{CATEGORIES_TOML_LABEL} share the homepage {homepage}."
                )
        return issues

    def top_level_categories(self) -> list[str]:
        """The top-level category IDs (without a parent), in display order."""
        return [
            category_id
            for category_id, category in self.categories.items()
            if category.parent is None
        ]

    def subcategories(self, parent_id: str) -> list[str]:
        """The direct child category IDs of `parent_id`, in display order."""
        direct = [
            category_id
            for category_id, category in self.categories.items()
            if category.parent == parent_id
        ]
        pin = (
            self.categories[parent_id].pin
            if parent_id in self.categories
            else ()
        )
        pinned = [cid for cid in pin if cid in direct]
        pinned_set = set(pinned)
        return pinned + [cid for cid in direct if cid not in pinned_set]

    def category_depth(self, category_id: str) -> int:
        """The nesting depth of `category_id` (0 for top-level categories)."""
        depth = 0
        seen = {category_id}
        curr = self.categories[category_id].parent
        while curr is not None and curr in self.categories and curr not in seen:
            seen.add(curr)
            depth += 1
            curr = self.categories[curr].parent
        return depth

    def category_has_any_modules(
        self, category_id: str, _seen: set[str] | None = None
    ) -> bool:
        """Whether the category or any of its descendants has a module."""
        if self.category_modules(category_id):
            return True
        seen = (
            set(_seen) | {category_id} if _seen is not None else {category_id}
        )
        return any(
            self.category_has_any_modules(sub_id, seen)
            for sub_id in self.subcategories(category_id)
            if sub_id not in seen
        )

    def category_relpath(self, category_id: str) -> str:
        """Path of the category's landing page in the Sphinx source tree."""
        homepage = self.categories[category_id].homepage
        if homepage.startswith("//docs/sphinx/"):
            return homepage.removeprefix("//docs/sphinx/")
        return homepage.removeprefix("//")

    def category_docname(self, category_id: str) -> str:
        """Sphinx document name of the category's landing page."""
        return self.category_relpath(category_id).removesuffix(".rst")

    def category_modules(self, category_id: str) -> list[str]:
        """The modules in a category, with pinned modules first, then sorted."""
        direct = sorted(
            name
            for name, module in self.modules.items()
            if module.category is not None and module.category.id == category_id
        )
        pin = (
            self.categories[category_id].pin
            if category_id in self.categories
            else ()
        )
        pinned = [name for name in pin if name in direct]
        pinned_set = set(pinned)
        return pinned + [name for name in direct if name not in pinned_set]

    def has_category(self, module: str) -> bool:
        """Whether the module belongs to a category."""
        return self.modules[module].category is not None

    def facades(self) -> list[str]:
        """The facade modules that have at least one backend, sorted by name."""
        return sorted(
            {
                module.facade.module
                for module in self.modules.values()
                if module.facade is not None
            }
        )

    def facade_backends(self, facade: str) -> list[str]:
        """The backend modules that implement `facade`, sorted by name."""
        return sorted(
            name
            for name, module in self.modules.items()
            if module.facade is not None and module.facade.module == facade
        )


# ==============================================================================
# Sphinx
# ==============================================================================

_TOCTREE_DIRECTIVE = re.compile(r"^(?P<indent>\s*)\.\.\s+toctree::\s*$")
_TOCTREE_TARGET = re.compile(r"^(?:.+<(?P<target>[^>]+)>|(?P<doc>\S+))$")
_HEADING_CHARS = {1: "=", 2: "-", 3: "=", 4: "-", 5: "^", 6: "."}


def _toctree_docnames_in_text(text: str, parent_dir: str) -> set[str]:
    """Returns docnames or globs in `.. toctree::` directives in `text`."""
    docnames: set[str] = set()
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        match = _TOCTREE_DIRECTIVE.match(lines[i])
        if not match:
            i += 1
            continue
        base_indent = len(match.group("indent"))
        i += 1
        while i < len(lines):
            line = lines[i]
            stripped = line.strip()
            if not stripped:
                i += 1
                continue
            if len(line) - len(line.lstrip()) <= base_indent:
                break
            i += 1
            if stripped.startswith((":", "..")):
                continue
            if target_match := _TOCTREE_TARGET.match(stripped):
                target = target_match.group("target") or target_match.group(
                    "doc"
                )
                if "://" in target or target == "self":
                    continue
                joined = (
                    target.lstrip("/")
                    if target.startswith("/")
                    else posixpath.join(parent_dir, target)
                )
                docnames.add(posixpath.normpath(joined).removesuffix(".rst"))
    return docnames


def find_external_toctree_docnames(sphinx_dir: Path) -> set[str]:
    """Returns docnames that appear in a hand-written toctree in sphinx_dir.

    //docs/sphinx/index.rst and parent category landing pages link to these
    category pages with pw:// instead of listing them directly in their
    toctrees so that every page has a single toctree parent.
    """
    generated_block = re.compile(
        rf"^{re.escape(RST_MARKERS.start)}$.*?^{re.escape(RST_MARKERS.end)}$",
        re.M | re.S,
    )
    docnames: set[str] = set()
    for path in sorted(sphinx_dir.rglob("*.rst")):
        relpath = path.relative_to(sphinx_dir).as_posix()
        text = generated_block.sub("", path.read_text(encoding="utf-8"))
        parent_dir = posixpath.dirname(relpath)
        docnames |= _toctree_docnames_in_text(text, parent_dir)
    return docnames


def _heading(title: str, level: int) -> list[str]:
    """Renders a reST section title at the given H1-H6 heading level."""
    adornment = _HEADING_CHARS[level] * len(title)
    if level <= 2:
        return [adornment, title, adornment]
    return [title, adornment]


def _section_title(title: str, char: str) -> list[str]:
    """Renders a reST section title with matching over- and underlines."""
    adornment = char * len(title)
    return [adornment, title, adornment]


def _status_badge(status: str) -> str:
    """Renders a module status as a Sphinx Design badge."""
    badge = {
        "stable": "primary",
        "unstable": "secondary",
        "experimental": "warning",
        "deprecated": "danger",
    }[status]
    return f":bdg-{badge}:`{status}`"


def _csv_table(meta: Metadata, modules: list[str]) -> list[str]:
    """Renders a table of modules with their status, tagline, and languages."""
    lines = [
        ".. csv-table::",
        '   :header: "Name", "Status", "Description", "Languages"',
        "",
    ]
    for name in modules:
        module = meta.modules[name]
        cells = (
            f":ref:`module-{name}`",
            _status_badge(module.status),
            module.tagline or "",
            ", ".join(module.languages),
        )
        lines.append(
            "   "
            + ", ".join('"' + cell.replace('"', '""') + '"' for cell in cells)
        )
    return lines


def _category_section_lines(
    meta: Metadata,
    category_id: str,
    level: int,
    *,
    leading_slash: bool = False,
) -> list[str]:
    """Renders a category section and its nested subcategory sections."""
    category = meta.categories[category_id]
    doc_link = (
        f"/{meta.category_docname(category_id)}"
        if leading_slash
        else meta.category_docname(category_id)
    )
    lines = [
        *_heading(category.title, level),
        category.description,
        "",
        f"Learn more: :doc:`{doc_link}`",
    ]
    modules = meta.category_modules(category_id)
    if modules:
        lines += ["", *_csv_table(meta, modules)]
    for sub_id in meta.subcategories(category_id):
        lines += [
            "",
            *_category_section_lines(
                meta, sub_id, level + 1, leading_slash=leading_slash
            ),
        ]
    return lines


def _category_block(
    meta: Metadata,
    category_id: str,
    *,
    include_self: bool = True,
    local_toctree_docnames: Collection[str] = (),
    external_toctree_docnames: Collection[str] = (),
) -> str:
    """Renders the generated block of a category landing page."""
    modules = meta.category_modules(category_id)
    category_title = meta.categories[category_id].title
    relpath = meta.category_relpath(category_id)
    parent_dir = posixpath.dirname(relpath)
    parent_dirs = "../" * len(Path(relpath).parent.parts)

    lines = [
        RST_MARKERS.start,
        *RST_MARKERS.notice,
    ]
    if modules:
        lines += [
            "",
            *_heading("Modules", 2),
            f"The following table lists all modules related to "
            f"{category_title}.",
            "",
            *_csv_table(meta, modules),
        ]
    for sub_id in meta.subcategories(category_id):
        lines += [
            "",
            *_category_section_lines(meta, sub_id, 2, leading_slash=True),
        ]

    toctree_entries: list[str] = []
    if include_self:
        toctree_entries.append("   self")

    module_toctree: dict[str, str] = {}
    for name in modules:
        category = meta.modules[name].category
        if category is None:
            continue
        target = f"{parent_dirs}{name}/docs"
        module_toctree[name] = f"   {category.toctree} <{target}>"

    sub_toctree: dict[str, str] = {}
    for sub_id in meta.subcategories(category_id):
        sub_docname = meta.category_docname(sub_id)
        if any(
            fnmatch.fnmatchcase(sub_docname, pat)
            for pat in local_toctree_docnames
        ):
            continue
        sub_title = meta.categories[sub_id].title
        if any(
            fnmatch.fnmatchcase(sub_docname, pat)
            for pat in external_toctree_docnames
        ):
            sub_toctree[sub_id] = f"   {sub_title} <pw://{sub_docname}.html>"
        else:
            rel_target = (
                posixpath.relpath(sub_docname, parent_dir)
                if parent_dir
                else sub_docname
            )
            sub_toctree[sub_id] = f"   {sub_title} <{rel_target}>"

    pin = meta.categories[category_id].pin
    pinned_set = set(pin)
    for item in pin:
        if item in module_toctree:
            toctree_entries.append(module_toctree[item])
        elif item in sub_toctree:
            toctree_entries.append(sub_toctree[item])
    for name in modules:
        if name not in pinned_set and name in module_toctree:
            toctree_entries.append(module_toctree[name])
    for sub_id in meta.subcategories(category_id):
        if sub_id not in pinned_set and sub_id in sub_toctree:
            toctree_entries.append(sub_toctree[sub_id])

    if toctree_entries:
        lines += [
            "",
            ".. Do not edit this toctree. To add non-module docs to this"
            " category,",
            ".. create a separate toctree outside the categories-start /",
            ".. categories-end block.",
            ".. toctree::",
            "   :maxdepth: 1",
            "   :hidden:",
            "",
            *toctree_entries,
        ]
    lines += [
        "",
        *RST_MARKERS.end_notice,
        RST_MARKERS.end,
    ]
    return "\n".join(lines)


def render_category_page(
    meta: Metadata,
    category_id: str,
    existing: str | None,
    *,
    external_toctree_docnames: Collection[str] = (),
) -> str:
    """Renders a category landing page.

    Hand-written content outside the markers is preserved; missing pages are
    scaffolded with a label, an H1 title, and the generated block.
    """
    if existing is None:
        block = _category_block(
            meta,
            category_id,
            include_self=True,
            external_toctree_docnames=external_toctree_docnames,
        )
        title = _section_title(meta.categories[category_id].title, "=")
        return (
            f".. _docs-category-{category_id}:\n\n"
            + "\n".join(title)
            + f"\n{block}\n"
        )
    label = meta.categories[category_id].homepage
    before, after = RST_MARKERS.split(existing, label)
    parent_dir = posixpath.dirname(meta.category_relpath(category_id))
    local_toctree_docnames = _toctree_docnames_in_text(
        f"{before}\n{after}", parent_dir
    )
    include_self = not any(
        _TOCTREE_DIRECTIVE.match(line) for line in before.splitlines()
    )
    block = _category_block(
        meta,
        category_id,
        include_self=include_self,
        local_toctree_docnames=local_toctree_docnames,
        external_toctree_docnames=external_toctree_docnames,
    )
    return (before + block + after).rstrip("\n") + "\n"


def _backends_block(meta: Metadata, facade: str) -> str:
    """Renders the generated block of a facade's backends.rst page."""
    backends = meta.facade_backends(facade)
    lines = [
        RST_MARKERS.start,
        *RST_MARKERS.notice,
        "",
        *_csv_table(meta, backends),
        "",
        ".. Do not edit this toctree. To add non-module docs to this page,",
        ".. create a separate toctree outside the categories-start /",
        ".. categories-end block.",
        ".. toctree::",
        "   :maxdepth: 1",
        "   :hidden:",
        "",
    ]
    for name in backends:
        facade_info = meta.modules[name].facade
        if facade_info is None:
            continue
        title = facade_info.toctree
        if not meta.has_category(name):
            lines.append(f"   {title} <../{name}/docs>")
        else:
            lines.append(f"   {title} <pw://{name}/docs.html>")
    lines += ["", *RST_MARKERS.end_notice, RST_MARKERS.end]
    return "\n".join(lines)


def render_backends_page(
    meta: Metadata, facade: str, existing: str | None
) -> str:
    """Renders //<facade>/backends.rst for a facade module."""
    block = _backends_block(meta, facade)
    if existing is None:
        title = _section_title("Backends", "=")
        return (
            f".. _module-{facade}-backends:\n\n"
            + "\n".join(title)
            + f"\n.. pigweed-module-subpage::\n   :name: {facade}\n\n{block}\n"
        )
    label = f"//{facade}/backends.rst"
    before, after = RST_MARKERS.split(existing, label)
    return (before + block + after).rstrip("\n") + "\n"


def _category_cards(meta: Metadata, limit: int | None = None) -> list[str]:
    """Renders 3-column Sphinx Design grids of top-level category cards."""
    cards = [
        (category_id, meta.categories[category_id])
        for category_id in meta.top_level_categories()
    ]
    if limit is not None:
        cards = cards[:limit]
    lines: list[str] = []
    for row in range(0, len(cards), 3):
        lines += [".. grid:: 3", ""]
        for category_id, category in cards[row : row + 3]:
            lines += [
                f"   .. grid-item-card:: :octicon:`{category.icon}` "
                f"{category.title}",
                f"      :link: {meta.category_docname(category_id)}",
                "      :link-type: doc",
                "      :class-item: sales-pitch-cta-secondary",
                "",
                f"      {category.description}",
                "",
            ]
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def render_modules_rst(meta: Metadata, existing: str | None) -> str:
    """Renders //docs/sphinx/modules.rst, the alphabetical module index."""
    before, after = RST_MARKERS.split(existing, "//docs/sphinx/modules.rst")
    lines = [
        RST_MARKERS.start,
        *RST_MARKERS.notice,
        "",
        *_csv_table(meta, sorted(meta.modules)),
        "",
        *RST_MARKERS.end_notice,
        RST_MARKERS.end,
    ]
    block = "\n".join(lines)
    return (before + block + after).rstrip("\n") + "\n"


def render_platform_rst(
    meta: Metadata,
    existing: str | None,
    external_toctree_docnames: Collection[str] = (),
) -> str:
    """Renders //docs/sphinx/platform.rst, the index of every module."""
    before, after = RST_MARKERS.split(existing, "//docs/sphinx/platform.rst")
    lines = [
        RST_MARKERS.start,
        *RST_MARKERS.notice,
        "",
        *_section_title("Overview", "-"),
        *_category_cards(meta),
    ]
    for category_id in meta.top_level_categories():
        lines += [
            "",
            *_category_section_lines(meta, category_id, 2),
        ]
    lines += [
        "",
        ".. Do not edit this toctree. To add non-category pages to this",
        ".. page's toctree, add them to the hand-written toctree above the",
        ".. categories-start block.",
        ".. toctree::",
        "   :maxdepth: 1",
        "   :hidden:",
        "",
    ]
    for category_id in meta.top_level_categories():
        category = meta.categories[category_id]
        docname = meta.category_docname(category_id)
        if any(
            fnmatch.fnmatchcase(docname, pat)
            for pat in external_toctree_docnames
        ):
            lines.append(f"   {category.title} <pw://{docname}.html>")
        else:
            lines.append(f"   {docname}")
    lines += ["", *RST_MARKERS.end_notice, RST_MARKERS.end]
    block = "\n".join(lines)
    return (before + block + after).rstrip("\n") + "\n"


def render_index_rst(
    meta: Metadata,
    existing: str | None,
) -> str:
    """Renders the category cards on //docs/sphinx/index.rst."""
    before, after = RST_MARKERS.split(existing, "//docs/sphinx/index.rst")
    lines = [
        RST_MARKERS.start,
        *RST_MARKERS.notice,
        "",
        *_category_cards(meta, limit=HOMEPAGE_CATEGORY_CARDS),
        "",
        "And more! See :ref:`docs-module-guides`.",
        "",
        *RST_MARKERS.end_notice,
        RST_MARKERS.end,
    ]
    block = "\n".join(lines)
    return before.rstrip() + "\n\n" + block + "\n\n" + after.lstrip()


def render_sphinx_build_bazel(meta: Metadata, existing: str | None) -> str:
    """Renders the category landing page list in //docs/sphinx/BUILD.bazel."""
    before, after = BAZEL_MARKERS.split(existing, "//docs/sphinx/BUILD.bazel")
    indent = BAZEL_MARKERS.indent(existing)
    relpaths: set[str] = set()
    for category_id, category in meta.categories.items():
        if category.homepage.startswith("//docs/sphinx/"):
            relpaths.add(meta.category_relpath(category_id))
    lines = [
        f"{indent}{BAZEL_MARKERS.start}",
        *(f"{indent}{line}" for line in BAZEL_MARKERS.notice),
        *(f'{indent}"{relpath}",' for relpath in sorted(relpaths)),
        *(f"{indent}{line}" for line in BAZEL_MARKERS.end_notice),
        f"{indent}{BAZEL_MARKERS.end}",
    ]
    return (
        before.rstrip("\n")
        + "\n"
        + "\n".join(lines)
        + "\n"
        + after.lstrip("\n")
    )


def find_stale_category_pages(sphinx_dir: Path, meta: Metadata) -> list[Path]:
    """Finds pages with a generated block that are no longer category pages."""
    active = {
        meta.category_relpath(category_id) for category_id in meta.categories
    } | {f"{facade}/backends.rst" for facade in meta.facades()}
    start_marker = re.compile(rf"^{re.escape(RST_MARKERS.start)}\s*$", re.M)
    stale: list[Path] = []
    for path in sorted(sphinx_dir.rglob("*.rst")):
        relpath = path.relative_to(sphinx_dir).as_posix()
        if relpath in active or relpath in (
            "index.rst",
            "modules.rst",
            "platform.rst",
        ):
            continue
        if start_marker.search(path.read_text(encoding="utf-8")):
            stale.append(path)
    return stale


# ==============================================================================
# Doxygen
# ==============================================================================

_DEFGROUP = re.compile(r"^/// @defgroup (\S+)")
_INGROUP = re.compile(r"^/// @ingroup (\S+)")
# Child tags of `<compound kind="group">` in Doxygen's `index.tag` that describe
# the group's own declaration and child subgroups rather than documented C/C++
# entities (`<class>`, `<member>`, `<namespace>`, etc.).
_GROUP_METADATA_TAGS = frozenset({"name", "title", "filename", "subgroup"})


def _documented_doxygen_modules(
    meta: Metadata, tagfile: Path, manual_groups: str
) -> set[str]:
    """Returns the modules that have documented C/C++ entities in `tagfile`."""
    if not tagfile.is_file():
        raise SyncError(
            f"Doxygen tagfile {tagfile} is missing; run `{SYNC_COMMAND}`."
        )

    parent: dict[str, str] = {}
    current: str | None = None
    for line in manual_groups.splitlines():
        if match := _DEFGROUP.match(line):
            current = match.group(1)
        elif (match := _INGROUP.match(line)) and current is not None:
            parent[current] = match.group(1)

    groups = [
        c
        for c in ET.parse(tagfile).getroot().findall("compound")
        if c.get("kind") == "group"
    ]
    for compound in groups:
        if name := compound.findtext("name"):
            for sub in compound.findall("subgroup"):
                if sub.text and sub.text not in parent:
                    parent[sub.text] = name

    documented: set[str] = set()
    for compound in groups:
        if not ({el.tag for el in compound} - _GROUP_METADATA_TAGS):
            continue
        group = compound.findtext("name") or ""
        seen: set[str] = set()
        while (
            group in parent and group not in meta.modules and group not in seen
        ):
            seen.add(group)
            group = parent[group]
        if group in meta.modules:
            documented.add(group)
    return documented


def render_doxygen_modules_h(
    meta: Metadata, tagfile: Path, existing: str | None
) -> str:
    """Renders the category and module groups in //docs/doxygen/modules.h.

    Declares a `category_<id>` group for every category that contains a
    documented module (or has a descendant category that does) and a `<module>`
    group for every module with Doxygen documentation in `tagfile` (plus any
    facade whose backend is documented).
    """
    before, after = CPP_MARKERS.split(existing, "//docs/doxygen/modules.h")
    documented = _documented_doxygen_modules(meta, tagfile, after)
    modules = sorted(
        documented
        | {
            facade.module
            for m in documented
            if (facade := meta.modules[m].facade) is not None
        }
    )
    active_categories_set: set[str] = set()
    for module in modules:
        category = meta.modules[module].category
        curr = category.id if category is not None else None
        while curr is not None and curr in meta.categories:
            active_categories_set.add(curr)
            curr = meta.categories[curr].parent
    active_categories = sorted(
        active_categories_set, key=lambda cid: (meta.category_depth(cid), cid)
    )

    header = [CPP_MARKERS.start, *CPP_MARKERS.notice]
    for category_id in active_categories:
        cat_info = meta.categories[category_id]
        header.append(f"/// @defgroup category_{category_id} {cat_info.title}")
        if cat_info.parent is not None:
            header.append(f"/// @ingroup category_{cat_info.parent}")
        header.append(f"/// @brief {cat_info.description}")
    for name in modules:
        mod_info = meta.modules[name]
        parents = []
        if mod_info.facade is not None:
            parents.append(mod_info.facade.module)
        if mod_info.category is not None:
            parents.append(f"category_{mod_info.category.id}")
        ingroup = " ".join(parents)
        header += [
            f"/// @defgroup {name} {name}",
            f"/// @ingroup {ingroup}",
        ]
        if mod_info.tagline:
            header.append(f"/// @brief {mod_info.tagline}")
    header += [*CPP_MARKERS.end_notice, CPP_MARKERS.end]

    return (
        before.rstrip()
        + "\n\n"
        + "\n".join(header)
        + "\n\n"
        + after.strip("\n")
        + "\n"
    )


# ==============================================================================
# Rustdoc
# ==============================================================================


def _rustdoc_category_lines(
    meta: Metadata,
    category_id: str,
    modules_by_category: dict[str, list[str]],
    depth: int = 0,
) -> list[str]:
    """Renders a category section and nested subcategory sections in lib.rs."""
    direct = modules_by_category.get(category_id, [])
    child_blocks: list[str] = []
    for sub_id in meta.subcategories(category_id):
        child_blocks.extend(
            _rustdoc_category_lines(
                meta, sub_id, modules_by_category, depth + 1
            )
        )
    if not direct and not child_blocks:
        return []
    category = meta.categories[category_id]
    hashes = "#" * (3 + depth)
    lines = [
        "//!",
        f"//! {hashes} {category.title}",
        "//!",
        f"//! {category.description.rstrip('.')}.",
    ]
    if direct:
        lines += ["//!", *direct]
    lines.extend(child_blocks)
    return lines


def render_rustdoc_lib_rs(
    meta: Metadata, rustdoc_dir: Path, existing: str | None
) -> str:
    """Renders the module list in //pw_rust/rust/pigweed/src/lib.rs."""
    if not rustdoc_dir.is_dir():
        raise SyncError(
            f"Rustdoc directory {rustdoc_dir} is missing; run `{SYNC_COMMAND}`."
        )
    before, after = RUST_MARKERS.split(
        existing, "//pw_rust/rust/pigweed/src/lib.rs"
    )
    modules_by_category: dict[str, list[str]] = {}
    for category_id in meta.categories:
        for name in meta.category_modules(category_id):
            module = meta.modules[name]
            if "Rust" not in module.languages:
                continue
            if not (rustdoc_dir / name / "index.html").is_file():
                continue
            entry = f"//! - [`{name}`](../{name}/index.html)"
            if module.tagline:
                entry += f" - {module.tagline.rstrip('.')}."
            modules_by_category.setdefault(category_id, []).append(entry)

    lines = [
        RUST_MARKERS.start,
        *RUST_MARKERS.notice,
        "//! The following Pigweed modules are available in Rust, organized "
        "by category:",
    ]
    for category_id in meta.top_level_categories():
        lines.extend(
            _rustdoc_category_lines(meta, category_id, modules_by_category)
        )
    lines += [*RUST_MARKERS.end_notice, RUST_MARKERS.end]
    return before.rstrip() + "\n" + "\n".join(lines) + "\n" + after.lstrip()


# ==============================================================================
# Sync and check
# ==============================================================================


@dataclasses.dataclass(frozen=True)
class DocsTree:
    """The locations of the files that this tool manages.

    This tool runs against two different directory layouts:
    1. A Git repository checkout (`for_workspace`, used by
       `bazelisk run //docs/common/metadata:sync`), where the main Sphinx site
       sources live under `<root>/docs/sphinx/`, module and target docs live in
       their own top-level directories (`<root>/pw_*/`, `<root>/targets/`), and
       built Doxygen/rustdoc outputs live in Bazel runfiles or `bazel-bin/`.
    2. The hermetic Sphinx source tree assembled by Bazel (`for_sphinx_srcdir`,
       used by `validate.py` during `bazelisk build //docs`), where
       `sphinx_docs` in `//docs/sphinx/BUILD.bazel` sets
       `strip_prefix = "docs/sphinx/"` so the contents of `//docs/sphinx/` sit
       directly at `<srcdir>/`, files from other `sphinx_docs_library` targets
       sit at `<srcdir>/<repo-relpath>`, Doxygen and rustdoc outputs are mounted
       at `<srcdir>/doxygen/api/cc/` and `<srcdir>/rustdoc/rustdoc/`, and
       `BUILD.bazel` files are not copied into `<srcdir>/` at all.
    """

    sphinx_dir: Path
    doxygen_modules_h: Path
    doxygen_tagfile: Path
    rust_lib_rs: Path
    rustdoc_dir: Path
    # Not present in the Sphinx source tree that Bazel assembles.
    sphinx_build_bazel: Path | None = None
    workspace_root: Path | None = None

    @classmethod
    def for_workspace(cls, root: Path) -> DocsTree:
        """The files in a Pigweed checkout."""
        runfiles_tag = Path("docs/doxygen/html/index.tag").resolve()
        doxygen_tagfile = (
            runfiles_tag
            if runfiles_tag.is_file()
            else root / "bazel-bin/docs/doxygen/html/index.tag"
        )
        runfiles_rustdoc = Path("pw_rust/docs.rustdoc").resolve()
        rustdoc_dir = (
            runfiles_rustdoc
            if runfiles_rustdoc.is_dir()
            else root / "bazel-bin/pw_rust/docs.rustdoc"
        )
        return cls(
            sphinx_dir=root / "docs/sphinx",
            doxygen_modules_h=root / "docs/doxygen/modules.h",
            doxygen_tagfile=doxygen_tagfile,
            rust_lib_rs=root / "pw_rust/rust/pigweed/src/lib.rs",
            rustdoc_dir=rustdoc_dir,
            sphinx_build_bazel=root / "docs/sphinx/BUILD.bazel",
            workspace_root=root,
        )

    @classmethod
    def for_sphinx_srcdir(cls, srcdir: Path) -> DocsTree:
        """The files in the Sphinx source tree assembled by `bazelisk build`.

        The tree has the contents of //docs/sphinx/ at its root, files from
        elsewhere in the checkout at their usual paths, and no BUILD files.
        """
        return cls(
            sphinx_dir=srcdir,
            doxygen_modules_h=srcdir / "docs/doxygen/modules.h",
            doxygen_tagfile=srcdir / "doxygen/api/cc/index.tag",
            rust_lib_rs=srcdir / "pw_rust/rust/pigweed/src/lib.rs",
            rustdoc_dir=srcdir / "rustdoc/rustdoc",
            workspace_root=srcdir,
        )

    def _root(self) -> Path:
        return (
            self.workspace_root
            if self.workspace_root is not None
            else self.sphinx_dir.parent.parent
        )

    def category_page_path(self, meta: Metadata, category_id: str) -> Path:
        """The path on disk of a category's landing page."""
        homepage = meta.categories[category_id].homepage
        relpath = meta.category_relpath(category_id)
        if homepage.startswith("//docs/sphinx/"):
            return self.sphinx_dir / relpath
        return self._root() / relpath

    def module_dir(self, module: str) -> Path:
        """The directory of a Pigweed module."""
        return self._root() / module

    def label(self, path: Path) -> str:
        """The //-prefixed path of a file under sphinx_dir, for messages."""
        return f"//docs/sphinx/{path.relative_to(self.sphinx_dir).as_posix()}"


@dataclasses.dataclass(frozen=True)
class ManagedFile:
    """A file generated from categories.toml and module.toml."""

    path: Path
    # The //-prefixed path, for messages.
    label: str
    # Returns the expected content of the file given its current content, or
    # None if it doesn't exist. Files that are generated in full are created
    # from scratch; files that merely contain a generated block raise SyncError.
    render: Callable[[str | None], str]


def managed_files(tree: DocsTree, meta: Metadata) -> list[ManagedFile]:
    """Lists every file generated from categories.toml and module.toml."""
    external_toctrees = find_external_toctree_docnames(tree.sphinx_dir)
    files = [
        ManagedFile(
            tree.sphinx_dir / "platform.rst",
            "//docs/sphinx/platform.rst",
            render=functools.partial(
                render_platform_rst,
                meta,
                external_toctree_docnames=external_toctrees,
            ),
        ),
        ManagedFile(
            tree.sphinx_dir / "modules.rst",
            "//docs/sphinx/modules.rst",
            render=functools.partial(render_modules_rst, meta),
        ),
    ]
    for category_id in sorted(meta.categories):
        files.append(
            ManagedFile(
                tree.category_page_path(meta, category_id),
                meta.categories[category_id].homepage,
                render=functools.partial(
                    render_category_page,
                    meta,
                    category_id,
                    external_toctree_docnames=external_toctrees,
                ),
            )
        )
    for facade in meta.facades():
        files.append(
            ManagedFile(
                tree.module_dir(facade) / "backends.rst",
                f"//{facade}/backends.rst",
                render=functools.partial(render_backends_page, meta, facade),
            )
        )
    files.append(
        ManagedFile(
            tree.sphinx_dir / "index.rst",
            "//docs/sphinx/index.rst",
            render=functools.partial(
                render_index_rst,
                meta,
            ),
        )
    )
    if tree.sphinx_build_bazel is not None:
        files.append(
            ManagedFile(
                tree.sphinx_build_bazel,
                "//docs/sphinx/BUILD.bazel",
                render=functools.partial(render_sphinx_build_bazel, meta),
            )
        )
    files += [
        ManagedFile(
            tree.doxygen_modules_h,
            "//docs/doxygen/modules.h",
            render=functools.partial(
                render_doxygen_modules_h, meta, tree.doxygen_tagfile
            ),
        ),
        ManagedFile(
            tree.rust_lib_rs,
            "//pw_rust/rust/pigweed/src/lib.rs",
            render=functools.partial(
                render_rustdoc_lib_rs, meta, tree.rustdoc_dir
            ),
        ),
    ]
    return files


def _read(path: Path) -> str | None:
    return path.read_text(encoding="utf-8") if path.is_file() else None


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _stale_category_page_issue(tree: DocsTree, path: Path) -> str:
    return (
        f"{tree.label(path)} contains a '{RST_MARKERS.start}' block, "
        f"but is no longer a category homepage in {CATEGORIES_TOML_LABEL}. "
        "Delete the file or remove the generated block."
    )


def check(tree: DocsTree, meta: Metadata) -> list[str]:
    """Describes every managed file that is out of date, if any."""
    issues: list[str] = []
    for file in managed_files(tree, meta):
        existing = _read(file.path)
        try:
            expected = file.render(existing)
        except SyncError as exc:
            issues.append(str(exc))
            continue
        if existing is None:
            issues.append(f"{file.label} is missing.")
        elif expected != existing:
            issues.append(
                f"{file.label} is out of sync with {CATEGORIES_TOML_LABEL} and "
                "//<module>/module.toml."
            )

    for path in find_stale_category_pages(tree.sphinx_dir, meta):
        issues.append(_stale_category_page_issue(tree, path))
    return issues


def sync(tree: DocsTree, meta: Metadata) -> list[Path]:
    """Brings every managed file up to date. Returns the paths that changed.

    Raises:
        SyncError: A file is missing, its markers are gone, or a stale category
            page still contains a generated block.
    """
    stale = find_stale_category_pages(tree.sphinx_dir, meta)
    if stale:
        raise SyncError(
            "\n".join(_stale_category_page_issue(tree, p) for p in stale)
        )

    changed: list[Path] = []
    for file in managed_files(tree, meta):
        existing = _read(file.path)
        expected = file.render(existing)
        if expected != existing:
            _write(file.path, expected)
            changed.append(file.path)
    return changed


def format_report(issues: list[str]) -> str:
    """Formats check failures with instructions for fixing them."""
    bullets = "\n".join(f"  * {issue}" for issue in issues)
    return f"""
======================================================================
[ERROR] Docs Categorization Failed
======================================================================

{bullets}

How to fix:
  1. If you added or removed a module in //PIGWEED_MODULES, add or
     remove its //<module>/module.toml file (and include
     "module.toml" in its sphinx_docs_library in //<module>/BUILD.bazel).
     Every module needs a `status` and at least one of `[category]` or
     `[facade]`.
  2. Regenerate the Sphinx, Doxygen, and rustdoc files:

       {SYNC_COMMAND}

  3. If you added or removed a category in {CATEGORIES_TOML_LABEL},
     `git add` or `git rm` its landing page under //docs/sphinx/.

See {CONTRIBUTOR_DOCS_URL}
and //docs/agents/metadata/AGENTS.md.
======================================================================
"""


# ==============================================================================
# CLI
# ==============================================================================


def find_workspace_root() -> Path:
    """Finds the root of the Pigweed checkout that contains this script.

    Raises:
        SyncError: PIGWEED_MODULES was not found in any parent directory.
    """
    if "BUILD_WORKSPACE_DIRECTORY" in os.environ:  # Set by `bazelisk run`.
        return Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
    for parent in Path(__file__).resolve().parents:
        if (parent / "PIGWEED_MODULES").is_file():
            return parent
    raise SyncError("PIGWEED_MODULES not found in any parent directory.")


def main() -> int:
    """Validates metadata TOML files, then updates or checks generated docs."""
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Report out-of-date files instead of updating them.",
    )
    args = parser.parse_args()

    try:
        root = find_workspace_root()
    except SyncError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    meta = Metadata.load(root)
    tree = DocsTree.for_workspace(root)

    issues = meta.validate()
    if issues:
        print(format_report(issues), file=sys.stderr)
        return 1

    if args.check:
        issues = check(tree, meta)
        if issues:
            print(format_report(issues), file=sys.stderr)
            return 1
        print(
            "The docs are in sync with categories.toml and "
            "//<module>/module.toml."
        )
        return 0

    try:
        changed = sync(tree, meta)
    except SyncError as exc:
        print(format_report([str(exc)]), file=sys.stderr)
        return 1
    for path in changed:
        print(f"Updated {path.relative_to(root)}")
    if not changed:
        print(
            "The docs are already in sync with categories.toml and "
            "//<module>/module.toml."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
