.. _module-pw_ghish-project-integration:

======================
Project Adoption Guide
======================
.. pigweed-module-subpage::
   :name: pw_ghish

While ``pw_ghish`` provides the ``./gh`` CLI for Pigweed contributors, its
underlying engine (``gh-ish``) can be used with any project hosted on Gerrit
and LUCI.

This guide explains how to integrate ``gh-ish`` into a repository, build and
distribute the binary, configure project profiles, set up authentication, and
inspect CI builds.

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
and update a global binary, we strongly recommend placing a lightweight wrapper
script named ``gh`` at the root of your repository (e.g. ``./gh``).

Why use a repository wrapper?
=============================
1. **Zero-setup onboarding**: New contributors and AI agents can immediately run
   ``./gh pr list`` or ``./gh pr status`` without prior installation steps.
2. **Commit-pinned consistency**: The wrapper automatically builds or downloads
   the exact version of the tool tied to the repository's current commit,
   eliminating "works on my machine" version skew.
3. **Transparent caching**: The wrapper can build the binary once per commit and
   cache it in a local output directory (e.g. ``out/gh/``), providing instant
   sub-second execution on subsequent runs.
4. **Agent muscle memory**: AI coding agents frequently check for a ``./gh``
   executable or default to GitHub CLI commands. Providing ``./gh`` allows
   agents to interact with the repository without custom instructions.

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

----------------------------
Project Profile Architecture
----------------------------
Every Gerrit project has unique review conventions: differing label names
(such as ``Code-Review`` vs. ``Commit-Queue`` vs. ``Auto-Submit``), varying
presubmit gates, and dedicated LUCI build buckets.

``pw_ghish`` abstracts these differences through **Project Profiles**.

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
``pw_ghish`` supports multiple authentication strategies, from standard open-source
cookie and token files to automated CI bot credentials and Google-internal
developer workstations.

Credential discovery order
==========================
When communicating with Gerrit REST APIs, ``pw_ghish`` searches for credentials in
the following order:

1. **Explicit Environment Token**:
   Reads bearer or personal access tokens from the ``GERRIT_TOKEN`` environment
   variable. Recommended for automated CI pipelines and bots.

   .. code-block:: console

      $ export GERRIT_TOKEN="your-http-access-token"

2. **Git Cookies** (``.gitcookies``):
   Parses Netscape-formatted cookie jars at ``~/.gitcookies`` or the path
   specified by ``git config http.cookiefile``. This is the standard mechanism
   used by Git-on-Borg and Google Open Source Gerrit hosts.

   To generate cookies for a Google-hosted Gerrit server:
   * Visit the Gerrit web UI (e.g. ``https://your-host-review.googlesource.com``).
   * Click your user avatar and select **Settings** -> **HTTP Credentials**.
   * Click **Generate Password** and copy the provided command into your terminal.

3. **Netrc** (``.netrc``):
   Parses machine entries in ``~/.netrc`` matching the target Gerrit hostname.

4. **Internal Workstation Transport**:
   When running on Google-internal developer workstations, ``pw_ghish``
   automatically integrates with local authentication helpers without requiring
   manual configuration.

5. **Anonymous Read Fallback**:
   If no credentials are found, public read operations (such as ``pr view``,
   ``pr list``, ``pr diff``, and ``pr checks``) fall back to unauthenticated
   access on public Gerrit hosts. Write operations (such as ``pr push``,
   ``pr comment``, or ``pr merge``) report an actionable authentication error.

Forcing authentication method
=============================
In automated environments or debugging sessions, you can strictly enforce a
specific authentication backend using the ``GH_ISH_AUTH_METHOD`` environment
variable:

.. code-block:: console

   $ export GH_ISH_AUTH_METHOD=cookies    # Strictly require .gitcookies
   $ export GH_ISH_AUTH_METHOD=token      # Strictly require GERRIT_TOKEN
   $ export GH_ISH_AUTH_METHOD=none       # Force anonymous access
   $ export GH_ISH_AUTH_METHOD=gob-curl   # Force workstation helper transport

If the requested authentication method cannot be satisfied (e.g. missing cookie
file or missing token), ``pw_ghish`` immediately halts with a descriptive error
rather than silently falling back to anonymous access.

-----------------------------------
LUCI CI and Buildbucket Integration
-----------------------------------
Projects using LUCI (such as Chromium, Fuchsia, Pigweed, and Android) benefit
from direct Buildbucket and LogDog integration:

pRPC checks querying
====================
``pw_ghish pr checks`` sends lightweight pRPC queries directly to
``cr-buildbucket.appspot.com`` to fetch builder statuses, run times, and direct
Milo build URLs. It operates without web scraping or headless browser overhead.

Terminal log inspection
=======================
``pw_ghish pr checks log`` queries LogDog streams to retrieve the tail of failed
build steps, providing immediate terminal diagnostics for broken tests and lints
without opening a web browser.

Builder reruns
==============
``pw_ghish pr checks rerun`` automatically constructs and executes ``bb add``
commands targeting your profile's try bucket:

.. code-block:: console

   # Rerun all failed builders on the current change:
   $ gh-ish pr checks rerun --failed

   # Rerun a specific builder:
   $ gh-ish pr checks rerun myproject-linux-dbg

   # Preview the generated bb command without executing:
   $ gh-ish pr checks rerun --failed --dry-run
