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
"""End command for the changelog scripts."""


import common


MONTH_NAMES = {
    "01": "January",
    "02": "February",
    "03": "March",
    "04": "April",
    "05": "May",
    "06": "June",
    "07": "July",
    "08": "August",
    "09": "September",
    "10": "October",
    "11": "November",
    "12": "December",
}


def _get_month_name(month: str) -> str:
    """Returns the full name of the month or the string itself."""
    return MONTH_NAMES.get(month, month)


def _build_month_header(year: str, month: str) -> list[str]:
    """Generates the top-level ReST header for the month."""
    date_str = f"{_get_month_name(month)} {year}"
    title = f"What's new in Pigweed: {date_str}"

    rst = []
    rst.append("=" * len(title))
    rst.append(title)
    rst.append("=" * len(title))
    rst.append("")
    return rst


def _section_id(category: str, story_id: str, year: str, month: str) -> str:
    """Generates a unique ReST section reference ID for a story."""
    safe_category = category.replace('_', '-')
    safe_story = story_id.replace('_', '-')
    return f"changelog-{year}-{month}-{safe_category}-{safe_story}"


def _get_highlights(
    data: dict,
    ignore_list: list | None = None,
) -> list[dict]:
    """Extracts and sorts the highlighted stories."""
    highlight_stories = []
    ignore_list = ignore_list or []

    stories = data.get("stories", {})
    for category, category_stories in stories.items():
        if category in ignore_list:
            continue
        if not isinstance(category_stories, dict):
            continue

        for story_key, story in category_stories.items():
            if not isinstance(story, dict):
                continue

            highlight = story.get("highlight", "")
            final_score = story.get("score", 0)
            if highlight and highlight.strip() and final_score >= 750:
                highlight_stories.append(
                    {
                        "score": final_score,
                        "category": category,
                        "story_key": story_key,
                        "title": story.get("title", story_key),
                        "highlight": highlight.strip(),
                    }
                )

    highlight_stories.sort(key=lambda x: x["score"], reverse=True)
    return highlight_stories


def _build_highlights_section(
    highlights: list[dict],
    year: str,
    month: str,
    categories_map: dict,
) -> list[str]:
    """Generates the Highlights section of the changelog."""
    rst = []
    rst.append(".. changelog_highlights_start")
    rst.append("")
    rst.append("Highlights:")
    rst.append("")

    for item in highlights:
        category_name = categories_map.get(item["category"]) or item["category"]
        sid = _section_id(item["category"], item["story_key"], year, month)

        lines = item["highlight"].split('\n')
        if lines:
            rst.append(f"* {category_name}: :ref:`{sid}` - {lines[0].lstrip()}")
            for line in lines[1:]:
                if line.strip():
                    rst.append(f"  {line.rstrip()}")
                else:
                    rst.append("")
        rst.append("")

    rst.append(".. changelog_highlights_end")
    rst.append("")
    return rst


def _category_sort_key(category: str) -> tuple[int, str]:
    """Sorts pw_* categories first, then alphabetically, else alphabetically."""
    if category.startswith("pw_"):
        return (0, category)
    return (1, category)


def _build_story_section(
    category: str, story_key: str, story: dict, year: str, month: str
) -> list[str]:
    """Generates ReST for an individual story."""
    rst = []
    h3_title = story.get("title", story_key)
    sid = _section_id(category, story_key, year, month)
    rst.append(f".. _{sid}:")
    rst.append("")

    rst.append(h3_title)
    rst.append("=" * len(h3_title))
    rst.append("")

    body = story.get("body", "")
    commits = story.get("commits", {})

    links = []
    if commits:
        for i, (_, commit_info) in enumerate(sorted(commits.items()), 1):
            url = commit_info.get("url", "")
            url = url.replace(
                "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/",
                "https://pwrev.dev/",
            )
            links.append(f"`{i} <{url}>`__")

    if body and body.strip():
        if links:
            rst.append(f"{body.strip()} CLs: {', '.join(links)}")
        else:
            rst.append(body.strip())
        rst.append("")
    elif links:
        rst.append(f"CLs: {', '.join(links)}")
        rst.append("")

    return rst


def _build_category_section(
    category: str,
    category_stories: dict,
    year: str,
    month: str,
    categories_map: dict,
) -> list[str]:
    """Generates ReST for a category and its underlying stories."""
    if not isinstance(category_stories, dict):
        return []

    # Filter out stories with low user-facing impact.
    renderable_stories = {
        k: v
        for k, v in category_stories.items()
        if isinstance(v, dict) and v.get("score", 0) > 250
    }

    if not renderable_stories:
        return []

    rst = []
    h2_title = categories_map.get(category) or category
    rst.append("-" * len(h2_title))
    rst.append(h2_title)
    rst.append("-" * len(h2_title))
    rst.append("")

    # Sort primarily by score descending, secondarily by story_key ascending
    sorted_story_items = sorted(
        renderable_stories.items(),
        key=lambda item: (-item[1].get("score", 0), item[0]),
    )

    for story_key, story in sorted_story_items:
        rst.extend(
            _build_story_section(category, story_key, story, year, month)
        )

    return rst


def build_rst_for_month(
    year: str,
    month: str,
    data: dict,
    categories_map: dict,
    ignore_list: list | None = None,
) -> str:
    """Builds reStructuredText for a single month."""
    rst = []
    ignore_list = ignore_list or []
    rst.extend(_build_month_header(year, month))

    highlights = _get_highlights(data, ignore_list)
    rst.extend(
        _build_highlights_section(highlights, year, month, categories_map)
    )

    stories = data.get("stories", {})
    for category in sorted(stories.keys(), key=_category_sort_key):
        if category in ignore_list:
            continue
        rst.extend(
            _build_category_section(
                category, stories[category], year, month, categories_map
            )
        )

    return "\n".join(rst) + "\n"


def export_rst(args) -> None:
    """Exports story data to a month-specific ReST file."""
    data = common.get_data(args.year, args.month)
    if not data or not data.get("stories"):
        return

    categories_data = common.load_categories_data()
    categories_map = {
        k: v.get("title", k)
        for k, v in categories_data.items()
        if v.get("show", True)
    }
    ignore_list = [
        k for k, v in categories_data.items() if not v.get("show", True)
    ]

    month_str = f"{args.month:02d}"
    rst_content = build_rst_for_month(
        str(args.year),
        month_str,
        data,
        categories_map,
        ignore_list,
    )

    dest_dir = common.get_changelog_dir() / str(args.year)
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest_file = dest_dir / f"{month_str}.rst"
    dest_file.write_text(rst_content)


def end_command(args) -> None:
    """CLI entrypoint for the changelog automation `end` subcommand."""
    export_rst(args)
