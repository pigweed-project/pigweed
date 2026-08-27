# Testing Oncall Skill

This document provides test cases to verify that the `Oncall` skill and its
`roll_cipd` subskill (b/315378787) function correctly with manual slash
command triggering and disabled auto-load.

---

## Test Cases

### 1. Frontmatter Verification

Verify that `.agents/skills/oncall/SKILL.md` contains valid YAML frontmatter
specifying slash-command triggering:

```bash
head -n 8 .agents/skills/oncall/SKILL.md
```

**Verification:**
- Frontmatter begins and ends with `---`.
- Contains `name: oncall` and `description` instructing agents **not** to
  auto-load during normal tasks and to trigger on `/oncall` or `/roll-cipd`.

### 2. Subskill & Resource Files Exist

Verify all referenced files exist in the skill directory:

```bash
ls .agents/skills/oncall/SKILL.md
ls .agents/skills/oncall/resources/roll_cipd.md
ls .agents/skills/oncall/scripts/roll_cipd.py
```

### 3. Automated Script Verification (`--check` and `--dry-run`)

Test that the automated helper script executes cleanly:

```bash
# 1. Test check command
python3 .agents/skills/oncall/scripts/roll_cipd.py --check

# 2. Test dry-run calculation
python3 .agents/skills/oncall/scripts/roll_cipd.py --dry-run
```

**Expected Behavior:**
- `--check` verifies platform digests and performs an end-to-end CIPD client
  bootstrap & execution test, exiting with status 0.
- `--dry-run` discovers the latest available `git_revision` supported across
  platforms without modifying files.

### 4. Slash Command Triggering Tests

Test with the following prompts in fresh agent sessions:

1. **Prompt**: `/oncall`
   - **Expected Behavior**: Agent loads `oncall/SKILL.md` and displays the
     oncall task list and available subskills.
2. **Prompt**: `/roll-cipd` or `/oncall roll-cipd`
   - **Expected Behavior**: Agent triggers the CIPD rolling subskill, runs
     `roll_cipd.py` (or `wrapper.py selfupdate-roll`), verifies the digests,
     and prepares the commit formatted with `Bug: 315378787`.
3. **Negative Test (Unrelated prompt)**: "Fix indentation in pw_status/status.cc"
   - **Expected Behavior**: Agent does **NOT** load or mention the oncall
     skill or CIPD rolling workflow.
