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
"""Next command for the changelog scripts."""

import common


def get_next(shas: list[str], data: dict, count: int = 25) -> list[str]:
    """Gets the next unacknowledged SHAs from the list of repository SHAs."""
    done = {sha for sha, _ in _all_commits(data)}
    next_shas = []
    for sha in shas:
        if sha not in done:
            next_shas.append(sha)
            if len(next_shas) == count:
                break
    return next_shas


def _all_commits(data: dict):
    """Yields (sha, commit) for all commits in data."""
    for _, _, story in _all_stories(data):
        for sha, commit in story.get("commits", {}).items():
            yield sha, commit


def populate_commits(data: dict, repo) -> None:
    """Populates missing date, title, and url for all commits in data."""
    for sha, commit in _all_commits(data):
        message = repo.commit_message(sha)
        commit.setdefault("title", message.splitlines()[0])
        commit.setdefault("url", repo.commit_review_url(sha))
        commit.setdefault("date", str(repo.commit_date(sha)))


def check_shas(data: dict, true_shas: set) -> dict:
    """Verifies all stored SHAs correspond to valid local git SHAs."""
    gathered_shas = {sha for sha, _ in _all_commits(data)}

    invalid_shas = gathered_shas - true_shas
    if invalid_shas:
        valid_shas = gathered_shas & true_shas
        return {
            "valid": False,
            "invalid_shas": list(invalid_shas),
            "valid_shas": list(valid_shas),
        }
    return {"valid": True}


def check_categories_validity(data: dict, valid_categories: dict) -> dict:
    """Verifies all stated categories inside story data are valid."""
    gathered_categories = set(data.get("stories", {}).keys())
    invalid_categories = gathered_categories - set(valid_categories.keys())
    if invalid_categories:
        return {
            "valid": False,
            "invalid_categories": list(invalid_categories),
            "valid_categories": sorted(list(valid_categories.keys())),
        }
    return {"valid": True}


def _all_stories(data: dict):
    """Yields (category_name, story_id, story) for all stories."""
    for category_name, category in data.get("stories", {}).items():
        for story_id, story in category.items():
            yield category_name, story_id, story


def check_stories(data: dict) -> dict:
    """Verifies that every story has a title, body, highlight, and score."""
    invalid_stories = []
    for category_name, story_id, story in _all_stories(data):
        missing = [
            field
            for field in ["title", "body", "highlight", "score"]
            if field not in story
        ]
        if missing:
            invalid_stories.append(
                {
                    "category": category_name,
                    "story_id": story_id,
                    "missing": missing,
                }
            )

    if invalid_stories:
        return {"valid": False, "invalid_stories": invalid_stories}
    return {"valid": True}


def _all_stories_with_scores(data: dict):
    """Yields (story_full_id, score) for all stories with a valid score."""
    for category_name, story_id, story in _all_stories(data):
        score = story.get("score")
        if score is not None and score != 0:
            yield f"{category_name}.{story_id}", score


def check_scores_uniqueness(data: dict) -> dict:
    """Ensures each score between 1 and 100 is only used once."""
    score_to_stories: dict[int, list[str]] = {}
    for story_full_id, score in _all_stories_with_scores(data):
        score_to_stories.setdefault(score, []).append(story_full_id)

    duplicates = [
        {"score": score, "stories": stories}
        for score, stories in score_to_stories.items()
        if len(stories) > 1
    ]

    if duplicates:
        return {"valid": False, "duplicates": duplicates}
    return {"valid": True}


def get_categories() -> dict:
    """Loads all categories."""
    data = common.load_categories_data()
    return {k: v.get("title", k) for k, v in data.items()}


def next_command(args) -> None:
    """CLI entrypoint for the changelog automation `next` subcommand."""
    repo = common.get_repo()
    shas = common.get_shas(args, repo)
    data = common.get_data(args.year, args.month)
    # Check SHAs first
    shas_data = check_shas(data, set(shas))
    if not shas_data.get("valid", True):
        instructions = "Fix the invalid SHAs that were found in data.toml."
        common.write_json_resource(
            "next.json",
            {"shas": shas_data, "instructions": instructions},
        )
        return

    # Then check stories
    stories_data = check_stories(data)
    if not stories_data.get("valid", True):
        instructions = (
            "Fix the incomplete stories that were found in data.toml. "
            "Every story must have a title, body, highlight, and score."
        )
        common.write_json_resource(
            "next.json",
            {
                "stories": stories_data,
                "instructions": instructions,
            },
        )
        return
    # Then check scores uniqueness

    scores_data = check_scores_uniqueness(data)
    if not scores_data.get("valid", True):
        instructions = (
            "Fix the duplicate scores found in data.toml. Each score "
            "between 1 and 1000 must be unique."
        )
        common.write_json_resource(
            "next.json",
            {
                "scores": scores_data,
                "instructions": instructions,
            },
        )
        return

    # Then check categories
    categories_data = check_categories_validity(data, get_categories())
    if not categories_data.get("valid", True):
        instructions = (
            "Fix the invalid categories that were found in data.toml."
        )
        common.write_json_resource(
            "next.json",
            {
                "categories": categories_data,
                "instructions": instructions,
            },
        )
        return
    # Once valid, populate missing data and fix format
    populate_commits(data, repo)
    data_file = common.get_data_toml_file()
    common.fix_data(data, data_file)
    # Reload to ensure we work with the latest
    data = common.get_data(args.year, args.month)
    # Process commits in batches
    next_shas = get_next(shas, data)
    if not next_shas:
        instructions = "No more commits to process. Proceed to the next step."
        common.write_json_resource(
            "next.json",
            {
                "next": None,
                "instructions": instructions,
            },
        )
        return

    next_commits = []
    for sha in next_shas:
        message = repo.commit_message(sha)
        next_commits.append(
            {
                "sha": sha,
                "message": message,
                "diff": repo.diff(f"{sha}^!"),
                "url": repo.commit_review_url(sha),
                "date": str(repo.commit_date(sha)),
                "title": message.splitlines()[0],
            }
        )

    instructions = "Add these commits to data.toml."
    out_data = {
        "next": next_commits,
        "instructions": instructions,
    }
    common.write_json_resource("next.json", out_data)
    print(out_data)
