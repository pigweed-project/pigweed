# Copyright 2025 The Pigweed Authors
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
"""Code formatter plugin for OWNERS files."""

import collections
import dataclasses
import enum
from pathlib import Path
import re
from typing import (
    Callable,
    DefaultDict,
    OrderedDict,
)

from pw_cli.file_filter import FileFilter
from pw_presubmit.format.core import (
    FileFormatter,
    FormattedFileContents,
    FormatFixStatus,
)


DEFAULT_OWNERS_FILE_PATTERNS = FileFilter(name=['^OWNERS$'])


class OwnersError(Exception):
    """Generic level OWNERS file error."""


class FormatterError(OwnersError):
    """Errors where formatter doesn't know how to act."""


class OwnersDuplicateError(OwnersError):
    """Errors where duplicate lines are found in OWNERS files."""


class OwnersUserGrantError(OwnersError):
    """Invalid user grant, * is used with any other grant."""


class OwnersProhibitedError(OwnersError):
    """Any line that is prohibited by the owners syntax.

    https://android-review.googlesource.com/plugins/code-owners/Documentation/backend-find-owners.html
    """


class OwnersDependencyError(OwnersError):
    """OWNERS file tried to import file that does not exist."""


class OwnersInvalidLineError(OwnersError):
    """_Line in OWNERS file does not match any 'line_typer'."""


class OwnersStyleError(OwnersError):
    """OWNERS file does not match style guide."""


@dataclasses.dataclass
class _Line:
    content: str
    comments: list[str] = dataclasses.field(default_factory=list)


class OwnersFile:
    """Holds OWNERS file in easy to use parsed structure."""

    class _LineType(enum.Enum):
        COMMENT = "comment"
        WILDCARD = "wildcard"
        FILE_LEVEL = "file_level"
        FILE_RULE = "file_rule"
        INCLUDE = "include"
        PER_FILE = "per-file"
        USER = "user"
        # Special type to hold lines that don't get attached to another type
        TRAILING_COMMENTS = "trailing-comments"

    _LINE_TYPERS: OrderedDict[_LineType, Callable[[str], bool]] = (
        collections.OrderedDict(
            (
                (_LineType.COMMENT, lambda x: x.startswith("#")),
                (_LineType.WILDCARD, lambda x: x == "*"),
                (_LineType.FILE_LEVEL, lambda x: x.startswith("set ")),
                (_LineType.FILE_RULE, lambda x: x.startswith("file:")),
                (_LineType.INCLUDE, lambda x: x.startswith("include ")),
                (_LineType.PER_FILE, lambda x: x.startswith("per-file ")),
                (
                    _LineType.USER,
                    lambda x: bool(
                        re.match("^[a-zA-Z1-9.+-]+@[a-zA-Z0-9.-]+", x)
                    ),
                ),
            )
        )
    )
    path: Path
    original_lines: list[str]
    sections: dict[_LineType, list[_Line]]
    formatted_lines: list[str]

    def __init__(self, path: Path, file_contents: bytes) -> None:
        self.path = path

        self.original_lines = file_contents.decode().split('\n')
        cleaned_lines = self._clean_lines(self.original_lines)
        self.sections = self._parse_owners(cleaned_lines)
        self.formatted_lines = self._format_sections(self.sections)

    @staticmethod
    def load_from_path(owners_file: Path) -> 'OwnersFile':
        return OwnersFile(
            owners_file,
            OwnersFile._read_owners_file(owners_file),
        )

    @staticmethod
    def _read_owners_file(owners_file: Path) -> bytes:
        if not owners_file.exists():
            raise OwnersDependencyError(
                f"Tried to load {owners_file} but it does not exist"
            )
        return owners_file.read_bytes()

    @staticmethod
    def _clean_lines(dirty_lines: list[str]) -> list[str]:
        """Removes extra whitespace from list of strings."""

        cleaned_lines = []
        for line in dirty_lines:
            line = line.strip()  # Remove initial and trailing whitespace

            # Compress duplicated whitespace and remove tabs.
            # Allow comment lines to break this rule as they may have initial
            # whitespace for lining up text with neighboring lines.
            if not line.startswith("#"):
                line = re.sub(r"\s+", " ", line)
            if line:
                cleaned_lines.append(line)
        return cleaned_lines

    @staticmethod
    def _find_line_type(line: str) -> _LineType:
        for line_type, type_matcher in OwnersFile._LINE_TYPERS.items():
            if type_matcher(line):
                return line_type

        raise OwnersInvalidLineError(
            f"Unrecognized OWNERS file line, '{line}'."
        )

    @staticmethod
    def _parse_owners(
        cleaned_lines: list[str],
    ) -> DefaultDict[_LineType, list[_Line]]:
        """Converts text lines of OWNERS into structured object."""
        sections: DefaultDict[OwnersFile._LineType, list[_Line]] = (
            collections.defaultdict(list)
        )
        comment_buffer: list[str] = []

        def add_line_to_sections(
            sections,
            section: OwnersFile._LineType,
            line: str,
            comment_buffer: list[str],
        ):
            if any(
                seen_line.content == line for seen_line in sections[section]
            ):
                raise OwnersDuplicateError(f"Duplicate line '{line}'.")
            line_obj = _Line(content=line, comments=comment_buffer)
            sections[section].append(line_obj)

        for line in cleaned_lines:
            line_type: OwnersFile._LineType = OwnersFile._find_line_type(line)
            if line_type == OwnersFile._LineType.COMMENT:
                comment_buffer.append(line)
            else:
                add_line_to_sections(sections, line_type, line, comment_buffer)
                comment_buffer = []

        add_line_to_sections(
            sections, OwnersFile._LineType.TRAILING_COMMENTS, "", comment_buffer
        )

        return sections

    @staticmethod
    def _format_sections(
        sections: DefaultDict[_LineType, list[_Line]]
    ) -> list[str]:
        """Returns ideally styled OWNERS file.

        The styling rules are
        * Content will be sorted in the following orders with a blank line
        separating
            * "set noparent"
            * "include" lines
            * "file:" lines
            * user grants (example, "*", foo@example.com)
            * "per-file:" lines
        * Do not combine user grants and "*"
        * User grants should be sorted alphabetically (this assumes English
        ordering)

        Returns:
          List of strings that make up a styled version of a OWNERS file.

        Raises:
          FormatterError: When formatter does not handle all lines of input.
                          This is a coding error in owners_checks.
        """
        all_sections = [
            OwnersFile._LineType.FILE_LEVEL,
            OwnersFile._LineType.INCLUDE,
            OwnersFile._LineType.FILE_RULE,
            OwnersFile._LineType.WILDCARD,
            OwnersFile._LineType.USER,
            OwnersFile._LineType.PER_FILE,
            OwnersFile._LineType.TRAILING_COMMENTS,
        ]
        formatted_lines: list[str] = []

        def append_section(line_type):
            # Add a line of separation if there was a previous section and our
            # current section has any content. I.e. do not lead with padding and
            # do not have multiple successive lines of padding.
            if (
                formatted_lines
                and line_type != OwnersFile._LineType.TRAILING_COMMENTS
                and sections[line_type]
            ):
                formatted_lines.append("")

            sections[line_type].sort(key=lambda line: line.content)
            for line in sections[line_type]:
                # Strip keep-sorted comments out since sorting is done by this
                # script
                formatted_lines.extend(
                    [
                        comment
                        for comment in line.comments
                        if not comment.startswith("# keep-sorted: ")
                    ]
                )
                formatted_lines.append(line.content)

        for section in all_sections:
            append_section(section)

        if any(section_name not in all_sections for section_name in sections):
            raise FormatterError("Formatter did not process all sections.")
        return formatted_lines

    def look_for_owners_errors(self) -> None:
        """Scans owners files for invalid or useless content."""

        # Confirm when using the wildcard("*") we don't also try to use
        # individual user grants.
        if (
            self.sections[OwnersFile._LineType.WILDCARD]
            and self.sections[OwnersFile._LineType.USER]
        ):
            raise OwnersUserGrantError(
                "Do not use '*' with individual user "
                "grants, * already applies to all users."
            )


class OwnersFormatter(FileFormatter):
    """A Python-native formatter for OWNERS files."""

    def __init__(self, **kwargs):
        kwargs.setdefault('mnemonic', 'OWNERS')
        kwargs.setdefault('file_patterns', DEFAULT_OWNERS_FILE_PATTERNS)
        super().__init__(**kwargs)

    def format_file_in_memory(
        self, file_path: Path, file_contents: bytes
    ) -> FormattedFileContents:
        """Checks the formatting of the requested file.

        The file at ``file_path`` is NOT modified by this check.

        Returns:
            A populated
            :py:class:`pw_presubmit.format.core.FormattedFileContents` that
            contains either the result of formatting the file, or an error
            message.
        """
        # `gn format --dry-run` only signals which files need to be updated,
        # not what the effects are. To get the formatted result, we pipe the
        # file in via stdin and then return the result.
        try:
            file_to_format = OwnersFile(file_path, file_contents)
            formatted_version = '\n'.join(
                file_to_format.formatted_lines
            ).encode()
            return FormattedFileContents(
                ok=True,
                formatted_file_contents=formatted_version,
                error_message=None,
            )
        except OwnersError as err:
            return FormattedFileContents(
                ok=False,
                formatted_file_contents=b'',
                error_message=str(err),
            )
        try:
            file_to_format.look_for_owners_errors()
        except OwnersError as err:
            return FormattedFileContents(
                ok=False,
                formatted_file_contents=formatted_version,
                error_message=str(err),
            )

    def format_file(self, file_path: Path) -> FormatFixStatus:
        """Formats the provided file in-place using ``gn format``.

        Returns:
            A FormatFixStatus that contains relevant errors/warnings.
        """
        try:
            file_to_format = OwnersFile.load_from_path(file_path)
            file_path.write_text('\n'.join(file_to_format.formatted_lines))
            return FormatFixStatus(
                ok=True,
                error_message=None,
            )
        except OwnersError as err:
            return FormatFixStatus(
                ok=False,
                error_message=str(err),
            )
