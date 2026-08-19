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
"""CLI tool to query Buildbucket for build checks and deduplicate results."""

import sys
import re
import urllib.request
import urllib.error
import json
from typing import Any


def parse_args() -> tuple[str, str]:
    """Parses command line arguments."""
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 search_builds.py <CHANGE_ID> <PATCHSET_ID>")
        print("  python3 search_builds.py <GERRIT_URL>")
        sys.exit(1)

    if len(sys.argv) == 2:
        url = sys.argv[1]
        # Match URL pattern like:
        # https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/406352/4
        # or references to refs/changes/...
        match = re.search(r'/changes/\d+/(\d+)/(\d+)', url)
        if not match:
            match = re.search(r'/\+/(\d+)/(\d+)', url)
        if not match:
            match = re.search(r'(\d+)/(\d+)', url)
        if match:
            change_id, patchset_id = match.group(1), match.group(2)
        else:
            # Maybe just change ID?
            match_id = re.search(r'(\d+)', url)
            if match_id:
                change_id = match_id.group(1)
                # Ask/default to latest patchset or default to 1?
                # Actually, let's print error if patchset is missing
                print(
                    "Error: Could not parse both Change ID and Patchset ID "
                    "from argument."
                )
                sys.exit(1)
            else:
                print("Error: Invalid argument format.")
                sys.exit(1)
    else:
        change_id = sys.argv[1]
        patchset_id = sys.argv[2]

    return change_id, patchset_id


def fetch_all_builds(buildset: str) -> list[dict[str, Any]]:
    """Fetches all builds for a given buildset from Buildbucket."""
    url = (
        "https://cr-buildbucket.appspot.com/prpc/"
        "buildbucket.v2.Builds/SearchBuilds"
    )
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    all_builds = []
    page_token = None

    while True:
        body = {
            "predicate": {"tags": [{"key": "buildset", "value": buildset}]},
            "pageSize": 100,
        }
        if page_token:
            body["pageToken"] = page_token

        req = urllib.request.Request(
            url,
            data=json.dumps(body).encode('utf-8'),
            headers=headers,
            method="POST",
        )

        try:
            with urllib.request.urlopen(req) as r:
                response_text = r.read().decode('utf-8')
                # Strip the JSONP magic prefix if present
                if response_text.startswith(")]}'\n"):
                    response_text = response_text[5:]
                elif response_text.startswith(")]}'"):
                    response_text = response_text[4:]

                data = json.loads(response_text)
                builds = data.get("builds", [])
                all_builds.extend(builds)

                page_token = data.get("nextPageToken")
                if not page_token:
                    break
        except (urllib.error.URLError, json.JSONDecodeError) as e:
            print(f"Error calling Buildbucket API: {e}")
            sys.exit(1)

    return all_builds


def deduplicate_builds(
    builds: list[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    """Deduplicates builds by builder name, keeping the latest run."""
    unique_builds = {}
    for build in builds:
        builder = build.get("builder", {})
        builder_name = builder.get("builder")
        if not builder_name:
            continue

        create_time = build.get("createTime", "")

        # If we haven't seen this builder yet, or if this build is newer
        if builder_name not in unique_builds:
            unique_builds[builder_name] = build
        else:
            existing_build = unique_builds[builder_name]
            existing_create_time = existing_build.get("createTime", "")
            if create_time > existing_create_time:
                unique_builds[builder_name] = build

    return unique_builds


def print_build_summary(unique_builds: dict[str, dict[str, Any]]):
    """Prints a formatted summary of the unique builds and their statuses."""
    sorted_builders = sorted(unique_builds.keys())
    status_counts: dict[str, int] = {}
    failed_builds = []

    for name in sorted_builders:
        build = unique_builds[name]
        status = build.get("status", "UNKNOWN")
        status_counts[status] = status_counts.get(status, 0) + 1
        if status in ("FAILURE", "INFRA_FAILURE", "CANCELED"):
            failed_builds.append((name, build))

    print("--- Check Status Summary ---")
    print(f"Total Unique Builders: {len(unique_builds)}")
    for status, count in sorted(status_counts.items()):
        print(f"  {status}: {count}")
    print()

    if failed_builds:
        print("--- Failed/Canceled Builders (Latest Run) ---")
        for name, build in failed_builds:
            build_id = build.get("id")
            status = build.get("status")
            print(
                f"❌ {name:<40} {status:<15} "
                f"https://cr-buildbucket.appspot.com/build/{build_id}"
            )
        print()

    print("--- All Builders (Latest Run) ---")
    for name in sorted_builders:
        build = unique_builds[name]
        build_id = build.get("id")
        status = build.get("status")
        icon = (
            "✅"
            if status == "SUCCESS"
            else "⏳" if status in ("STARTED", "SCHEDULED") else "❌"
        )
        print(
            f"{icon} {name:<40} {status:<15} "
            f"https://cr-buildbucket.appspot.com/build/{build_id}"
        )


def main():
    """Main execution entry point."""
    change_id, patchset_id = parse_args()
    buildset = (
        f"patch/gerrit/pigweed-review.googlesource.com/"
        f"{change_id}/{patchset_id}"
    )
    print(f"Searching Buildbucket for buildset: {buildset}\n")

    builds = fetch_all_builds(buildset)
    if not builds:
        print("No builds found for this patchset.")
        return

    unique_builds = deduplicate_builds(builds)
    print_build_summary(unique_builds)


if __name__ == "__main__":
    main()
