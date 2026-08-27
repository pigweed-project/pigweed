---
name: oncall
description: >-
  Pigweed oncall rotation runbooks and maintenance workflows (such as rolling
  CIPD client tools for b/315378787). DO NOT auto-load during normal coding
  tasks. Only load when explicitly triggered by the user via the `/oncall` or
  `/roll-cipd` slash command.
---

# Pigweed Oncall Workflows

This skill provides operational workflows and runbooks for the Pigweed oncall
rotation. It is designed to be **manually triggered via slash commands** to
prevent unnecessary auto-loading.

---

## Invoking this Skill

Trigger this skill and its subskills explicitly using slash commands in the
prompt:

- **`/oncall`**: Access the general oncall workflow, overview, and task router.
- **`/oncall roll-cipd`** or **`/roll-cipd`**: Trigger the CIPD client update
  subskill ([b/315378787](https://pwbug.dev/315378787)), which rolls the pin,
  creates a `pw_env_setup: Roll cipd` commit, and submits it.
- **`/oncall presubmit`**: Run presubmit verification on your branch or stack.

---

## Oncall Responsibilities & Subskills

| Subskill / Command | Description | Runbook / Tool |
| :--- | :--- | :--- |
| **`/roll-cipd`** | Update pinned CIPD version, create `pw_env_setup: Roll cipd` commit, and submit to Gerrit ([b/315378787](https://pwbug.dev/315378787)) | [roll_cipd.md](file:///Users/hoangmle/pigweed/.agents/skills/oncall/resources/roll_cipd.md) / [roll_cipd.py](file:///Users/hoangmle/pigweed/.agents/skills/oncall/scripts/roll_cipd.py) |
| **`/oncall presubmit`** | Run presubmit checks across commit stacks | [Run Presubmit Checks](file:///Users/hoangmle/pigweed/.agents/skills/run_presubmit_checks/SKILL.md) |
| **`/oncall review`** | Review incoming and ongoing Git/Gerrit patches | [Code Review](file:///Users/hoangmle/pigweed/.agents/skills/code_review/SKILL.md) |

---

## Subskill: Roll CIPD Client (b/315378787)

CIPD (Chrome Infra Package Deployer) is used by Pigweed to install and manage
prebuilt toolchains and binaries. Updating the CIPD client pin is a recurring
oncall maintenance task tracked under
[b/315378787](https://pwbug.dev/315378787).

### Triggers
- Explicit user slash commands: `/roll-cipd` or `/oncall roll-cipd`.
- Explicit user prompt during oncall rotation specifically requesting CIPD pin
  roll.

### Guards
- **Do not auto-load** on general tool, environment, or setup tasks unless
  explicitly requested.
- Do **not** use the mutable `latest` ref in `.cipd_version` (it causes
  selfupdate validation failures).
- Always ensure all platform digests in `.cipd_version.digests` are generated
  and verified.

### Execution Workflow

#### Option 1: Automated Roll, Commit & Push (Recommended)

Run the oncall helper script:

```bash
# Roll and commit
python3 .agents/skills/oncall/scripts/roll_cipd.py --commit

# Roll, commit, and submit to Gerrit
python3 .agents/skills/oncall/scripts/roll_cipd.py --commit --push
```

#### Option 2: Manual Roll, Commit & Push

1. Determine candidate `git_revision` tags:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py describe \
       infra/tools/cipd/linux-amd64 -version latest
   ```
2. Roll version and update digests:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py selfupdate-roll \
       -version-file pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version \
       -version git_revision:<HASH>
   ```
3. Check and verify:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py selfupdate-roll \
       -version-file pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version \
       -check
   ```
4. Verify client functionality:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py -version
   ```
5. Commit changes:
   ```bash
   git add pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version \
       pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version.digests
   git commit -m "$(cat <<'EOF'
pw_env_setup: Roll cipd

Rolls CIPD client version to <git_revision>.

Bug: 315378787
EOF
)"
   ```
6. Submit to Gerrit:
   ```bash
   git push origin HEAD:refs/for/main%ready
   ```

For full details and troubleshooting, see the
[roll_cipd.md](file:///Users/hoangmle/pigweed/.agents/skills/oncall/resources/roll_cipd.md) runbook.
