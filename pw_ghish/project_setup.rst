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

Distributing via CIPD or package managers
=========================================
For large multi-repo projects (such as Fuchsia or Chromium), ``gh-ish`` can be
packaged as a CIPD (Chrome Infrastructure Package Deployer) package or added to
host toolchains so developers and CI bots have it pre-installed on their
``$PATH``.

-----------------------------
Creating a repository wrapper
-----------------------------
Rather than requiring every developer and AI coding agent to manually install
and update a global binary, we recommend placing a wrapper script named ``gh``
at the root of your repository (e.g. ``./gh``).

Why use a repository wrapper?
=============================
1. **No global installation step**: Contributors and coding agents can run
   ``./gh pr list`` or ``./gh pr status`` directly from a fresh checkout.
2. **Commit-pinned consistency**: The wrapper builds or downloads the version
   of the tool matching the repository's current commit.
3. **Local binary caching**: The wrapper builds the binary once per commit and
   caches it in a local output directory (e.g. ``out/gh/``) for subsequent
   runs.
4. **Standard CLI entry point**: Coding agents can invoke ``./gh`` from the
   checkout root using standard GitHub CLI subcommands.

Example wrapper implementation
==============================
Here is an example wrapper script that builds and caches the binary:

.. code-block:: bash

   #!/usr/bin/env bash
   set -euo pipefail

   REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
   CACHE_DIR="${REPO_ROOT}/out/gh"
   COMMIT_HASH="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo "latest")"
   CACHED_BIN="${CACHE_DIR}/gh-ish-${COMMIT_HASH}"

   if [[ ! -x "${CACHED_BIN}" ]]; then
     mkdir -p "${CACHE_DIR}"
     # Build using Bazel or Go:
     bazelisk build --noshow_progress //pw_ghish:gh-ish
     cp -f "${REPO_ROOT}/bazel-bin/pw_ghish/gh-ish_/gh-ish" "${CACHED_BIN}"
     chmod +x "${CACHED_BIN}"
   fi

   exec "${CACHED_BIN}" "$@"

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
   # Default Gerrit host and project for this repository:
   host = "pigweed-review.googlesource.com"
   project = "pigweed/pigweed"
   default_branch = "main"

   # Scope `gh pr list` to this Gerrit project by default (pass --all-projects to override):
   scope_list_to_project = true

   # Submodule safety policy ("allow", "warn-unpushed", or "forbid-manual-rolls"):
   submodule_policy = "warn-unpushed"

   # Reject --topic / -o topic=... before git push on projects with Topics-Not-Supported:
   forbid_topics = false

   # Custom shortlink prefixes accepted by all commands (e.g. `gh pr view myrev/12345`):
   [gerrit.shortlinks]
   "myrev" = "myproject-review.googlesource.com"

   [ci]
   # CI providers used by this project ("luci", "gerrit"):
   providers = ["luci", "gerrit"]
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

Overriding settings via ``git config``
======================================
Any ``.ghish.toml`` setting can be configured or overridden locally via
``git config``:

.. code-block:: console

   # Hide subbuilds in `gh pr checks`, `gh run list`, and `gh run view`:
   $ git config --local ghish.ci.hideTagFilters "hide-in-gerrit:subbuild"

   # Register a custom Gerrit shortlink prefix:
   $ git config --local ghish.gerrit.shortlink.myrev "myproject-review.googlesource.com"

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
