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
"""Fast standalone reStructuredText syntax and code-block presubmit checks."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass, field
import re
import textwrap

from pw_presubmit.v2 import Context, step

# Optional in minimal downstream GN venvs that omit sphinx and pw_console.
try:
    import docutils.core  # type: ignore
    import docutils.utils  # type: ignore
    from docutils import nodes  # type: ignore
    from docutils.parsers.rst import roles  # type: ignore
    import pygments.lexers  # type: ignore
    from pygments.token import Error as TokenError  # type: ignore
    from pygments.util import ClassNotFound  # type: ignore

    _HAS_DEPS = True
except ImportError:
    _HAS_DEPS = False

# Standard docutils parser settings
# (https://docutils.sourceforge.io/docs/user/config.html) for hermetic
# in-memory syntax checking; independent of docs/sphinx/conf.py:
_DOCUTILS_SETTINGS = {
    '_disable_config': True,  # Ignore host /etc/docutils.conf or ~/.docutils.
    'file_insertion_enabled': False,  # Do not read `.. include::` from disk.
    'raw_enabled': False,  # Do not execute `.. raw::` directives.
    'halt_level': 5,  # Collect SEVERE/4 errors as nodes instead of raising.
    'syntax_highlight': 'none',  # Code blocks are lexed separately below.
    'warning_stream': False,  # Discard stderr; inspect system_message nodes.
}

# Standalone docutils does not load Sphinx extensions and disables file
# insertion / raw HTML above, so ignore diagnostics caused solely by those
# two differences:
_IGNORED_DOCUTILS_MESSAGES = (
    # Unregistered Sphinx directives and roles (`.. toctree::`, `:ref:`, etc.).
    re.compile(r'^Unknown (?:directive type|interpreted text role) "'),
    # Unregistered Sphinx role inside `.. |name| replace:: :ref:...`.
    re.compile(
        r'^Substitution definition contains illegal element <problematic>'
    ),
    # Sphinx-only options on `.. code-block::` (docutils only knows `:class:`,
    # `:name:`, and `:number-lines:`).
    re.compile(
        r'^Error in "(?:code-block|sourcecode|code)" directive:\s+'
        r'unknown option: "(?:caption|dedent|emphasize-lines|force|'
        r'lineno-start|linenos)"\.'
    ),
    # Expected from `file_insertion_enabled=False` (`.. include::`) and
    # `raw_enabled=False` (`.. raw:: html`, `:raw-html:`, `.. |br| raw:: html`).
    re.compile(r'^".+" directive disabled\.$'),
    re.compile(r'^raw \(and derived\) roles disabled'),
    re.compile(r'^Substitution definition ".+" empty or invalid\.$'),
)

_CODE_DIRECTIVE_RE = re.compile(
    r'^\.\.\s+(?:code-block|sourcecode|code)::(?:\s+(\S+))?(?:\s|$)'
)
_OPTION_FIELD_RE = re.compile(r'^:[a-zA-Z0-9_+-]+:(?:\s|$)')
_SKIPPED_LEXERS = frozenset({'', 'default', 'guess', 'none', 'text'})

# Directives whose indented body is unparsed literal/raw text rather than reST.
_UNPARSED_DIRECTIVES = ('raw::', 'parsed-literal::')
_LIST_PREFIX_RE = re.compile(
    r'^(?:[-*+]|(?:\d+|[#a-zA-Z])[.)]|\((?:\d+|[#a-zA-Z])\))\s+'
)


@dataclass(frozen=True, order=True)
class RstIssue:
    """A reST diagnostic at a 1-based line number."""

    line: int
    message: str


@dataclass
class _CodeBlock:
    """An explicit code-block directive and its indented body lines."""

    lineno: int
    lang: str
    indent: int
    lines: list[str] = field(default_factory=list)


def _is_ignored_docutils_message(msg: str) -> bool:
    return any(pattern.search(msg) for pattern in _IGNORED_DOCUTILS_MESSAGES)


def _unparsed_block_indent(stripped: str, indent: int) -> int | None:
    if stripped == '..':
        return indent
    if stripped.startswith('.. '):
        rest = stripped[3:].lstrip()
        head = rest.split(maxsplit=1)[0]
        # Skip raw/parsed-literal directives and reST comments (`.. text` that
        # is not a `_target`, `|substitution|`, `[footnote]`, or `directive::`).
        if head in _UNPARSED_DIRECTIVES or (
            not rest.startswith(('_', '|', '[')) and not head.endswith('::')
        ):
            return indent
        return None
    if stripped.endswith('::'):
        # For `* item::` or `1. item::`, only lines indented past the
        # item's text column belong to the literal block.
        if match := _LIST_PREFIX_RE.match(stripped):
            return indent + match.end()
        return indent
    return None


def _iter_code_blocks(text: str) -> Iterator[_CodeBlock]:
    """Yields each explicit code-block directive and its indented body."""
    active: _CodeBlock | None = None
    skip_indent: int | None = None
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.expandtabs(8)
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())

        if skip_indent is not None:
            if not stripped or indent > skip_indent:
                continue
            skip_indent = None

        if active is not None:
            if not stripped or indent > active.indent:
                active.lines.append(line if stripped else '')
                continue
            yield active
            active = None

        if match := _CODE_DIRECTIVE_RE.match(stripped):
            active = _CodeBlock(lineno, match.group(1) or '', indent)
        else:
            skip_indent = _unparsed_block_indent(stripped, indent)

    if active is not None:
        yield active


def _lex_code_block(block: _CodeBlock) -> RstIssue | None:
    """Checks lexer name and Pygments token errors for a single code-block."""
    if block.lang.lower() in _SKIPPED_LEXERS:
        return None
    try:
        lexer = pygments.lexers.get_lexer_by_name(block.lang)
    except ClassNotFound:
        return RstIssue(
            block.lineno,
            f'Pygments lexer name {block.lang!r} is not known in code-block.',
        )

    opts: list[str] = []
    body = block.lines
    if body and _OPTION_FIELD_RE.match(body[0].lstrip()):
        sep = body.index('') if '' in body else len(body)
        opts, body = body[:sep], body[sep + 1 :]

    # Sphinx's `:force:` option suppresses Pygments highlighting warnings for
    # blocks that intentionally contain invalid syntax or placeholders.
    if any(opt.strip() == ':force:' for opt in opts):
        return None

    code = textwrap.dedent('\n'.join(body)).strip('\n')
    if code:
        for tok, val in lexer.get_tokens(code):
            if tok in TokenError:
                return RstIssue(
                    block.lineno,
                    f'Lexing code-block as "{block.lang}" resulted in a '
                    f'Pygments error at token {val!r}.',
                )
    return None


def _check_code_blocks(text: str) -> list[RstIssue]:
    """Finds explicit code-block directives and lexes them with Pygments."""
    return [
        issue
        for block in _iter_code_blocks(text)
        if (issue := _lex_code_block(block)) is not None
    ]


def _parse_doctree(text: str) -> nodes.document:
    """Parses reST into a docutils AST without leaking custom `.. role::`s.

    `publish_doctree` parses `text` into a document tree and attaches any
    syntax warnings or errors inline as `nodes.system_message` elements.
    Because `.. role:: <name>` mutates `roles._roles` process-globally in
    docutils, snapshot and restore it around each parse.
    """
    saved_roles = roles._roles.copy()  # pylint: disable=protected-access
    try:
        return docutils.core.publish_doctree(
            source=text, settings_overrides=_DOCUTILS_SETTINGS
        )
    finally:
        roles._roles.clear()  # pylint: disable=protected-access
        roles._roles.update(saved_roles)  # pylint: disable=protected-access


def _collect_syntax_issues(doctree: nodes.document) -> Iterator[RstIssue]:
    """Extracts docutils WARNING/ERROR/SEVERE diagnostics from the AST."""
    for msg_node in doctree.findall(nodes.system_message):
        if msg_node.attributes['level'] < docutils.utils.Reporter.WARNING_LEVEL:
            continue
        # The first child holds the diagnostic message.
        summary = msg_node.children[0].astext() if msg_node.children else ''
        msg = ' '.join(summary.split())
        if msg and not _is_ignored_docutils_message(msg):
            lineno = docutils.utils.get_source_line(msg_node)[1] or 1
            yield RstIssue(lineno, msg)


def _collect_nested_inline_code(doctree: nodes.document) -> Iterator[RstIssue]:
    """Flags inline code (````) nested inside **bold** or *italic* spans.

    reST does not support nested inline markup, so ``**foo ``bar``**`` silently
    renders literal backticks in HTML without emitting a docutils warning.
    """
    for node in doctree.findall(
        lambda n: isinstance(n, (nodes.strong, nodes.emphasis))
    ):
        if '``' not in node.astext():
            continue
        kind = (
            'bold (**...**)'
            if isinstance(node, nodes.strong)
            else 'italic (*...*)'
        )
        lineno = docutils.utils.get_source_line(node)[1] or 1
        yield RstIssue(
            lineno,
            f'reST cannot nest inline code (````) inside {kind}; '
            'close emphasis before ```` and reopen after.',
        )


def check_rst_text(text: str) -> list[RstIssue]:
    """Checks a reST document string and returns sorted RstIssues."""
    if not _HAS_DEPS:
        return []

    doctree = _parse_doctree(text)
    return sorted(
        {
            *_collect_syntax_issues(doctree),
            *_collect_nested_inline_code(doctree),
            *_check_code_blocks(text),
        }
    )


@step(name='rst_check', endswith=('.rst',))
def presubmit_check(ctx: Context) -> None:
    """Presubmit check for reStructuredText syntax and code-block lexing."""
    if not _HAS_DEPS:
        return
    for path in ctx.paths:
        if path.is_symlink() or path.is_dir():
            continue
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError as exc:
            ctx.fail(f'Failed to decode UTF-8: {exc}', path=path)
            continue
        for issue in check_rst_text(text):
            ctx.fail(issue.message, path=path, line=issue.line)
