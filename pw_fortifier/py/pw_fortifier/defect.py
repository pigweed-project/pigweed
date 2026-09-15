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
"""Defines the Defect class for representing security/freshness defects."""

from dataclasses import dataclass, field
import logging
import re
from typing import Any

from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.issue import Issue

_LOG = logging.getLogger(__name__)

SOURCE_FILE_PATTERN: re.Pattern[str] = re.compile(r'[\w/.-]+\.(?:cpp|cc|c|h)\b')


@dataclass(kw_only=True)
class Defect(Issue):
    """A structured report representing a defect found in the codebase."""

    affected_files: list[str] = field(default_factory=list)

    @property
    def filename(self) -> str:
        """Convenience property returning the primary file path."""
        return str(self.location.file) if self.location else ''

    @classmethod
    def _parse_dict(cls, data: dict[str, Any]) -> dict[str, Any]:
        """Parses and normalizes Defect fields from a raw dictionary."""
        kwargs = super()._parse_dict(data)
        kwargs['affected_files'] = data.get('affected_files', [])
        return kwargs

    @classmethod
    def from_issue(cls, issue: Issue) -> 'Defect | None':
        """Converts a base Issue into a Defect.

        Args:
            issue: The Issue report fetched from the tracker.

        Returns:
            The converted Defect instance, or None if invalid.
        """
        if issue.issue_id is None or issue.issue_id == 0:
            _LOG.warning(
                'Malformed defect issue %r: missing or invalid issue_id',
                issue.title,
            )
            return None

        filename = ''
        if issue.title and ' - ' in issue.title and ':' in issue.title:
            after_dash = issue.title.split(' - ', 1)[1]
            filename = after_dash.split(':', 1)[0].strip()

        affected_files: list[str] = []
        if filename:
            affected_files.append(filename)
        if issue.description:
            matches = SOURCE_FILE_PATTERN.findall(issue.description)
            for match in matches:
                if match not in affected_files:
                    affected_files.append(match)

        location = CodeSnippet(file=filename) if filename else None

        return Defect(
            issue_id=issue.issue_id,
            title=issue.title,
            description=issue.description,
            comments=list(issue.comments),
            priority=issue.priority,
            severity=issue.severity,
            assignee=issue.assignee,
            cl_num=issue.cl_num,
            location=location,
            affected_files=affected_files,
        )
