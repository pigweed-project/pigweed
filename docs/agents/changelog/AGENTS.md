# Changelog update

This is the official process for updating the [Pigweed](https://pigweed.dev)
changelog.

## General guidelines

The goal of the changelog is to highlight how Pigweed is progressing
and evolving. We do not attempt to comprehensively cover every change.
Users can consult the Git commit log when they need that level of granularity.

The intended audience is a software engineer in a downstream project that
relies on Pigweed. Users should be able to enjoy reading a changelog
update during their morning coffee or commute.

Bug fixes should be ignored. Commits that only fix bugs should not have
stories created for them; assign them to the appropriate `misc` category
with a score of `0`.

Do not attempt to create scripts to speed up this process.
You must process commits in small batches, as specified in this document,
to ensure that each commit is properly analyzed.

### Debugging

Never attempt to run `python` or `python3` directly. Instead, create a
temporary target in `//docs/agents/changelog/scripts/BUILD.bazel` and use
`bazelisk run` to invoke your temporary Python script. Delete all of these
debugging artifacts when you're done with them.

## 1. Get the year and month

If the user hasn't specified a year and month, prompt them
to do so now. Do not guess.

In subsequent steps, `<YYYY>` should always be replaced with the
user-specified year and `<MM>` with the user-specified month.

## 2. Start

Run this command:

```
bazelisk run //docs/agents/changelog/scripts -- start --year=<YYYY> --month=<MM>
```

This command makes sure that data files are properly populated. If there
are errors, stop here and inform the user of the problem(s).

Before processing commits, read the 2 most recent published changelog files in
`//docs/sphinx/changelog/<YYYY>/` to calibrate your writing style, tone, story
conciseness, and code example density.

## 3. Get the next commits

Run this command:

```
bazelisk run //docs/agents/changelog/scripts -- next --year=<YYYY> --month=<MM>
```

Inspect the data that was written to
`//docs/agents/changelog/resources/next.json`.

If the `next` key is `null`, you have reached the
terminating condition. Proceed to Step 5 (Holistic review and `NOTES.md`).

```
{"next": null}
```

If the `next` key contains data, it will be a list of commits. Use these
commits in step 4.

## 4. Group the next commits into stories

For each commit in the `next` list, decide whether to:

1. Add the next commit to an existing story
2. Refactor an existing story into 2 or more stories, then add
   the next commit to one of these stories
3. Create a new story

Only add a commit to an existing story when the commits form a single coherent
user-facing feature or architectural goal. Never create grab-bag stories that
lump unrelated features together just because they touch the same module (for
example, avoid `"X, Y, and Z in pw_foo"` titles). Conversely, **do** merge
tightly coupled companion changes across related modules when they deliver one
unified end-to-end capability (for example, introducing a base status trait in
`pw_status` and teaching `pw_result` to carry it) rather than splitting one
narrative across redundant stories or highlights. If a commit introduces a
distinct feature, create a new story (or assign minor commits to `misc` with
`score = 0`).

Update `//docs/agents/changelog/resources/data.toml` with the new or
modified story.

Story example:

```
# stories

[stories.<category_id>.<story_id>]
title = "…"
body = """
…
"""
highlight = """
…
"""
example = """
…
"""
example_proof = """
…
"""
score = 600

[stories.<category_id>.<story_id>.commits."7818487b04e22ebbf2fc477c770c563e9fc0b5ed"]
summary = "…"

[stories.<category_id>.<story_id>.commits."4b8659b954abdbe97b97677d26105ac6b2eb14f7"]
summary = "…"

…
```

### `data.toml` reference

#### `<category_id>`

`<category_id>` is a placeholder for the category ID. Valid category IDs are the
**top-level** categories defined in `//docs/common/metadata/categories.toml`
(tables without a `parent` field), plus custom changelog-only categories defined
in `EXTRA_CATEGORIES` in `//docs/agents/changelog/scripts/common.py` (such as
`rust`).

* **Subcategory rollup**: Subcategories in
  `//docs/common/metadata/categories.toml` (tables with `parent = "..."`) are
  not used directly in `data.toml`; roll them up to their top-level ancestor
  (for example, `bluetooth` -> `comms` or `zephyr` -> `os`).
* **Module lookup**: Each Pigweed module's category is defined in
  `//<module>/module.toml` (`[category]` `id`, or via `[facade]` `module` for
  target-agnostic backends). Each commit in `next.json` also includes a
  `suggested_categories` field pre-resolved from `module.toml` and
  `categories.toml`.
* **Non-module paths**: Route `docs/` and `seed/` changes to `devtools`,
  `third_party/` rolls and build infrastructure to `build`, and `targets/`
  changes to `hw`.
* **Category precedence**: The `rust` category takes precedence over module
  categories (such as `test`, `async`, or `observability`) for any Rust crate or
  Rust-specific feature so that Rust-interested readers can find all Rust news
  in one place.

#### `<story_id>`

`<story_id>` is a placeholder for the story ID. This should be a small word
related to the main story idea.

#### `title`

`title` is a natural, scannable summary of the story in 60 characters or less.
It must be plaintext and name the capability and module/type (for example,
`"Reader-writer synchronization with pw::sync::SharedMutex"` or
`"Low-overhead packet framing with pw_cobs"`). Avoid awkward noun stacks or
clunky spelled-out acronym collisions (such as `"Consistent Overhead Byte
Stuffing framing in pw_cobs"`), and never list 3 or more unrelated items
(`"A, B, and C in pw_foo"`).

#### `body`

`body` is a short paragraph (1–4 sentences) in present tense explaining **what
changed, how it behaves, and why downstream engineers care**—not a catalog of
symbols. Name only the 1–2 primary entry points and focus on the problem the
change solves or the capability it unlocks.

Avoid falling back on repetitive `"``pw_foo`` adds/introduces/now provides…"` or
`"``pw_foo/bar.h`` adds…"` openings that treat module and header names as
actors. Instead, vary opening sentences naturally across:

* **Caller capability** (*"You can now implement and call RPCs asynchronously…"*
  or *"Host tools can now discover at runtime which RPC services a device
  supports…"*)
* **Developer friction removed** (*"Unit tests for ``pw_rpc2`` services no
  longer need to wire up sockets by hand. The new ``TestHarness`` fixture…"*)
* **Direct subject definition** (*"The new ``PW_ASSERT_NOTNULL(ptr)`` macro is a
  header- and ``constexpr``-safe counterpart to ``PW_CHECK_NOTNULL()``…"*)

Break lines at 80 characters and use double backticks (`` ``...`` ``) for inline
code:

* **Good** (natural subject opening; explains purpose, behavior, and value):
  ```
  body = """The new ``PW_ASSERT_NOTNULL(ptr)`` macro is a header- and
  ``constexpr``-safe counterpart to ``PW_CHECK_NOTNULL()`` in C++. It evaluates
  raw, function, or smart pointers once, crashes if the pointer equals
  ``nullptr``, and returns the argument with its original type and value
  category preserved."""
  ```
* **Bad** (personifies the module and lists methods/details without value):
  ```
  body = """``pw_cobs`` implements ``0x00``-delimited framing. ``cobs.h``
  provides ``Encode``, ``Decode``, ``EncodeBuffer``, ``EncodeInPlace``,
  ``Streamer``, ``StreamerBuffer``, ``Decoder``, and ``DecoderBuffer``, and
  Python provides ``encode``, ``decode``, ``Streamer``, and ``Decoder``."""
  ```

Avoid both bulleted and comma-separated inventories of helper methods, wrapper
variants, overloads, or CLI flags (especially methods already demonstrated in
`example`), omit header paths when `#include`s already appear in `example` (as
well as internal subpackage paths), and omit low-level wire or implementation
details unless callers need them to use the API.

Do **not** include Sphinx cross-reference roles (`:ref:`, `:doc:`, `:cc:`,
`:cpp:`, `:c:`, `:py:`) or inline hyperlinks in `body` or `highlight`. Always
use double-backtick inline code instead. The `end` command automatically
appends `CLs:` links and generates the top-of-page `Highlights` `:ref:` links.

#### `highlight`

`highlight` is a one-sentence summary of the story written as an active
imperative verb phrase (for example, `"Synchronize concurrent readers and
exclusive writers with the new ``pw::sync::SharedMutex`` primitive."`). This
may be presented at the top of the changelog update. `highlight` must not
redundantly repeat `title`, must not contain cross-reference roles or inline
links, and should wrap lines so the rendered `* :ref:... - ` bullet stays
within 80 characters.

#### `example` and `example_proof`

`example` is a code example showing how to use the new feature or API.
It must include the reStructuredText code-block directive and the code itself
must be indented by 3 spaces. For example:

```toml
example = """
.. code-block:: cpp

   int n = 5 + 6;
"""
example_proof = """Verified against pw_foo/public/pw_foo/bar.h:42 and compiled in //pw_foo:bar_test."""
```

Only include an `example` when it genuinely clarifies how to use the new API,
build rule, or CLI flag. If a story is self-explanatory without code, set
`example` to an empty string `""""""`.

Every non-empty `example` must be sourced from or verified against real in-tree
headers, unit tests, docs, or build targets—never invented from commit messages
alone. Whenever `example` is non-empty, you **must** also populate
`example_proof` with a concise note citing the exact file/line numbers or build
and test commands that prove the example is accurate (`next` rejects non-empty
`example` fields without `example_proof`).

See `//docs/agents/rst/AGENTS.md` for guidance on what language values (lexer
short names) to use for the `.. code-block:: <lang>` directive.

For `misc` stories, `example` must always be set to `""""""`.

#### `commits`

The `commits` object contains the primary commits for this story. Each top-level
key in the object is a commit SHA. The `summary` field is a 1-sentence summary
of the commit. Do not use the first line of the commit message as the summary.
Generate a summary based on the entire commit message and diff. This `summary`
field will not be displayed in the changelog, we only use it for "debugging"
purposes.

For rendered stories (`score > 250`), use judgment to keep the `CLs:` list
short and high-signal (typically **around 1–5 primary commits**)—attach only
the CLs that introduce or materially shape the user-facing feature. When a
feature lands alongside many incremental follow-up commits (such as build
target or visibility cleanups, comment/doc polish, or minor edge-case
adjustments), route those secondary commits to `stories.<category_id>.misc`
(`score = 0`). More than 5 commits is fine when every CL genuinely contributes
a distinct, substantive part of the feature rather than routine follow-up
churn.

#### `score`

`score` represents the importance of the story. See the "Scoring criteria"
section below. If needed, you can adjust the scores of other stories in order
to account for the new story. For example, if another story previously was
scored `750`, but now it looks like a `600`, you are welcome to change the
score for that other story to `600` and set the new story to `750`.

`score` must be an integer between `0` and `1000`. There will potentially
be many stories with a score of `0`. For every other number (`1` to `1000`)
there must only be one story with that score.

At most **5 visible stories** may have `score >= 750` (the threshold for
top-level `Highlights`). The `next` command enforces this cap, and `end` emits
at most 5 highlights.

Explanation of scoring ranges:

* `0`: The story has no user-facing impact. 0% of users are likely to be
  affected. Or, it's in one of the special categories (listed below)
  that technically has user-facing impact but which we have decided to omit.
  Examples:

  * An internal, non-public function was renamed.
  * Docs-only changes. (This technically has user-facing impact but we do not
    show documentation-only changes in the changelog.)
  * Test-only changes. (Pigweed is extensively tested. Covering every
    test-only change will generate a lot of noise.)
  * Trivial build system fixes. (Again, this technically has user-facing impact,
    but it's boring to read.)
  * Bug fixes. (Bug fixes should be ignored for changelog updates. Put any bug
    fix commits into the relevant `misc` story with a score of `0`.)

* `1` to `250`: The story has trivial impact on downstream projects. Less than
  10% of users are likely to be affected. Stories in this range are omitted
  from the rendered changelog—use it generously whenever a change feels too
  small or niche to warrant its own section. Examples:

  * An extra parameter with a default value was added to an unimportant
    function.
  * A minor feature, build-flag default flip, or feature-gate cleanup was
    applied to an experimental, unstable, or "work in progress" module or
    crate.
  * Base-class virtual-method or trait housekeeping that only affects custom
    subclass implementers.
  * A niche or target-specific backend was added to an auxiliary module (such
    as a baremetal cycle-counter backend for `pw_perf_test`).
  * A new set of documentation was added.

* `251` to `500`: The story has minor impact on downstream projects. 11% to 25%
  of users are likely to be affected. Examples:

  * A helper function was added.
  * A new configuration option was added to a commonly used module, but the
    default behavior remains unchanged.
  * A minor performance improvement was made to a utility function.

* `501` to `750`: The story has moderate impact on downstream projects. 26% to
  75% of users are likely to be affected. Examples:

  * A Hardware Abstraction Layer (HAL) module like `pw_i2c_rp2040` or a
    third-party integration module like `pw_thread_freertos` was created.
  * A moderately used public function or class is being deprecated.
  * A new backend implementation for an existing core facade was added.

* `751` to `1000`: The story has high impact on downstream projects. 76%
  or more of users are likely to be affected. Examples:

  * A core Pigweed module like `pw_kernel` or `pw_async2` was created.
  * An important function was added to an important Pigweed module.
  * A refactor of a Pigweed module has improved performance and resource usage
    by 100%.
  * A widely used API was changed in a breaking, non-backward-compatible way.
  * A fundamental, highly anticipated capability or paradigm was introduced.

### Story grouping guidelines

#### All commits must be related to the story

You must ensure that all commits associated to a story are actually related to
that story. Commit `5513af6b7366c3c3da1f8bcb90e0b81e7859f0aa` in the example below
should be moved elsewhere because it's not related to the overall story.

```
[stories.core.containers]
score = 625
title = "New pw::DynamicMap and pw::IntrusiveQueue classes"
body = """Introduced ``pw::DynamicMap``, a sorted map container that uses a
caller-provided ``pw::Allocator`` to dynamically allocate nodes, and
``pw::IntrusiveQueue``, a singly-linked list that tracks the tail element to
provide O(1) ``push_back`` support.
."""
highlight = """``pw::DynamicMap is a new sorted map container that supports
dynamic allocation, and ``pw::IntrusiveQueue is a new singly-linked list that
provides O(1) ``push_back`` support."""

# This commit should be moved elsewhere because it's not related to
# DynamicMap or IntrusiveQueue.

[stories.core.containers.commits.5513af6b7366c3c3da1f8bcb90e0b81e7859f0aa]
summary = """pw_containers: Merge CountAndCapacity into GenericDeque"""
date = "2026-03-10 13:15:15-07:00"
title = "pw_containers: Merge CountAndCapacity into GenericDeque"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/383938"

[stories.core.containers.commits.4dc4a90fc765e6b26013f0efd657ca4b24bbf330]
summary = """Add pw::DynamicMap."""
date = "2026-03-03 07:47:38-08:00"
title = "pw_containers: Add pw::DynamicMap"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/359892"

[stories.core.containers.commits.8fa442e7bab4ad60d5754b0473e0e14f34cc9c92]
summary = """pw_containers: Introduce IntrusiveQueue"""
date = "2026-03-24 00:20:58-07:00"
title = "pw_containers: Introduce IntrusiveQueue"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/388292"
```

#### Dump miscellaneous commits into `stories.<category_id>.misc`

When you encounter miscellaneous commits with 0 or low user-facing impact (such as
bug fixes, docs-only changes, test-only changes, or trivial build fixes), you
should dump them all into a `stories.<category_id>.misc` story, set the `score`
to `0`, and set `example` to `""""""`.

## 5. Holistic review and `NOTES.md`

Once all commits have been processed (`{"next": null}`), perform a holistic
review of `//docs/agents/changelog/resources/data.toml` against the 2 most
recent published changelogs in `//docs/sphinx/changelog/<YYYY>/`:

1. **Cohesion, merging, & pruning**: Verify that every story has a single
   unifying narrative. Split grab-bag stories (`"X, Y, and Z in pw_foo"`),
   merge tightly coupled companion stories that deliver one end-to-end
   capability across modules, and demote small, niche, or low-signal stories to
   `<= 250` or `stories.<category_id>.misc` (`score = 0`). Also check commits
   near UTC month boundaries or with older CL numbers against earlier files in
   `//docs/sphinx/changelog/` to confirm the feature was not already covered.
2. **Curate story CL lists**: Review any rendered story (`score > 250`) with a
   long `commits` list (for example, more than ~5 CLs) and use judgment to move
   secondary build, visibility, cleanup, or polish commits to
   `stories.<category_id>.misc` so `CLs:` links only to the primary changes.
3. **Category routing**: Verify that all Rust stories live under
   `stories.rust.*`.
4. **Highlights cap**: Verify that at most 5 visible stories have
   `score >= 750`, and merge overlapping highlights that describe two halves of
   the same workflow.
5. **Value-focused prose & links**: Verify that every `title` reads naturally
   (no awkward noun stacks), every `body` is a concise 1–4 sentence
   present-tense paragraph explaining purpose and user value rather than
   reciting an inventory of helper methods/classes/flags, every `highlight` is
   an active imperative verb phrase, and neither contains `:ref:`/`:cc:`
   cross-reference roles or inline links.
6. **Code examples & proofs**: Review every story to decide whether an
   `example` is needed: delete trivial or redundant examples, add examples
   where a story benefits from one, verify or compile every snippet against
   in-tree sources/tests, and ensure `example_proof` is populated for each.
   Re-run `next` to confirm all automated checks pass.
7. **Generate `NOTES.md`**: Create a temporary `NOTES.md` at the repository
   root summarizing the top highlights, borderline stories kept or cut, code
   example verification proofs, and any open questions for human review (to be
   deleted once the user signs off before committing).

## 6. End

Run this command:

```
bazelisk run //docs/agents/changelog/scripts -- end --year=<YYYY> --month=<MM>
```

This command transforms `data.toml` into reStructuredText. The transformed
content will be written to `//docs/sphinx/changelog/<YYYY>/<MM>.rst`.

## 7. Build and format the docs

Add the new `//docs/sphinx/changelog/<YYYY>/<MM>.rst` file to
`//docs/sphinx/changelog/BUILD.bazel` and `//docs/sphinx/changelog/index.rst`.

Check `//docs/sphinx/index.rst`. If the changelog update that you just created
is more recent than the update that the `docs-root-changelog` section is
pointing to, update the `include` and `ref` to point to the latest changelog
update.

Format the modified files:

```
./pw format --fix
```

Run this command:

```
bazelisk build //docs
```

If there are errors and they look related to the changelog update, fix them.

If there are unrelated errors, do not attempt to fix them. Inform the user and
stop.
