---
name: Gerrit
description: Helps with tasks related to Gerrit and Buildbucket.
---

# Gerrit & Buildbucket Tips

This document summarizes the tricks and methods discovered for interacting with
Gerrit and Buildbucket programmatically and via CLI in the Pigweed environment.

## 1. Querying Buildbucket Checks via API

Pigweed uses LUCI Buildbucket for checks. You can query the status of builds
for a specific CL and patchset without using the Gerrit UI.

### Search Builds

To query the status of all Buildbucket checks for a patchset (including handling
pagination and deduplicating retries to keep only the latest run for each
builder), use the provided Python helper script:

```bash
python3 .agents/skills/gerrit/scripts/search_builds.py <CHANGE_ID> <PATCHSET_ID>
```

Or with a Gerrit URL:

```bash
python3 .agents/skills/gerrit/scripts/search_builds.py <GERRIT_URL>
```

*   **GERRIT UI CHECK COUNT DISCREPANCY**: The total count of unique builders
    fetched from Buildbucket (e.g., ~72) will often be **less** than the total
    checks count displayed in the Gerrit UI (e.g., ~132). This is expected
    behavior. The Gerrit UI "Checks" tab aggregates top-level Buildbucket
    tryjobs *plus* individual sub-steps (like distinct tests run inside a
    `pw_presubmit` invocation reporting separately) and other robot/linter
    plugins (e.g., AyeAye linter comments, SLSA policy checks). The
    Buildbucket query focuses specifically on top-level infrastructure
    builders that can be individually triggered or retried via `bb add`.

### Get Build Details and Logs

To get specific logs for a build:

*   **Endpoint**:
    `https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/GetBuild`
*   **Method**: `POST`
*   **Body**:
    ```json
    {
      "id": "<BUILD_ID>",
      "fields": "steps"
    }
    ```
    *Note: Requesting the `steps` field returns `viewUrl` links to logs in
    LogDog for each step.*

---

## 2. Triggering Checks

### Prerequisites & Setup (`bb` CLI)

The `bb` CLI tool is used to interact with Buildbucket builders directly. Note
that `bb` is made ambiently available on `$PATH` by bootstrapping or activating
the Pigweed environment:

*   **Activate Pigweed environment**:
    ```bash
    source bootstrap.sh  # or: source activate.sh
    ```
*   **If `bb` is still not installed and `cipd` is available**, download it using
    `cipd export`:
    ```bash
    # e.g., platform can be mac-arm64, mac-amd64, linux-amd64
    echo 'infra/tools/bb/${platform} latest' | \
      cipd export -root /path/to/install -ensure-file -
    ```
*   **Authentication**: The user must run `bb auth-login` in their own
    interactive terminal first. Use `bb auth-info` to verify the active login
    state.

### Guidance: Workflow for Fixing and Retrying Checks

When addressing failed checks on a CL, follow this resource-efficient
workflow:

1.  **Targeted Verification (`bb add`)**: Initially, trigger **only the
    specific checks that failed** using the `bb` CLI to quickly verify the
    fix without wasting CI resources on the whole suite.
2.  **Full Validation (Full Dry Run)**: **Only after the targeted failed
    checks have passed successfully**, proceed to trigger a **full dry run**
    (all checks) via Gerrit API or push options. This ensures the complete
    suite is validated on the final code state as required by Gerrit for
    submission.

### Via `bb` CLI (Specific Builders)

To trigger a specific check manually after it has been scheduled or failed:

```bash
bb add -cl \
  https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/<CHANGE_ID>/<PATCHSET_ID> \
  pigweed/pigweed.try/<BUILDER_NAME>
```

Example:

```bash
bb add -cl \
  https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/404076/4 \
  pigweed/pigweed.try/pigweed-mac-arm-vscode
```

### Via Gerrit API (Full Dry Run / CQ Dry Run)

You can trigger all CQ checks (Dry Run) on the current patchset by setting
the `Commit-Queue` label to `1` via the Gerrit REST API using the user's local
git cookies:

```bash
curl -sb ~/.gitcookies -X POST \
  -H "Content-Type: application/json" \
  -d '{"labels": {"Commit-Queue": 1}}' \
  "https://pigweed-review.googlesource.com/a/changes/<CHANGE_ID>/revisions/current/review"
```

### Via Git Push Options (Full Dry Run on Upload)

You can trigger a dry run when pushing to Gerrit by setting the
`Commit-Queue` label:

```bash
git push origin HEAD:refs/for/main%l=Commit-Queue+1
```

Or using the `-o` option:

```bash
git push origin HEAD:refs/for/main -o l=Commit-Queue+1
```

---

## 3. Authentication & Troubleshooting

### API Authentication

*   **Gerrit JWT**: The endpoint `.../changes/<ID>/jwts` requires a
    logged-in session.
*   **Buildbucket API**: Requests to schedule builds require authentication.
    Attempting to use the Gerrit JWT directly resulted in a `signature check
    error: unknown signing key`.

### `bb` CLI Auth

*   The `bb` tool requires authentication via `bb auth-login`.
*   **Checking Status**: Use `bb auth-info` to verify if the active account
    details and OAuth scopes are valid.
*   **Crucial Limit**: `bb auth-login` **cannot** be run by the AI agent
    directly because it requires an interactive terminal (TTY) to proceed
    with the login flow.
*   **Solution**: The user must run `bb auth-login` in their own interactive
    terminal first. Once authenticated, the agent can use the `bb` CLI tool
    successfully.

---

## 4. Fetching and Responding to Review Comments

### Fetching Comments

To get open comments on a change:

*   **Endpoint**:
    `https://pigweed-review.googlesource.com/changes/<CHANGE_ID>/comments`
*   **Method**: `GET`
*   **Filter**: Look for comments with `"unresolved": true` and for the
    latest `patch_set`.

### Workflow for Comments

1.  Create a checklist file (e.g., `comments-<CHANGE_ID>.md`) with checkboxes
    for each unresolved comment.
2.  Include file name, line number, and author for each comment.
3.  Address comments one-by-one and confirm with the user before making
    changes.
4.  Mark as completed with a resolution description when addressed.

