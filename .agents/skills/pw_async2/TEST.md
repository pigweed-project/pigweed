# pw_async2 skill tests

## Setup

1. Read `//.agents/skills/pw_async2/SKILL.md`.
2. `cd` into the root directory of this repository. All tests should be
   run from the root directory.

## Test cases

### 1. `SKILL.md` has correct frontmatter

```console
cat .agents/skills/pw_async2/SKILL.md
```

Verify:

- Frontmatter is delimited by `---` markers.
- YAML frontmatter contains `name` and `description` fields.
- `description` field value mentions all of the workflows
  described in the body of `SKILL.md`.

### 2. Workflow files exist

```console
ls pw_async2/agents/references/*.md
```

Verify:

- Files for channels, coroutines, and testing exist.

### 3. Check async2 code is idiomatic

Prompt: Ask the agent to write an async2 library, e.g. for a hardware
peripheral. Ideally as a middleware layer so that everything isn't just a
ValueFuture directly.

Verify:

- All user-facing APIs return futures.
- Every future definition satisfies the `Future` concept.
- There is no coroutine-based code at all.
- Pendable functions either do not exist or are limited to one or two small,
  low-level internal helpers.
- Tests correctly use a DispatcherForTest and step it through wakes.
