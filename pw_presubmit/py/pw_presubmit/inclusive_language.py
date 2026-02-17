# Copyright 2021 The Pigweed Authors
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
"""Inclusive language presubmit check."""

import dataclasses
from pathlib import Path
import re

from pw_cli import file_filter

from . import presubmit, presubmit_context

# List borrowed from Android:
# https://source.android.com/setup/contribute/respectful-code
# inclusive-language: disable
NON_INCLUSIVE_WORDS = (
    r'master',
    r'slave',
    r'red[-\s]?line',
    r'(white|gr[ae]y|black)[-\s]*(list|hat)',
    r'craz(y|ie)',
    r'insane',
    r'crip+led?',
    r'sanity',
    r'sane',
    r'dummy',
    r'grandfather',
    r's?he',
    r'his',
    r'her',
    r'm[ae]n[-\s]*in[-\s]*the[-\s]*middle',
    r'mitm',
    r'first[-\s]?class[-\s]?citizen',
)
# inclusive-language: enable

# Test: master  # inclusive-language: ignore
# Test: master


def _process_inclusive_language(*words: str) -> re.Pattern[str]:
    """Turn word list into one big regex with common inflections."""

    if not words:
        words = NON_INCLUSIVE_WORDS

    all_words = []
    for entry in words:
        if isinstance(entry, str):
            all_words.append(entry)
        elif isinstance(entry, (list, tuple)):
            all_words.extend(entry)
        all_words.extend(x for x in words)

    # Confirm each individual word compiles as a valid regex.
    for word in all_words:
        _ = re.compile(word)

    word_boundary = (
        r'(\b|_|(?<=[a-z])(?=[A-Z])|(?<=[0-9])(?=\w)|(?<=\w)(?=[0-9]))'
    )

    return re.compile(
        r"({b})(?i:{w})(e?[sd]{b}|{b})".format(
            w='|'.join(all_words), b=word_boundary
        ),
    )


NON_INCLUSIVE_WORDS_REGEX = _process_inclusive_language()

# If seen, ignore this line and the next.
IGNORE = 'inclusive-language: ignore'

# Ignore a whole section. Please do not change the order of these lines.
DISABLE = 'inclusive-language: disable'
ENABLE = 'inclusive-language: enable'

ISSUE_TYPE = 'non-inclusive word'


@dataclasses.dataclass(frozen=True)
class PathMatch:
    issue_type: str
    word: str

    def __repr__(self):
        return f'Found {self.issue_type} "{self.word}" in file path'


@dataclasses.dataclass(frozen=True)
class LineMatch:
    issue_type: str
    line: int
    word: str

    def __repr__(self):
        return f'Found {self.issue_type} "{self.word}" on line {self.line}'


def check_file(
    path: Path,
    found_words: dict[Path, list[PathMatch | LineMatch]],
    words_regex: re.Pattern[str] = NON_INCLUSIVE_WORDS_REGEX,
    disable_tag: str = DISABLE,
    enable_tag: str = ENABLE,
    ignore_tag: str = IGNORE,
    issue_type: str = ISSUE_TYPE,
    check_path: bool = True,
    root: Path | None = None,
) -> None:
    """Check one file for non-inclusive language.

    Args:
        path: File to check.
        found_words: Output. Data structure where found words are added.
        words_regex: Pattern of non-inclusive terms.
        check_path: Whether to check the path instead of just the contents.
            (Used for testing.)
        root: Path to add as a prefix to path.
    """
    if check_path:
        match = words_regex.search(str(path))
        if match:
            found_words.setdefault(path, [])
            found_words[path].append(PathMatch(issue_type, match.group(0)))

    if path.is_symlink() or path.is_dir():
        return

    try:
        if root:
            path = root / path

        with open(path, 'r') as ins:
            enabled = True
            prev = ''
            for i, line in enumerate(ins, start=1):
                if disable_tag in line:
                    enabled = False
                if enable_tag in line:
                    enabled = True

                # If we see the ignore line on this or the previous line we
                # ignore any bad words on this line.
                ignored = ignore_tag in prev or ignore_tag in line

                if enabled and not ignored:
                    match = words_regex.search(line)

                    if match:
                        found_words.setdefault(path, [])
                        found_words[path].append(
                            LineMatch(issue_type, i, match.group(0))
                        )

                # Not using 'continue' so this line always executes.
                prev = line

    except UnicodeDecodeError:
        # File is not text, like a gif.
        pass


def generic_presubmit_check(
    ctx: presubmit_context.PresubmitContext,
    words_regex: re.Pattern[str],
    *,
    disable_tag: str,
    enable_tag: str,
    ignore_tag: str,
    issue_type: str,
) -> None:
    """Presubmit check that ensures files do not contain banned words."""

    # No subprocesses are run for inclusive_language so don't perform this check
    # if dry_run is on.
    if ctx.dry_run:
        return

    found_words: dict[Path, list[PathMatch | LineMatch]] = {}

    ctx.paths = presubmit_context.apply_exclusions(ctx)

    for path in ctx.paths:
        check_file(
            path.relative_to(ctx.root),
            found_words,
            words_regex,
            disable_tag=disable_tag,
            enable_tag=enable_tag,
            ignore_tag=ignore_tag,
            issue_type=issue_type,
            root=ctx.root,
        )

    if found_words:
        with open(ctx.failure_summary_log, 'w') as outs:
            for i, (path, matches) in enumerate(found_words.items()):
                if i:
                    print('=' * 40, file=outs)
                print(path, file=outs)
                for match in matches:
                    print(match, file=outs)

        print(ctx.failure_summary_log.read_text(), end=None)

        print()
        print(
            f"""
Individual lines can be ignored with "{ignore_tag}". Blocks can be
ignored with "{disable_tag}" and reenabled with
"{enable_tag}".
""".strip()
        )
        raise presubmit_context.PresubmitFailure


@presubmit.check(
    name='inclusive_language',
    path_filter=file_filter.FileFilter(exclude=[r'MODULE\.bazel\.lock']),
)
def presubmit_check(ctx: presubmit_context.PresubmitContext) -> None:
    generic_presubmit_check(
        ctx,
        words_regex=NON_INCLUSIVE_WORDS_REGEX,
        disable_tag=DISABLE,
        enable_tag=ENABLE,
        ignore_tag=IGNORE,
        issue_type=ISSUE_TYPE,
    )


def inclusive_language_checker(*words):
    """Create banned words checker for the given list of banned words."""

    regex = _process_inclusive_language(*words)

    def inclusive_language(  # pylint: disable=redefined-outer-name
        ctx: presubmit_context.PresubmitContext,
    ):
        globals()['inclusive_language'](ctx, regex)

    return inclusive_language
