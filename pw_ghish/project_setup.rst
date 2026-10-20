.. _module-pw_ghish-project-setup:
.. _module-pw_ghish-project-integration:

=============
Project setup
=============
.. pigweed-module-subpage::
   :name: pw_ghish

.. warning::

   **NOT READY FOR EXTERNAL PROJECTS**: ``pw_ghish`` is currently
   **experimental for upstream Pigweed** and is **not ready for adoption
   outside Pigweed yet**. Several Pigweed-specific assumptions still exist in
   the codebase while multi-project abstractions are being completed. See
   :ref:`module-pw_ghish-roadmap` for current status and planned work to
   decouple project policies.

This page documents the build targets, repository wrapper pattern, and
``ProjectProfile`` architecture used to configure ``gh-ish`` for Gerrit and
LUCI repositories.

.. note::

   Looking to configure your personal AI coding assistant (Antigravity/Jetski,
   Claude Code, Codex, Cursor, or OpenCode) or install local pre-run tool
   hooks? See :ref:`module-pw_ghish-agent-setup`.

--------------------------------
Building and distributing gh-ish
--------------------------------
``gh-ish`` is written in standard Go without external C library dependencies.
It can be built and distributed using Bazel, Go toolchains, or pre-built binary
packages.

Building with Bazel
===================
If your project uses Bazel, compile the binary target directly:

.. code-block:: console

   $ bazelisk build //pw_ghish:gh-ish

The compiled executable is placed in ``bazel-bin/pw_ghish/gh-ish_/gh-ish``.

Building with standard Go
=========================
You can compile and install ``gh-ish`` using the Go toolchain:

.. code-block:: console

   # Compile binary from source:
   $ go build -o gh-ish ./pw_ghish/cmd/main.go

   # Or install directly into $GOPATH/bin:
   $ go install pigweed.dev/pw_ghish/cmd@latest

Distributing via CIPD (``pw_env_setup``)
========================================
Pigweed publishes pre-built ``gh-ish`` binaries to CIPD under
`pigweed/ghish/${platform} <https://chrome-infra-packages.appspot.com/p/pigweed/ghish>`_
(currently ``linux-amd64`` and ``mac-arm64``) and includes the package in
``pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json`` (which is included by
``default.json``).

For downstream projects using ``pw_env_setup`` (such as GN or CMake projects
that run ``source bootstrap.sh``), the CIPD pre-built binary is automatically
downloaded into ``environment/cipd/packages/pigweed/gh-ish`` and updated
whenever the Pigweed submodule rolls. Projects with custom CIPD manifests can
also add the package directly:

.. code-block:: json

   {
     "path": "pigweed/ghish/${platform}",
     "platforms": [
       "linux-amd64",
       "mac-arm64"
     ],
     "tags": [
       "git_revision:57b4f2eff500996a9a2f5a9c1e4b6b75539293e1"
     ]
   }

-----------------------------
Creating a repository wrapper
-----------------------------
Rather than requiring every developer and AI coding agent to manually install
and update a global binary, we recommend placing a wrapper script or symlink
named ``gh`` at the root of your repository (``./gh``).

Why use a repository wrapper?
=============================
1. **No global installation step**: Contributors and coding agents can run
   ``./gh pr list`` or ``./gh pr status`` directly from a checkout.
2. **Zero Bazel overhead for GN/CMake projects**: When backed by the CIPD
   pre-built package, ``./gh`` executes directly from
   ``environment/cipd/packages/`` without invoking Bazel or downloading a Go
   toolchain.
3. **Automatic updates**: Both the CIPD pre-built pin in ``pigweed.json`` and
   the ``pigweed/gh`` script roll automatically with Pigweed (and projects that
   check in a standalone ``./gh`` wrapper script can request an automated LUCI
   roller to keep it current).
4. **Standard CLI entry point**: Coding agents can invoke ``./gh`` from the
   checkout root using standard GitHub CLI subcommands even in non-interactive
   subshells where ``activate.sh`` has not been sourced.

Option A: Symlinking ``pigweed/gh`` (submodule projects)
========================================================
If your project includes Pigweed as a Git submodule (e.g. at ``pigweed/`` or
``third_party/pigweed/``) and initializes it during checkout, you can symlink
``./gh`` directly to ``pigweed/gh``:

.. code-block:: console

   $ ln -s pigweed/gh gh

``pigweed/gh`` resolves symlinks via ``readlink -f``, sets
``BUILD_WORKSPACE_DIRECTORY`` to the caller's superproject root (so
``.ghish.toml`` and Git operations target your project rather than the
``pigweed/`` submodule), and automatically prefers the CIPD pre-built binary in
``environment/cipd/packages/`` when invoked from a downstream repository.

Option B: Standalone CIPD-first ``./gh`` wrapper script
=======================================================
If ``pigweed/`` is an optional submodule or your repository uses ``pw_env_setup``
without always initializing ``pigweed/`` in shallow checkouts, check in a small
CIPD-first wrapper script at ``./gh`` that executes the CIPD pre-built binary
and falls back to ``pigweed/gh``:

.. code-block:: bash

   #!/bin/bash
   set -e

   _REPO_ROOT="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

   for _LUCI_DIR in \
     "${PW_ENVIRONMENT_ROOT:-}/cipd/packages/luci" \
     "${_REPO_ROOT}/environment/cipd/packages/luci" \
     "${_REPO_ROOT}/pigweed/environment/cipd/packages/luci"; do
     if [ -n "$_LUCI_DIR" ] && [ -d "$_LUCI_DIR" ]; then
       export PATH="${_LUCI_DIR}:${PATH}"
       break
     fi
   done

   export BUILD_WORKSPACE_DIRECTORY="${BUILD_WORKSPACE_DIRECTORY:-${_REPO_ROOT}}"
   export BUILD_WORKING_DIRECTORY="${PWD}"

   if [ -z "${PW_GH_FORCE_BUILD:-}" ]; then
     for _CANDIDATE in \
       "${PW_GH_BIN:-}" \
       "${PW_ENVIRONMENT_ROOT:-}/cipd/packages/pigweed/gh-ish" \
       "${_REPO_ROOT}/environment/cipd/packages/pigweed/gh-ish" \
       "$(command -v gh-ish 2>/dev/null || true)"; do
       if [ -n "$_CANDIDATE" ] && [ -x "$_CANDIDATE" ]; then
         exec "$_CANDIDATE" "$@"
       fi
     done
   fi

   if [ -x "${_REPO_ROOT}/pigweed/gh" ]; then
     exec "${_REPO_ROOT}/pigweed/gh" "$@"
   fi

   echo "Error: gh-ish binary not found. Run 'source bootstrap.sh' first." >&2
   exit 1

------------------------------------------------
Sharing Agent Skills & Hooks in Downstream Repos
------------------------------------------------
To give AI coding agents (Antigravity/Jetski, Gemini CLI, Claude Code) the
``ghish`` skill in a downstream repository without copy-pasting or manually
syncing ``SKILL.md``:

1. **Symlink the skill directory into your repository's** ``.agents/skills/``:

   .. code-block:: console

      $ mkdir -p .agents/skills
      $ ln -s ../../pigweed/.agents/skills/ghish .agents/skills/ghish

   Because the symlink points into the ``pigweed/`` submodule, the skill
   instructions automatically stay in sync whenever Pigweed rolls. Projects
   that vendor ``.agents/skills/ghish`` directly instead of symlinking into a
   ``pigweed/`` submodule can also request an automated LUCI roller to keep the
   skill directory current.

2. **Install Git and agent hooks during environment setup** (e.g. in
   ``.agents/setup.sh`` or ``bootstrap.sh``):

   .. code-block:: bash

      if [ -x "$REPO_ROOT/gh" ]; then
        "$REPO_ROOT/gh" hook install --agent=jetski --git >/dev/null 2>&1 || true
      fi

3. **Reference** ``./gh`` **and project-specific commit rules in** ``AGENTS.md``
   **or** ``GEMINI.md`` at your repository root (such as required commit
   trailers and ``./gh pr create`` / ``./gh pr push`` workflows).

-----------------------------------------------------------------------
Declarative Repository Configuration (``.ghish.toml`` & ``git config``)
-----------------------------------------------------------------------
Repositories can configure ``pw_ghish`` declaratively without modifying Go source
code by checking in a ``.ghish.toml`` file at the repository root (or in
subdirectories for monorepo overrides) and/or setting ``git config ghish.*``
keys locally.

Configuration hierarchy & strict validation
===========================================
``pw_ghish`` loads and merges configuration in the following order (later
sources override earlier ones):

1. Built-in defaults.
2. ``.ghish.toml`` at the Git repository root (``git rev-parse --show-toplevel``).
3. Any subdirectory ``.ghish.toml`` files along the path from the repository
   root to the current working directory.
4. Local or global ``git config ghish.*`` settings.
5. Explicit CLI flags (such as ``--host``, ``--all-projects``, ``--all``).

All ``.ghish.toml`` files and ``git config ghish.*`` keys are strictly
validated: syntax errors, unknown sections, unknown keys, or invalid types fail
immediately with a descriptive ``file:line`` error.

Example ``.ghish.toml``
=======================
.. code-block:: toml

   [gerrit]
   # Default Gerrit host, project, remote, and upstream branch for this repository:
   host = "pigweed-review.googlesource.com"
   project = "pigweed/pigweed"
   remote = "origin"
   default_branch = "main"

   # Scope `gh pr list` to this Gerrit project by default (pass --all-projects to override):
   scope_list_to_project = true

   # Submodule safety policy ("allow", "warn-unpushed", "require-pushed", or "forbid-manual-rolls"):
   submodule_policy = "warn-unpushed"

   # Reject --topic / -o topic=... before git push on projects with Topics-Not-Supported:
   forbid_topics = false

   # Custom shortlink prefixes accepted by all commands (e.g. `gh pr view myrev/12345`):
   [gerrit.shortlinks]
   "myrev" = "myproject-review.googlesource.com"

   [ci]
   # CI providers used by this project ("auto", "buildbucket", "busytown", "gerrit"):
   # - ["auto"] (default): Uses Buildbucket, and automatically enables Busytown + Buildbucket
   #   composite mode on Android Gerrit hosts or CLs with TreeHugger /
   #   Presubmit-Ready.
   # - ["busytown"]: Android Busytown / TreeHugger only (treetop~ + ci.android.com).
   # - ["buildbucket", "busytown"]: Dual-stack LUCI Buildbucket + Android Busytown.
   providers = ["auto"]
   buildbucket_host = "cr-buildbucket.appspot.com"
   try_buckets = ["pigweed/try"]

   # Hide child subbuilds matching these Buildbucket tags unless --all is passed:
   hide_tag_filters = ["hide-in-gerrit:subbuild"]

   # Skip retrying child subbuilds on `gh run rerun --failed` (retried by parent orchestrator):
   skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]

   # Ordered list of step log streams to prefer in `gh run view --log-failed`:
   preferred_logs = ["failure summary", "stdout", "stderr"]

   # Include top-level build SummaryMarkdown in failure reports:
   include_summary_markdown = true

   # Project-specific local presubmit command shown in hints:
   local_presubmit_hint = "./pw presubmit"

   [issue]
   # Issue tracker system ("buganizer"), prefix ("b/"), and optional host override:
   system = "buganizer"
   prefix = "b/"
   host = "issuetracker.googleapis.com"

   # Default Buganizer component ID and GCP quota project:
   default_component = 1194524
   quota_project = "my-gcp-quota-project"

   # Commit trailer template written by `gh issue create --amend` / `--commit`:
   trailer_format = "Bug: b/{id}"

   # Inspect nearest OWNERS files for `# COMPONENT: <id>` or `# Buganizer component: <id>`:
   use_owners_components = true

   # Path-prefix or glob mappings to Buganizer component IDs (longest match wins):
   [issue.path_components]
   "pw_bluetooth_sapphire/" = 1456789
   "pw_kernel/" = 1456790

   [worktree]
   # Prefix for warm physical worktree slots (`~/wrk/slots/<slot_prefix>01..N`):
   slot_prefix = "pw-"

   # Build cache warmup driver ("bazel" or "none"; auto-detected if omitted):
   warmup_driver = "bazel"

   [oncall]
   # Optional rotation schedule JSON file used by `gh sense`:
   schedule_file = ".ghish/oncall.json"

Overriding settings via ``git config``
======================================
Any ``.ghish.toml`` setting can be configured or overridden locally via
``git config``:

.. code-block:: console

   # Configure Gerrit remote and default upstream branch:
   $ git config --local ghish.gerrit.remote "goog"
   $ git config --local ghish.gerrit.defaultBranch "main"

   # Enforce submodule policy ("allow", "warn-unpushed", "require-pushed", "forbid-manual-rolls"):
   $ git config --local ghish.gerrit.submodulePolicy "forbid-manual-rolls"

   # Configure CI providers (e.g. "busytown" or "buildbucket,busytown") and hidden subbuild tags:
   $ git config --local ghish.ci.providers "busytown"
   $ git config --local ghish.ci.hideTagFilters "hide-in-gerrit:subbuild"

   # Register a custom Gerrit shortlink prefix:
   $ git config --local ghish.gerrit.shortlink.myrev "myproject-review.googlesource.com"

   # Configure Buganizer component, path mapping, and trailer format:
   $ git config --local ghish.issue.defaultComponent 1194524
   $ git config --local ghish.issue.pathComponents.pw_kernel/ 1456790
   $ git config --local ghish.issue.trailerFormat "Bug: b/{id}"

   # Set a custom local presubmit hint:
   $ git config --local ghish.ci.localPresubmitHint "fx test"

----------------------------
Project Profile Architecture
----------------------------
Every Gerrit project has unique review conventions: differing label names
(such as ``Code-Review`` vs. ``Commit-Queue`` vs. ``Presubmit-Ready`` vs.
``Auto-Submit``), varying presubmit gates, and dedicated LUCI build buckets.

``pw_ghish`` combines automatic Gerrit server introspection with **Project
Profiles** and ``.ghish.toml`` configuration.

Built-in profiles
=================
``pw_ghish`` includes built-in profiles for common Google open-source projects:

.. list-table::
   :header-rows: 1

   * - Profile
     - Gerrit Host Match
     - Commit-Queue
     - Code-Review
     - Try Bucket
   * - **pigweed**
     - ``pigweed-review.googlesource.com``
     - ``Commit-Queue+2``
     - ``Code-Review+2``
     - ``pigweed/try``
   * - **fuchsia**
     - ``fuchsia-review.googlesource.com``
     - ``Commit-Queue+2``
     - ``Code-Review+2``
     - ``fuchsia/try``
   * - **generic**
     - *(fallback)*
     - *(none)*
     - ``Code-Review+2``
     - *(custom)*

Auto-submit labels (such as ``Pigweed-Auto-Submit`` or ``Auto-Submit``) do not
need to be configured in a profile: ``--auto`` queries Gerrit for the change's
or project's labels and votes the matching auto-submit label automatically.

Automatic profile detection
===========================
``pw_ghish`` automatically selects the appropriate profile by inspecting:

1. The Git remote URL (e.g. ``origin``) for the repository.
2. The Gerrit review host hostname.
3. The Git config setting ``ghish.profile``.

Explicit profile override
=========================
You can force a specific profile on any command using the ``--profile`` flag:

.. code-block:: console

   $ gh-ish pr list --profile fuchsia
   $ gh-ish pr status --profile pigweed

Profile names are strictly validated. Passing an invalid profile name halts with
an error listing all available profiles.

Adding a new project profile
============================
To add a profile for your project, implement the ``ProjectProfile`` interface in
``pw_ghish/profile.go`` (or embed ``genericProfile`` for default behavior) and
register it with ``RegisterProfile``:

.. code-block:: go

   type myProjectProfile struct {
       genericProfile
   }

   func (p *myProjectProfile) Name() string {
       return "myproject"
   }

   func (p *myProjectProfile) DefaultGerritHost() string {
       return "https://myproject-review.googlesource.com/a"
   }

   func (p *myProjectProfile) CQLabel() (LabelVote, bool) {
       return LabelVote{Name: "Commit-Queue", Value: 2}, true
   }

   func (p *myProjectProfile) BuildbucketProject() string {
       return "myproject"
   }

   func init() {
       RegisterProfile(&myProjectProfile{})
   }

Add a test case in ``pw_ghish/profile_test.go`` verifying detection and behavior.

-----------------------------------
Authentication and Credential Setup
-----------------------------------
``./gh auth status`` checks credentials across Gerrit, LUCI Buildbucket, and
Google Issue Tracker (Buganizer), supporting both ``googler`` (internal +
public builders required) and ``community`` (public builders + ``.gitcookies``
or anonymous reads) authentication modes.

For full details on authentication modes, credential lookup order, and
environment variables (``GH_ISH_AUTH_MODE``, ``GH_ISH_AUTH_METHOD``,
``GERRIT_TOKEN``, ``LUCI_TOKEN``), see :ref:`module-pw_ghish-auth`.

-----------------------------------
LUCI CI and Buildbucket Integration
-----------------------------------
Projects using LUCI (such as Chromium, Fuchsia, Pigweed, and Android) can use
Buildbucket integration for check inspection and reruns:

pRPC checks querying
====================
``pw_ghish pr checks`` queries ``cr-buildbucket.appspot.com`` via pRPC to fetch
builder statuses, durations, and build URLs.

Terminal log inspection
=======================
``gh-ish run view --log-failed`` queries Buildbucket step summaries and step
log URLs to retrieve the tail of failed build steps in the terminal:

.. code-block:: console

   # Inspect step execution tree for a specific builder:
   $ gh-ish run view -j myproject-linux-dbg

   # Print failure summaries and step log snippets for failed builders:
   $ gh-ish run view --log-failed

Builder reruns
==============
``gh-ish run rerun`` automatically constructs and executes ``bb add`` commands
targeting your profile's try bucket:

.. code-block:: console

   # Rerun all failed builders on the current change:
   $ gh-ish run rerun --failed

   # Rerun a specific builder:
   $ gh-ish run rerun -j myproject-linux-dbg

   # Preview the generated bb command without executing:
   $ gh-ish run rerun --failed --dry-run
