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
"""Defines the Issue base class for pipeline findings."""

from dataclasses import dataclass, replace, asdict, fields, field
import json
from typing import TypeVar, Any

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet

_IssueT = TypeVar('_IssueT', bound='Issue')


@dataclass(kw_only=True)
class Issue:
    """A structured report representing a finding in the codebase."""

    issue_id: int | None = None
    title: str | None = None
    description: str | None = None
    comments: list[str] = field(default_factory=list)
    priority: int = 4
    severity: int = 4
    assignee: str | None = None
    cl_num: int | None = None
    location: CodeSnippet | None = None

    @classmethod
    def _parse_dict(cls, data: dict[str, Any]) -> dict[str, Any]:
        """Parses and normalizes common Issue fields from a raw dictionary."""
        cls_fields = {f.name for f in fields(cls)}
        kwargs = {k: v for k, v in data.items() if k in cls_fields}
        kwargs.setdefault('priority', 4)
        kwargs.setdefault('severity', 4)
        if kwargs.get('comments') is None:
            kwargs['comments'] = []
        if 'location' in data and data['location'] is not None:
            loc = data['location']
            if isinstance(loc, dict):
                lines = (
                    tuple(loc['lines'])
                    if loc.get('lines') is not None
                    else None
                )
                kwargs['location'] = CodeSnippet(file=loc['file'], lines=lines)
            elif isinstance(loc, CodeSnippet):
                kwargs['location'] = loc
        elif 'filename' in data and data['filename']:
            kwargs['location'] = CodeSnippet(file=data['filename'])
        elif 'source' in data and data['source']:
            kwargs['location'] = CodeSnippet(file=data['source'])
        return kwargs

    def _to_dict(self) -> dict[str, Any]:
        """Converts the instance to a dictionary for JSON serialization."""
        d = asdict(self)
        if self.location is not None:
            lines = list(self.location.lines) if self.location.lines else None
            d['location'] = {
                'file': str(self.location.file),
                'lines': lines,
            }
        return d

    @classmethod
    async def load(cls: type[_IssueT], path: AsyncPath) -> _IssueT:
        """Loads and reconstructs an Issue from a JSON file.

        Args:
          path: The file path to load the JSON report from.

        Returns:
          An Issue instance populated with the loaded data.
        """
        content = await path.read_text()
        data = json.loads(content)
        parsed_kwargs = cls._parse_dict(data)
        return cls(**parsed_kwargs)

    async def save(self, path: AsyncPath) -> None:
        """Serializes and saves the Issue to a JSON file.

        Args:
          path: The file path to write the JSON report to.
        """
        data = self._to_dict()
        content = json.dumps(data, sort_keys=True, indent=4)
        await path.write_text(content)

    def _replace(self: _IssueT, **kwargs) -> _IssueT:
        """Returns a copy of the Issue with updated fields."""
        return replace(self, **kwargs)

    @classmethod
    def from_issue(cls: type[_IssueT], issue: 'Issue') -> _IssueT | None:
        """Converts an untyped tracker Issue into this Issue subtype."""
        if cls is Issue:
            return issue  # type: ignore[return-value]
        raise NotImplementedError
