# Subskill: Roll CIPD Client (b/315378787)

This subskill runbook describes how to roll, commit, and submit the CIPD
(Chrome Infra Package Deployer) client pin in Pigweed, addressing the recurring
oncall maintenance task tracked in [b/315378787](https://pwbug.dev/315378787).

---

## Invocation

This subskill is triggered via slash commands:
- **`/roll-cipd`**
- **`/oncall roll-cipd`**

---

## Overview

Pigweed bootstraps and pins the CIPD client binary across supported host
platforms (Linux, macOS, and Windows) using two files:
1. `pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version`: The pinned version
   string (must be an immutable `git_revision:<HASH>` tag).
2. `pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version.digests`: SHA-256
   binary digests for host platforms.

As part of the Pigweed oncall rotation, engineers periodically roll this pin to
incorporate upstream CIPD updates, bug fixes, and security patches.

---

## Triggers

Run this subskill when:
- The user enters slash commands: `/roll-cipd` or `/oncall roll-cipd`.
- The user explicitly asks to roll/update the CIPD pin during an oncall rotation.

## Guards

- **No Auto-load**: Do not auto-load or run this workflow unless explicitly
  triggered by slash command or direct user request.
- **Working Tree Cleanliness**: Ensure the git working tree is clean before
  starting.
- **Tag Validation**: NEVER use mutable tags like `latest` in `.cipd_version`.
  Always use an immutable `git_revision:<HASH>` verified on supported platforms
  (Linux, macOS, Windows).
- **Digests Matching**: Always verify that `.cipd_version.digests` matches
  `.cipd_version` before committing.

---

## End-to-End Workflow (Roll, Commit, and Submit)

### Method 1: Automated Update & Commit (Recommended)

Run the dedicated oncall helper script:

```bash
# 1. Roll version, verify digests, and create the commit:
python3 .agents/skills/oncall/scripts/roll_cipd.py --commit

# 2. (Optional) Automatically submit to Gerrit:
python3 .agents/skills/oncall/scripts/roll_cipd.py --commit --push
```

The script automatically:
1. Queries candidate `git_revision` tags across supported host platforms
   (`linux-amd64`, `linux-arm64`, `mac-amd64`, `mac-arm64`, `windows-amd64`,
   `windows-arm64`).
2. Identifies the newest `git_revision` supported on all platforms.
3. Executes `cipd selfupdate-roll` to update `.cipd_version` and
   `.cipd_version.digests`.
4. Runs `selfupdate-roll ... -check` to verify platform hashes.
5. Verifies client execution and package installation in a sandbox.
6. Stages modified files and creates the commit titled `pw_env_setup: Roll cipd`
   with `Bug: 315378787`.
7. Submits the commit to Gerrit via `git push origin HEAD:refs/for/main%ready`
   if `--push` is passed.

---

### Method 2: Manual Update, Commit, and Push

If performing the steps manually:

1. **Find available tags**:
   Query the latest tags for supported packages:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py describe \
       infra/tools/cipd/linux-amd64 -version latest
   ```
   Check that the selected `git_revision:<HASH>` is also available on other
   supported platforms (e.g., `mac-arm64`, `windows-amd64`).

2. **Roll the version and regenerate digests**:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py selfupdate-roll \
       -version-file pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version \
       -version git_revision:<HASH>
   ```

3. **Verify the digests**:
   ```bash
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py selfupdate-roll \
       -version-file pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version \
       -check
   ```

4. **Verify rolled client functionality**:
   ```bash
   # Verify client version and binary execution
   python3 pw_env_setup/py/pw_env_setup/cipd_setup/wrapper.py -version

   # Or run automated end-to-end functionality check
   python3 .agents/skills/oncall/scripts/roll_cipd.py --check
   ```

5. **Create the commit**:
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

6. **Submit to Gerrit**:
   ```bash
   git push origin HEAD:refs/for/main%ready
   ```
