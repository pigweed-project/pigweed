# Docs skill tests

## Setup

1. Read `//.agents/skills/docs/SKILL.md`.
2. `cd` into the root directory of this repository. All tests should be
   run from the root directory.

## Test cases

### 1. `SKILL.md` has correct frontmatter

```console
cat .agents/skills/docs/SKILL.md
```

Verify:

* Frontmatter is delimited by `---` markers.
* YAML frontmatter contains `name` and `description` fields.
* `description` field value mentions all of the workflows
  described in the body of `SKILL.md`.

### 2. Workflow files exist

```console
ls docs/agents/changelog/AGENTS.md
ls docs/agents/rst/AGENTS.md
```

Verify:

* All files are found.

### 3. reST edits are correct

1. `cp .agents/docs/skills/tests/before.rst <number>.rst`

   Replace `<number>` with a small, random number.
2. Prompt: `fix the <number>.rst file in the root dir of the repo`
3. `diff <number>.rst .agents/docs/skills/tests/after.rst`

Verify:

* After the agent's edits, `after.rst` and `<number>.rst` have the
  exact same content.

### 4. Changelog process is followed

1. Prompt: `update the changelog for <month> <year>`

   Replace `<month>` and `<year>` with the current month.
2. Interrupt the agent after 30 to 60 seconds.

   > [!NOTE]
   >
   > The full changelog workflow usually takes 1 hour to complete.

Verify:

* Agent runs `bazelisk run //docs/agents/changelog/scripts -- start …`
  once.
* `//docs/agents/changelog/resources/data.toml` is created.
* Agent runs `bazelisk run //docs/agents/changelog/scripts -- next …`
  many times.
