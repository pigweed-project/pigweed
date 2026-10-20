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

Option C: Copying ``pigweed/gh`` (no Pigweed checkout or environment)
=====================================================================
If your repository does not include Pigweed and does not use ``pw_env_setup``,
copy ``pigweed/gh`` to the root of your repository. The only requirement is
that the `cipd <https://chromium.googlesource.com/infra/luci/luci-go/+/HEAD/cipd/>`_
client is on ``PATH``.

When ``./gh`` cannot find a bootstrapped CIPD binary and cannot build from
source (because there are no Pigweed Bazel build files next to it or
``bazelisk`` is not installed), it uses ``cipd ensure`` to fetch
``pigweed/ghish/${platform}`` into a per-user cache and runs it from there. The
package is re-resolved at most once a day, and the cached binary is used if
CIPD is unreachable. The following environment variables control this:

* ``PW_GH_CIPD_VERSION``: CIPD version to fetch (a ref such as ``latest``, or
  a tag such as ``git_revision:<sha>``). Defaults to ``latest``.
* ``PW_GH_CIPD_ROOT``: Cache directory. Defaults to
  ``${XDG_CACHE_HOME:-~/.cache}/pigweed/gh/cipd``.

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

.. _module-pw_ghish-config:

-----------------------------------------------------------------------
Declarative Repository Configuration (``.ghish.toml`` & ``git config``)
-----------------------------------------------------------------------
Repositories can configure ``pw_ghish`` declaratively without modifying Go source
code by checking in a ``.ghish.toml`` file at the repository root (or in
subdirectories for monorepo overrides) and/or setting ``git config ghish.*``
keys locally.

Resolution & precedence rules
=============================

Discovery and merge order
-------------------------
``pw_ghish`` loads and merges configuration in the following order (later
sources override earlier ones):

1. **Built-in defaults** (``DefaultProjectConfig()``).
2. **Repository root** ``.ghish.toml`` at the Git top-level directory
   (``git rev-parse --show-toplevel``, or the nearest ancestor directory
   containing ``.git`` on disk).
3. **Subdirectory** ``.ghish.toml`` files along the path from the repository
   root down to the current working directory (``$PWD``), applied in top-down
   directory order. For example, when running inside
   ``<repo>/components/sensor``, ``pw_ghish`` merges ``<repo>/.ghish.toml``,
   then ``<repo>/components/.ghish.toml`` (if present), and finally
   ``<repo>/components/sensor/.ghish.toml``.

   * **Scalar and array keys** in a deeper ``.ghish.toml`` replace values from
     parent directories.
   * **Map tables** (``[gerrit.shortlinks]`` and ``[issue.path_components]``)
     are merged across layers, with deeper entries overriding identical keys.

4. **Local or global** ``git config ghish.*`` settings (queried via
   ``git config --get-regexp ^ghish\.``). Subkeys are matched
   case-insensitively with ``_`` and ``-`` stripped (for example,
   ``ghish.gerrit.defaultBranch``, ``ghish.gerrit.default_branch``, and
   ``ghish.gerrit.default-branch`` are equivalent). For ``[]string`` settings,
   ``git config`` values are parsed as comma-separated lists (CSV).
5. **Explicit CLI flags** (such as ``--host``, ``--profile``, ``--base``,
   ``--all-projects``, ``--all``, ``--experimental``, and
   ``--component`` / ``-C``).

Strict validation behavior
--------------------------
Every ``.ghish.toml`` file in the hierarchy and every ``git config ghish.*``
override is strictly validated before any command executes:

* **Syntax errors**: Malformed TOML syntax halts immediately with an exact
  ``<file>:<line>: <error>`` diagnostic.
* **Unsectioned keys**: Any top-level key placed outside a ``[section]`` header
  fails with ``<file>: key "<key>" outside of any [section]``.
* **Unknown sections or keys**: Unknown ``[section]`` headers fail with
  ``<file>: unknown section [<section>]``, and unrecognized keys inside known
  sections (``[gerrit]``, ``[ci]``, ``[issue]``, ``[bugs]``, ``[worktree]``,
  ``[oncall]``) fail with
  ``<file>: unknown key "<key>" in section [<section>]``.
* **Invalid types, enum values, or globs**: Invalid value types, unsupported
  enum values (``gerrit.submodule_policy``, ``ci.providers``,
  ``worktree.warmup_driver``), or malformed glob syntax in
  ``[issue.path_components]`` fail fast in both ``.ghish.toml`` and
  ``git config ghish.*``.

Section & key reference
=======================

``[gerrit]`` section
--------------------
Configures Gerrit host discovery, Git remote/branch resolution, query scoping,
submodule safety policies, and topic restrictions.

.. list-table::
   :header-rows: 1
   :widths: 18 10 12 20 20 20

   * - Key
     - Type
     - Default
     - Allowed Values
     - ``git config`` Equivalent
     - Behavior
   * - ``host``
     - ``string``
     - ``""`` *(auto)*
     - Gerrit hostname or URL
     - ``ghish.gerrit.host``
     - Default Gerrit review server hostname when ``--host`` is not passed on
       the CLI.
   * - ``project``
     - ``string``
     - ``""`` *(auto)*
     - Gerrit project path (e.g. ``"pigweed/pigweed"``)
     - ``ghish.gerrit.project``
     - Gerrit project identifier used for change URLs and default ``gh pr list``
       query scoping.
   * - ``remote``
     - ``string``
     - ``""`` *(cascade)*
     - Git remote name (e.g. ``"origin"``, ``"goog"``, ``"aosp"``, ``"partner"``)
     - ``ghish.gerrit.remote``
     - Git remote used for ``git fetch`` and ``git push``. See
       `Remote resolution cascade`_ below.
   * - ``default_branch``
     - ``string``
     - ``""`` *(cascade ->* ``"main"`` *)*
     - Branch name (e.g. ``"main"``, ``"dev"``)
     - ``ghish.gerrit.defaultBranch``
     - Default upstream target branch for ``refs/for/<branch>`` pushes, rebases,
       and worktree creation. See `Default branch resolution cascade`_ below.
   * - ``scope_list_to_project``
     - ``bool``
     - ``true``
     - ``true``, ``false``
     - ``ghish.gerrit.scopeListToProject``
     - When ``true``, ``gh pr list`` automatically adds ``project:<project>``
       to Gerrit queries unless ``--all-projects`` is passed, ``-S`` already
       contains ``project:``, or ``--host`` targets a different server than
       ``remote.origin.url``.
   * - ``submodule_policy``
     - ``string``
     - ``"allow"``
     - ``"allow"``, ``"warn-unpushed"``, ``"require-pushed"``,
       ``"forbid-manual-rolls"``
     - ``ghish.gerrit.submodulePolicy``
     - Pre-push safety policy for gitlink (``mode 160000``) submodule changes.
       See `Submodule policies & No-Submodule-Changes`_ below.
   * - ``forbid_topics``
     - ``bool``
     - ``false``
     - ``true``, ``false``
     - ``ghish.gerrit.forbidTopics``
     - When ``true``, rejects ``--topic`` and ``-o topic=...`` before pushing
       and blocks ``gh pr edit --topic``. See
       `Topic restrictions & Topics-Not-Supported`_ below.

Remote resolution cascade
^^^^^^^^^^^^^^^^^^^^^^^^^
When resolving which Git remote to query or push to, ``pw_ghish`` evaluates the
following cascade in priority order:

1. ``.ghish.toml`` ``gerrit.remote`` or ``git config ghish.gerrit.remote``.
2. The active local branch's tracking remote (``git config branch.<cur>.remote``
   or the remote prefix of ``@{u}``).
3. ``"origin"`` if present in ``git remote``.
4. Preferred Gerrit remote names present in ``git remote`` in order:
   ``"origin"``, ``"goog"``, ``"aosp"``, ``"partner"`` (or the first remote
   listed by ``git remote``).
5. Fallback ``"origin"``.

Default branch resolution cascade
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
When determining the repository's default upstream branch, ``pw_ghish``
evaluates:

1. ``.ghish.toml`` ``gerrit.default_branch`` or
   ``git config ghish.gerrit.defaultBranch``.
2. The active local branch's tracked upstream merge ref
   (``git config branch.<cur>.merge`` when ``branch.<cur>.remote`` is set and
   the merge target differs from the local branch name).
3. Symbolic ref ``refs/remotes/<remote>/HEAD``.
4. ``git config init.defaultBranch``.
5. Existing local or remote branch refs (``refs/heads/main``,
   ``refs/remotes/<remote>/main``, and legacy default branch refs).
6. Fallback ``"main"``.

When pushing a change with ``gh pr create`` or ``gh pr push``, the target
branch for ``refs/for/<branch>`` is selected in order: (1) explicit
``--base <branch>`` flag, (2) tracked upstream ``@{upstream}``, (3) an existing
remote branch matching the current local branch name, and (4) the resolved
``default_branch``.

Submodule policies & ``No-Submodule-Changes``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
``submodule_policy`` inspects all commits being pushed for gitlink
(``mode 160000``) modifications:

* ``"allow"`` *(default)*: Permits submodule gitlink modifications without
  additional checks.
* ``"warn-unpushed"``: Runs ``git -C <submodule> branch -r --contains <sha>``
  for each modified gitlink and prints a non-blocking warning to ``stderr`` if
  the commit is not reachable from any remote branch.
* ``"require-pushed"``: Aborts ``gh pr create`` and ``gh pr push`` if any
  modified submodule gitlink references a commit not present on a remote
  branch, with remediation steps to either revert the gitlink or push/fetch the
  submodule commit first.
* ``"forbid-manual-rolls"``: Aborts ``gh pr create`` and ``gh pr push`` whenever
  any commit modifies a submodule gitlink, printing the exact
  ``git checkout <remote>/<branch> -- <path> && git commit --amend --no-edit``
  command to revert accidental submodule dirtiness.
* **Gerrit submit requirement override**: If an existing Gerrit change reports
  an active ``No-Submodule-Changes`` submit requirement
  (``Status != "NOT_APPLICABLE"``), ``pw_ghish`` automatically enforces
  ``"forbid-manual-rolls"`` even if ``submodule_policy`` is ``"allow"``.

Topic restrictions & ``Topics-Not-Supported``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Setting ``forbid_topics = true`` blocks ``--topic <name>`` and
``-o topic=<name>`` on ``gh pr create``, ``gh pr push``, and ``gh pr edit``
before contacting Gerrit. In addition, even when ``forbid_topics`` is ``false``,
if an existing Gerrit change carries an active ``Topics-Not-Supported`` submit
requirement, ``pw_ghish`` rejects topic assignment and recommends
``gh pr edit --remove-topic`` if a topic is already blocking submission.

``[gerrit.shortlinks]`` section
-------------------------------
Registers custom CL shortlink prefixes accepted by all commands that take a
change target (``gh pr view``, ``gh pr checkout``, ``gh pr diff``,
``gh pr checks``, ``gh run view``, ``gh sense``, ``gh wt use --cl``) and used by
``gh wt list`` when formatting CL status summaries.

.. list-table::
   :header-rows: 1
   :widths: 22 14 14 25 25

   * - Key
     - Type
     - Default
     - ``git config`` Equivalent
     - Behavior
   * - ``"<prefix>" = "<host>"``
     - ``map[string]string``
     - ``{}`` *(plus built-ins)*
     - ``ghish.gerrit.shortlinks.<prefix>`` or ``ghish.shortlink.<prefix>``
     - Maps ``<prefix>/<id>[/<ps>]`` and ``go/<prefix>/<id>`` to ``<host>``.

* **Resolution rules**: Custom prefixes are normalized with a trailing ``/`` and
  sorted longest-prefix-first so multi-segment prefixes (such as ``"acmer/i"``)
  match before shorter prefixes (``"acmer"``). Both numeric change IDs
  (``myrev/12345``, ``myrev/12345/3``) and 41-character Gerrit ``Change-Id``
  hashes (``myrev/I1234...``) are supported, with or without ``go/``,
  ``http(s)://go/``, or ``http(s)://goto.google.com/`` prefixes.
* **Built-in shortlinks**: Always available alongside custom entries:
  ``pwrev/``, ``pwrev.dev/``, ``pwrev/i/``, ``pwrev.dev/i/``, ``fxrev/``,
  ``fxrev.dev/``, ``fxr/``, ``fxrev/i/``, ``fxrev.dev/i/``, ``fxr/i/``,
  ``ag/``, ``aosp/``, ``crrev/c/``, ``crrev.com/c/``, ``crrev/i/``, and
  ``crrev.com/i/``.

``[ci]`` section
----------------
Configures CI/CD provider selection, Buildbucket hosts/buckets, orchestrator
subbuild tag filtering, failure log extraction, and local presubmit hints.

.. list-table::
   :header-rows: 1
   :widths: 18 10 14 18 18 22

   * - Key
     - Type
     - Default
     - Allowed Values
     - ``git config`` Equivalent
     - Behavior
   * - ``providers``
     - ``[]string``
     - ``["auto"]``
     - ``"auto"``, ``"buildbucket"`` (``"luci"``), ``"busytown"``
       (``"android-build"``, ``"android_build"``, ``"treehugger"``,
       ``"treetop"``), ``"gerrit"``
     - ``ghish.ci.providers`` *(CSV)*
     - Selects CI backend(s) for ``gh pr checks``, ``gh run``, and ``gh sense``.
       See `CI provider modes`_ below.
   * - ``buildbucket_host``
     - ``string``
     - ``"cr-buildbucket.appspot.com"``
     - Hostname
     - ``ghish.ci.buildbucketHost``
     - LUCI Buildbucket v2 pRPC server hostname.
   * - ``try_buckets``
     - ``[]string``
     - ``[]`` *(profile default)*
     - ``"<project>/<bucket>"`` strings
     - ``ghish.ci.tryBuckets`` *(CSV)*
     - LUCI Buildbucket try buckets for the repository (e.g.
       ``["pigweed/try"]``).
   * - ``hide_tag_filters``
     - ``[]string``
     - ``[]``
     - Buildbucket ``"<key>:<value>"`` tags
     - ``ghish.ci.hideTagFilters`` *(CSV)*
     - Omits builds matching any listed tag (e.g.
       ``["hide-in-gerrit:subbuild"]``) from ``gh pr checks``, ``gh run list``,
       ``gh run view``, and ``gh sense`` unless ``--all`` is passed.
   * - ``skip_retry_tag_filters``
     - ``[]string``
     - ``["skip-retry-in-gerrit:subbuild"]``
     - Buildbucket ``"<key>:<value>"`` tags
     - ``ghish.ci.skipRetryTagFilters`` *(CSV)*
     - Skips matching builds when collecting failed builders for
       ``gh run rerun --failed`` so child subbuilds are retried via their
       parent orchestrator builder instead of scheduled standalone.
   * - ``preferred_logs``
     - ``[]string``
     - ``["failure summary", "failure_summary", "stdout", "stderr"]``
     - Step log names
     - ``ghish.ci.preferredLogs`` *(CSV)*
     - Ordered preference list of step log streams extracted by
       ``gh run view --log-failed``, ``gh pr checks --log-failed``, and
       ``gh sense``.
   * - ``include_summary_markdown``
     - ``bool``
     - ``true``
     - ``true``, ``false``
     - ``ghish.ci.includeSummaryMarkdown``
     - When ``true``, includes top-level build ``SummaryMarkdown`` in CI
       failure reports alongside failed step logs.
   * - ``local_presubmit_hint``
     - ``string``
     - ``""``
     - Shell command string
     - ``ghish.ci.localPresubmitHint``
     - Project-specific local verification command surfaced in diagnostic hints
       (e.g. ``"./pw presubmit"`` or ``"fx test"``).

CI provider modes
^^^^^^^^^^^^^^^^^
* ``["auto"]`` *(default)*: Queries LUCI Buildbucket by default, and
  automatically activates composite ``BuildbucketProvider`` +
  ``BusytownProvider`` mode when the Gerrit hostname contains ``"android"``,
  when the CL has ``Presubmit-Ready``, ``Presubmit-Verified``, or
  ``Autosubmit`` labels, or when Gerrit change messages mention ``TreeHugger``
  or ``ci.android.com``.
* ``["buildbucket"]`` / ``["luci"]``: Queries LUCI Buildbucket v2 and LogDog
  only.
* ``["busytown"]`` / ``["android-build"]`` / ``["treehugger"]``: Queries
  Android Busytown / TreeHugger (``treetop~`` Gerrit checks, ``ci.android.com``,
  Android Build v3 REST API, and local workstation accelerators
  ``fetch_artifact`` and ``ants_cli``) only.
* ``["buildbucket", "busytown"]``: Explicitly enables dual-stack
  ``CompositeCIProvider`` fan-out across both LUCI Buildbucket and Android
  Busytown, routing build detail queries and ``gh run rerun`` actions to the
  appropriate backend per build ID.
* ``["gerrit"]``: Evaluates Gerrit submit requirements and verification labels
  without querying external build backends.

``[issue]`` section (and ``[bugs]`` alias)
------------------------------------------
Configures Google Issue Tracker (Buganizer) defaults, quota projects, commit
message trailer templates, and ``OWNERS`` component discovery. All keys under
``[issue]`` are also accepted under ``[bugs]`` (and ``ghish.bugs.*`` in
``git config``).

.. list-table::
   :header-rows: 1
   :widths: 18 10 14 18 18 22

   * - Key
     - Type
     - Default
     - Allowed Values
     - ``git config`` Equivalent
     - Behavior
   * - ``system``
     - ``string``
     - ``""`` (``"buganizer"``)
     - ``"buganizer"``
     - ``ghish.issue.system``
     - Issue tracker backend identifier.
   * - ``prefix``
     - ``string``
     - ``""`` (``"b/"``)
     - Shorthand prefix (e.g. ``"b/"``, ``"fxb/"``, ``"pwbug/"``)
     - ``ghish.issue.prefix``
     - Default issue prefix for the repository.
   * - ``host``
     - ``string``
     - ``""`` *(profile default)*
     - Hostname (e.g. ``"issues.pigweed.dev"``, ``"issuetracker.google.com"``)
     - ``ghish.issue.host``
     - Web hostname used when rendering issue URLs.
   * - ``default_component`` *(alias* ``default_component_id`` *)*
     - ``int64`` *(or numeric string)*
     - ``0``
     - Positive integer (e.g. ``1194524``)
     - ``ghish.issue.defaultComponent`` / ``ghish.componentid``
     - Fallback Buganizer component ID for ``gh issue list`` and
       ``gh issue create``.
   * - ``quota_project``
     - ``string``
     - ``""``
     - GCP project ID or number
     - ``ghish.issue.quotaProject``
     - Google Cloud project sent via ``X-Goog-User-Project`` when calling
       ``issuetracker.googleapis.com``.
   * - ``trailer_format`` *(alias* ``require_trailer`` *)*
     - ``string`` *(or* ``bool`` *for* ``require_trailer`` *)*
     - ``"Bug: b/{id}"``
     - Template with ``{id}`` or ``<id>`` (e.g. ``"Bug: b/{id}"``,
       ``"Bug: {id}"``, ``"Bug: fxbug.dev/{id}"``, ``"Bug: pwbug.dev/{id}"``,
       ``"b/{id}"``, ``"{id}"``)
     - ``ghish.issue.trailerFormat`` / ``ghish.issue.requireTrailer``
     - Template used by ``gh issue create --amend``,
       ``gh pr create --bug/--fixed``, and commit trailer normalization. Both
       ``{id}`` and ``<id>`` placeholders expand to the numeric issue ID. Setting
       ``require_trailer = true`` defaults ``trailer_format`` to
       ``"Bug: b/{id}"``.
   * - ``use_owners_components``
     - ``bool``
     - ``true``
     - ``true``, ``false``
     - ``ghish.issue.useOwnersComponents``
     - When ``true``, walks upward from modified files (or ``$PWD``) to the
       repository root inspecting ``OWNERS`` files for
       ``# COMPONENT: [b/]<id>`` or ``# Buganizer component: [b/]<id>``.

``[issue.path_components]`` section (and ``[bugs.path_components]`` alias)
--------------------------------------------------------------------------
Maps repository directory prefixes or glob patterns to Buganizer component IDs.

.. list-table::
   :header-rows: 1
   :widths: 22 14 14 25 25

   * - Key
     - Type
     - Default
     - ``git config`` Equivalent
     - Behavior
   * - ``"<path-or-glob>" = <id>``
     - ``map[string]int64``
     - ``{}``
     - ``ghish.issue.pathComponents.<path>`` *(also* ``path_components`` *,*
       ``path-components`` *)*
     - Routes matching file paths or ``$PWD`` to Buganizer component ``<id>``.

Path matching & 6-tier component resolution cascade
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Patterns in ``[issue.path_components]`` support directory prefixes
(``"pw_kernel/"`` or ``"pw_kernel"``), single-level globs
(``"pw_bluetooth_sapphire/*"``), recursive globs (``"targets/**"`` or ``"**"``),
and standard ``path.Match`` character classes. When multiple patterns match a
candidate path, the **longest pattern string (``len(pattern)``) wins**.

``ResolveIssueComponent`` resolves the target Buganizer component ID using a
6-tier cascade:

1. **Explicit CLI flag**: ``--component <id>`` / ``-C <id>``.
2. **Path component rules** (``[issue.path_components]`` in ``.ghish.toml`` or
   ``git config``): Matched against files modified in ``HEAD``
   (``git diff-tree --no-commit-id --name-only -r HEAD``) or, when ``HEAD`` has
   no candidate paths, ``$PWD`` relative to the repository root. Longest
   matching pattern wins.
3. **Nearest ``OWNERS`` file** (when ``use_owners_components = true``): Walks
   upward from each modified file's directory (or ``$PWD``) to the repository
   root looking for ``# COMPONENT: [b/]<id>`` or
   ``# Buganizer component: [b/]<id>``.
4. **Project config default**: ``[issue] default_component`` (or
   ``default_component_id``) in ``.ghish.toml`` or
   ``git config ghish.issue.defaultComponent``.
5. **Legacy Git config**: ``git config ghish.componentid``.
6. **Project profile default**: ``ProjectProfile.DefaultComponentID()`` (e.g.
   ``1194524`` for the ``pigweed`` profile).

``[issue.components]`` & ``[issue.hotlists]`` sections
------------------------------------------------------
Maps symbolic alias names to numeric Buganizer component IDs and hotlist IDs
for use in ``-l component:<alias>``, ``-l hotlist:<alias>``, ``--add-hotlist``,
and ``--remove-hotlist``.

.. list-table::
   :header-rows: 1
   :widths: 22 14 14 25 25

   * - Key
     - Type
     - Default
     - ``git config`` Equivalent
     - Behavior
   * - ``components."<alias>" = <id>``
     - ``map[string]int64``
     - ``{}``
     - ``ghish.issue.components.<alias>``
     - Expands ``component:<alias>`` labels to ``component:<id>``.
   * - ``hotlists."<alias>" = <id>``
     - ``map[string]int64``
     - ``{}``
     - ``ghish.issue.hotlists.<alias>``
     - Expands ``hotlist:<alias>`` labels and ``--add-hotlist`` /
       ``--remove-hotlist <alias>`` arguments to Buganizer hotlist ID ``<id>``.

``[worktree]`` section
----------------------
Configures warm Git worktree slot naming and build cache warmup behavior for
``gh wt`` (``gh worktree``).

.. list-table::
   :header-rows: 1
   :widths: 18 10 14 18 18 22

   * - Key
     - Type
     - Default
     - Allowed Values
     - ``git config`` Equivalent
     - Behavior
   * - ``slot_prefix``
     - ``string``
     - ``"pw-"`` *(->* ``pw-01..N`` *)*
     - Non-empty prefix string
     - ``ghish.worktree.slotPrefix``
     - Prefix used for physical worktree slot directories in
       ``~/wrk/slots/<prefix>-01..N`` and IDE workspace project names. Trailing
       ``-`` is normalized automatically.
   * - ``warmup_driver``
     - ``string``
     - ``"bazel"``
     - ``"bazel"``, ``"none"`` *(or* ``"gn"`` *)*
     - ``ghish.worktree.warmupDriver``
     - Build cache driver for ``gh wt init`` and ``gh wt gc``. ``"bazel"``
       configures ``~/.config/pw_ghish/bazelrc.worktrees`` (shared 80 GB
       auto-GC disk cache + hardlinked repo cache) and GCs orphaned
       ``_bazel_$USER`` output bases; ``"none"`` (or ``"gn"``) skips Bazel
       hooks and output base GC.

``[oncall]`` section
--------------------
Configures oncall rotation detection for ``gh sense``.

.. list-table::
   :header-rows: 1
   :widths: 18 10 14 18 18 22

   * - Key
     - Type
     - Default
     - Allowed Values
     - ``git config`` Equivalent
     - Behavior
   * - ``schedule_file``
     - ``string``
     - ``""``
     - File path
     - ``ghish.oncall.scheduleFile``
     - Path to the rotation schedule file inspected by ``gh sense`` to determine
       whether the current user is on active primary or secondary oncall duty.

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

   # Configure CI providers (CSV for slices) and hidden subbuild tags:
   $ git config --local ghish.ci.providers "buildbucket,busytown"
   $ git config --local ghish.ci.hideTagFilters "hide-in-gerrit:subbuild,hide-in-gerrit:experimental"

   # Register a custom Gerrit shortlink prefix:
   $ git config --local ghish.gerrit.shortlinks.myrev "myproject-review.googlesource.com"

   # Configure Buganizer component, path mapping, and trailer format:
   $ git config --local ghish.issue.defaultComponent 1194524
   $ git config --local ghish.issue.pathComponents.pw_kernel/ 1456790
   $ git config --local ghish.issue.trailerFormat "Bug: b/{id}"

   # Set a custom local presubmit hint:
   $ git config --local ghish.ci.localPresubmitHint "fx test"

End-to-end cookbook recipes
===========================

1. LUCI Buildbucket monorepo with orchestrator subbuilds
--------------------------------------------------------
For a LUCI monorepo where a top-level orchestrator builder spawns child
subbuilds tagged with ``hide-in-gerrit:subbuild`` and
``skip-retry-in-gerrit:subbuild``, and subdirectories map to dedicated
Buganizer components:

.. code-block:: toml

   # .ghish.toml
   [gerrit]
   host = "pigweed-review.googlesource.com"
   project = "pigweed/pigweed"
   remote = "origin"
   default_branch = "main"
   scope_list_to_project = true
   submodule_policy = "warn-unpushed"

   [gerrit.shortlinks]
   "pwrev" = "pigweed-review.googlesource.com"
   "pwrev/i" = "pigweed-internal-review.googlesource.com"

   [ci]
   providers = ["buildbucket"]
   buildbucket_host = "cr-buildbucket.appspot.com"
   try_buckets = ["pigweed/try"]
   hide_tag_filters = ["hide-in-gerrit:subbuild"]
   skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]
   preferred_logs = ["failure summary", "stdout", "stderr"]
   include_summary_markdown = true
   local_presubmit_hint = "./pw presubmit"

   [issue]
   system = "buganizer"
   prefix = "b/"
   host = "issues.pigweed.dev"
   default_component = 1194524
   trailer_format = "Bug: b/{id}"
   use_owners_components = true

   [issue.path_components]
   "pw_bluetooth_sapphire/**" = 1456789
   "pw_kernel/**" = 1456790

   [issue.components]
   security = 1456791

   [issue.hotlists]
   triage = 9876543

   [worktree]
   slot_prefix = "pw-"
   warmup_driver = "bazel"

2. Android Busytown / TreeHugger (``Presubmit-Ready`` + ``Autosubmit``) repository
----------------------------------------------------------------------------------
For an Android Gerrit repository that uses ``goog`` as its remote, runs
presubmits via TreeHugger / Android Busytown (``ci.android.com``), and forbids
manual submodule gitlink modifications:

.. code-block:: toml

   # .ghish.toml
   [gerrit]
   host = "googleplex-android-review.googlesource.com"
   project = "platform/external/myproject"
   remote = "goog"
   default_branch = "main"
   scope_list_to_project = true
   submodule_policy = "forbid-manual-rolls"

   [gerrit.shortlinks]
   "ag" = "googleplex-android-review.googlesource.com"

   [ci]
   providers = ["busytown"]
   include_summary_markdown = true
   local_presubmit_hint = "atest MyProjectTests"

   [issue]
   system = "buganizer"
   prefix = "b/"
   host = "issuetracker.google.com"
   default_component = 1357924
   trailer_format = "Bug: {id}"
   use_owners_components = true

   [worktree]
   slot_prefix = "and-"
   warmup_driver = "none"

3. Dual-stack LUCI Buildbucket + Android Busytown repository
------------------------------------------------------------
For a partner or firmware repository hosted on an Android or partner Gerrit
instance that runs both LUCI Buildbucket tryjobs and Android Busytown
presubmits on every CL:

.. code-block:: toml

   # .ghish.toml
   [gerrit]
   host = "partner-android-review.googlesource.com"
   project = "device/google/firmware"
   remote = "partner"
   default_branch = "main"
   scope_list_to_project = true

   [ci]
   # Fan out `gh pr checks`, `gh run view`, and `gh run rerun` across both backends:
   providers = ["buildbucket", "busytown"]
   buildbucket_host = "cr-buildbucket.appspot.com"
   try_buckets = ["firmware/try"]
   hide_tag_filters = ["hide-in-gerrit:subbuild"]
   skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]

   [issue]
   default_component = 1928374
   trailer_format = "Bug: b/{id}"

4. Multi-remote / submodule superproject configuration
------------------------------------------------------
For a superproject repository with submodules where all submodule gitlink
updates must already be pushed to a remote branch before uploading a superproject
CL, topics are forbidden by the Gerrit ``Topics-Not-Supported`` submit
requirement, and a subdirectory overrides the Gerrit project and Buganizer
component:

.. code-block:: toml

   # Root .ghish.toml
   [gerrit]
   host = "acme-internal-review.googlesource.com"
   project = "acme/superproject"
   remote = "origin"
   default_branch = "main"
   submodule_policy = "require-pushed"
   forbid_topics = true

   [gerrit.shortlinks]
   "acmerev" = "acme-internal-review.googlesource.com"

   [issue]
   default_component = 1000100
   trailer_format = "Bug: b/{id}"

.. code-block:: toml

   # Subdirectory vendor/sensor/.ghish.toml (overrides root settings when CWD is under vendor/sensor/)
   [gerrit]
   remote = "partner"
   forbid_topics = false

   [ci]
   local_presubmit_hint = "bazelisk test //vendor/sensor/..."

   [issue]
   default_component = 2000200
   [issue.path_components]
   "vendor/sensor/dsp/**" = 2000201

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
