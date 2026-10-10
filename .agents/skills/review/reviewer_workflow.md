# Reviewer workflow

Shared workflow for every language reviewer subagent dispatched by `/review`
(`.agents/skills/review/SKILL.md`, the coordinator). Your agent definition
names the language and the canonical guides; this file defines how to review
and how to report. You have read-only file access: you do not run commands,
edit files, or post to Gerrit.

## Workflow

1. **Read the guides at review time.** Read the review checklist in
   `docs/sphinx/code_reviews.rst` (the universal aspects and your language's
   row of the language table) and every canonical guide listed in your agent
   definition, under the checkout path the coordinator gave you. Do not rely
   on memory of their contents.
2. **Read your patch.** The coordinator gave you a patch file containing only
   your files. Review that patch; do not look for other changes.
3. **Review the changed lines only.** Read surrounding lines of a file under
   the checkout path when needed to judge a hunk, but do not report on
   unchanged code unless it is directly broken by the change. Mark anything
   outside the diff as `scope: out-of-diff`; the coordinator reports those
   only in the review summary, never as inline comments.
4. **Be exhaustive within scope.** Flag every violation of the checklist and
   the guides, however small. Do not stop at the first few.
5. **Verify before you cite.** Quote the sentence from the guide you read in
   step 1 and link its anchor. If you cannot find a rule that supports a
   finding, label it `nit` and say it is a judgment call.
6. **Return findings and nothing else.** No preamble, no restated diff.

## Finding format

One entry per finding, in this exact shape:

```
- file: <path>
  line: <line in the new file, or 0 for file-level>
  severity: blocking | should-fix | nit
  tag: bug | embedded/size | api | build | test   (omit for nit)
  scope: in-diff | out-of-diff
  rule: <URL, see below; omit for a judgment-call nit>
  quote: <the sentence from the guide that the change violates>
  issue: <one or two sentences describing the problem>
  fix: |
    <compliant example, as code>
```

Severity guide:

- `blocking` -- correctness, data loss, security, or a contract violation
  that is silently tolerated.
- `should-fix` -- a style-guide rule that the change violates.
- `nit` -- readability or consistency with no rule behind it.

Scope is independent of severity. `out-of-diff` findings are never counted
against the verdict.

## Mapping to `/review` severity prefixes

The coordinator reports every finding with the severity prefixes defined in
`.agents/skills/review/SKILL.md` ("Comment Calibration & Anti-Slop Rules").
Pick the `tag:` that names the dimension the rule belongs to:

| `tag:` | Use for |
| --- | --- |
| `bug` | Correctness, memory safety, races, lifetimes, silent failures, swallowed or unchecked errors |
| `embedded/size` | Heap allocation in a no-alloc module, large stack frames, ISR-unsafe calls, code-size bloat |
| `api` | Public header hygiene, C++17 compatibility, naming, namespaces, macros, Doxygen or API docs |
| `build` | `BUILD.bazel` / `BUILD.gn` / `CMakeLists.txt` parity, visibility, deps |
| `test` | Missing or weak tests, wrong status assertions, missing `PW_NC_TEST` |

The coordinator renders a `blocking` or `should-fix` finding as `[<tag>]
<issue> (<rule>)` and a `nit` finding as `nit: <issue>`. Any `blocking` or
`should-fix` finding with `scope: in-diff` makes the verdict `NEEDS_FIXES`;
nits never do, and the coordinator keeps at most three of them.

## Rule URLs

| Guide | URL form |
| --- | --- |
| `docs/sphinx/style/<page>.rst` | `https://pigweed.dev/style/<page>.html#<rst label>` |
| other `docs/sphinx/<page>.rst` | `https://pigweed.dev/<page>.html#<rst label>` |
| Rust API Guidelines | `https://rust-lang.github.io/api-guidelines/<page>.html#<anchor>` |
| Google style guides | `https://google.github.io/styleguide/<guide>.html#<anchor>` |

The anchor is the `.. _label:` line above the section with every `_`
replaced by `-` (Sphinx normalizes ids). For example, the label
`docs-code_reviews-checklist-no-silent-failures` is cited as
`https://pigweed.dev/code_reviews.html#docs-code-reviews-checklist-no-silent-failures`.

If there are no findings, return exactly: `No findings.`
