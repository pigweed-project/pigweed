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

## 3. Get the next commits

Run this command:

```
bazelisk run //docs/agents/changelog/scripts -- next --year=<YYYY> --month=<MM>
```

Inspect the data that was written to
`//docs/agents/changelog/resources/next.json`.

If the `next` key is `null`, you have reached the
terminating condition. Proceed to Step 5 (End).

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

Strongly prefer option 1 or 2. Only pick option 3 as a last resort, when
you are certain that the commit is not related to any of the existing stories.

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
score = 600

[stories.<category_id>.<story_id>.commits."7818487b04e22ebbf2fc477c770c563e9fc0b5ed"]
summary = "…"

[stories.<category_id>.<story_id>.commits."4b8659b954abdbe97b97677d26105ac6b2eb14f7"]
summary = "…"

…
```

### `data.toml` reference

#### `<category_id>`

`<category_id>` is a placeholder for the category ID. Valid category IDs are
defined in `//docs/agents/changelog/resources/categories.json5`.

#### `<story_id>`

`<story_id>` is a placeholder for the story ID. This should be a small word
related to the main story idea.

#### `title`

`title` is a summary of the story in 60 characters or less. It must be plaintext.

#### `body`

`body` is a short paragraph explaining the story in more detail.
Break lines at 80 characters. Use reStructuredText formatting, e.g.
double backticks for inline code. Example:

```
body = """Introduced ``pw::DynamicMap``, a sorted map container that uses a caller-provided
``pw::Allocator`` to dynamically allocate nodes."""
```

#### `highlight`

`highlight` is a one-sentence summary of the story. This may be presented
at the top of the changelog update. `highlight` must not redundantly repeat
the information provided in `title`.

#### `commits`

The `commits` object contains all commits related to this story. Each top-level
key in the object is a commit SHA. The `summary` field is a 1-sentence summary
of the commit. Do not use the first line of the commit message as the summary.
Generate a summary based on the entire commit message and diff. This `summary`
field will not be displayed in the changelog, we only use it for "debugging"
purposes.

#### `score`

`score` represents the importance of the story. See the "Scoring criteria"
section below. If needed, you can adjust the scores of other stories in order
to account for the new story. For example, if another story previously was
scored `750`, but now it looks like a `600`, you are welcome to change the
score for that other story to `600` and set the new story to `750`.

`score` must be an integer between `0` and `1000`. There will potentially
be many stories with a score of `0`. For every other number (`1` to `1000`)
there must only be one story with that score.

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

* `1` to `250`: The story has trivial impact on downstream projects. Less than
  10% of users are likely to be affected. Examples:

  * An extra parameter with a default value was added to an unimportant
    function.
  * A minor feature or bug fix was applied to an experimental or "work in
    progress" module.
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

  * A critical bug was fixed.
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
[stories.cpp.containers]
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

[stories.cpp.containers.commits.5513af6b7366c3c3da1f8bcb90e0b81e7859f0aa]
summary = """pw_containers: Merge CountAndCapacity into GenericDeque"""
date = "2026-03-10 13:15:15-07:00"
title = "pw_containers: Merge CountAndCapacity into GenericDeque"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/383938"

[stories.cpp.containers.commits.4dc4a90fc765e6b26013f0efd657ca4b24bbf330]
summary = """Add pw::DynamicMap."""
date = "2026-03-03 07:47:38-08:00"
title = "pw_containers: Add pw::DynamicMap"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/359892"

[stories.cpp.containers.commits.8fa442e7bab4ad60d5754b0473e0e14f34cc9c92]
summary = """pw_containers: Introduce IntrusiveQueue"""
date = "2026-03-24 00:20:58-07:00"
title = "pw_containers: Introduce IntrusiveQueue"
url = "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/388292"
```

#### Dump miscellaneous commits into `stories.<category_id>.misc`

When you encounter miscellaneous commits with 0 or low user-facing impact,
you should dump them all into a `stories.<category_id>.misc` story and set the
`score` to `0`.

## 5. End

Run this command:

```
bazelisk run //docs/agents/changelog/scripts -- end --year=<YYYY> --month=<MM>
```

This command transforms `data.toml` into reStructuredText. The transformed
content will be written to `//docs/sphinx/changelog/<YYYY>/<MM>.rst`.

## 6. Build the docs

Add the new `//docs/sphinx/changelog/<YYYY>/<MM>.rst` file to
`//docs/sphinx/changelog/BUILD.bazel`.

Check `//docs/sphinx/index.rst`. If the changelog update that you just created
is more recent than the update that the `docs-root-changelog` section is
pointing to, update the `include` and `ref` to point to the latest changelog
update.

Run this command:

```
bazelisk build //docs
```

If there are errors and they look related to the changelog update, fix them.

If there are unrelated errors, do not attempt to fix them. Inform the user and
stop.
