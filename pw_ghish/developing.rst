.. _module-pw_ghish-developing:
.. _module-pw_ghish-contributing:

==========
Developing
==========
.. pigweed-module-subpage::
   :name: pw_ghish

This section is for contributors developing, testing, and extending ``pw_ghish``
(``//pw_ghish:gh-ish``).

.. toctree::
   :maxdepth: 1
   :hidden:

   Compat policy <flag_compatibility>
   Agent evaluation <agent_eval>
   Status & roadmap <roadmap>

----------------------------
Building and running locally
----------------------------
``pw_ghish`` is written in Go and built with Bazel:

.. code-block:: console

   $ bazelisk build //pw_ghish:gh-ish

When you run ``./gh`` from the repository root, the wrapper script checks
``out/gh/gh-ish-<commit>`` and automatically rebuilds the binary when ``HEAD``
changes. During local development on uncommitted changes, you can either run
``bazelisk run //pw_ghish:gh-ish -- <args>`` or delete cached binaries in
``out/gh/`` to force a fresh build.

-------------
Running tests
-------------
``pw_ghish`` uses two test suites:

1. **Hermetic unit tests** (run in presubmit):

   .. code-block:: console

      $ bazelisk test //pw_ghish/...

   Unit tests run under a package ``TestMain`` (``hermetic_main_test.go``)
   that makes credential tool lookup and token subprocesses fail and refuses
   non-loopback HTTP; install credentials with ``WithHermeticAuth``.

2. **Live integration tests** (run manually with workstation credentials against
   ``pigweed-review.googlesource.com``, Buildbucket, and Buganizer):

   .. code-block:: console

      $ go test -v -tags=live ./pw_ghish -run TestLive

   See :ref:`module-pw_ghish-agent-eval` for details on the live test suite and
   agent evaluation scenarios.

----------------------
Engineering invariants
----------------------
Changes to ``pw_ghish`` follow strict CLI reliability and testing conventions
(see ``pw_ghish/AGENTS.md`` for full details):

* **Error propagation** (``RunE``): Every Cobra command uses ``RunE`` and
  returns non-zero exit codes on RPC, subprocess, or validation failures.
  ``os.Exit`` is called only in ``pw_ghish/cmd/main.go``.
* **Output stream discipline**: Write primary command output to
  ``cmd.OutOrStdout()`` and diagnostics/warnings to ``cmd.ErrOrStderr()`` so
  unit tests can capture both streams hermetically.
* **Broken pipes**: ``gh ... | head -1`` must never leave a mutation half done.
  ``pw_ghish.Main`` handles ``SIGPIPE`` (so writes return ``EPIPE`` instead of
  killing the process) and wraps piped stdout/stderr in a ``PipeGuard``: once
  the reader is gone, ``mutation`` and ``meta`` commands keep running with
  output discarded and exit ``141`` (128+SIGPIPE) after finishing, while
  ``view``, ``list``, and ``stream`` commands stop at the first failed write,
  like ``git log | head``. Real failures keep their own exit code. With
  ``2>&1`` both streams share one guard, so child output stays in order.
  Subprocesses that receive these writers run via
  ``runBoundingOrphanedPipes``, which sets ``WaitDelay`` so a detached
  grandchild holding the pipe cannot hang the command. The ``./gh``
  wrapper likewise keeps Bazel build chatter off stdout and, unless stderr is
  a terminal, captures it and replays it only if the build fails, so a closed
  pipe cannot abort the wrapper before it runs the binary.
* **Commit trailer preservation**: Commands that modify commit messages (such as
  ``pr create``, ``pr push``, and ``issue create --amend``) parse and preserve
  existing Git trailers (``Change-Id:``, ``Bug:``, ``Fixed:``,
  ``Reviewed-on:``).
* **Actionable error messages**: Errors state what failed, why (the precondition
  or server state), the exact command to fix it, and an inspection command to
  check current state.

----------------------------------------
UI design language and context budgeting
----------------------------------------
``pw_ghish`` serves both interactive terminal users and coding agents operating
under strict context-window budgets. New and modified commands should use the
shared formatting helpers in ``pw_ghish/ui.go`` and ``pw_ghish/environment.go``.

Answer-first layout and progressive disclosure
==============================================
When writing or updating command output:

* **Surface verdicts and next steps**: Include a ``Verdict:`` summary line and
  an actionable ``Next:`` command hint so callers see the current state and
  recommended follow-up action immediately.
* **Order actionable items first**: Render failing or running checks and
  unresolved review threads before passing or resolved items, and group
  repeated failure diagnostics via ``ClusterFailedBuilds``.
* **Fold high-volume details behind** ``--all`` **or** ``--path``: Collapse passing
  check rows, resolved comment threads, automated bot dumps, and large or
  generated diffs by default. Whenever output is folded, print a 1-line summary
  showing the exact ``--all`` or ``--path <glob>`` command to expand it.
* **Keep tables compact**: Target ``<100`` columns without decorative ``===``
  separator walls.

UI primitives, glyphs, and receipts
===================================
Shared terminal formatting primitives live in ``pw_ghish/ui.go`` and
``pw_ghish/environment.go``:

.. list-table::
   :header-rows: 1

   * - Primitive
     - Usage and rules
   * - Canonical BMP glyphs
     - Use single-width BMP constants from ``ui.go``: ``✓`` (``GlyphPass``),
       ``✗`` (``GlyphFail``), ``⚠`` (``GlyphWarn``), ``●`` (``GlyphRunning``),
       ``○`` (``GlyphPending``), ``·`` (``GlyphDot``), and ``→``
       (``GlyphArrow``). Avoid decorative ASCII walls or arbitrary emojis.
   * - TTY, pipe, and agent color discipline
     - Never embed raw ANSI escape literals outside ``ui.go``. Gate all styling
       through ``StyleGreen``, ``StyleRed``, ``StyleYellow``, ``StyleCyan``,
       ``StyleBold``, and ``StyleDim`` (which check ``ColorEnabled(w)`` /
       ``IsTTY(w)`` / ``IsAgentEnv()``) so pipes, test buffers, and coding
       agents always receive pure plaintext.
   * - Width-safe column padding
     - Pad table cells using ``PadRightStyled(w, raw, width, styleFn)``, which
       measures rune width before applying ANSI codes so colored TTY output
       never breaks column alignment.
   * - Structured mutation receipts
     - Every mutating command (annotated ``ghish:kind: mutation``) must confirm
       completion via ``PrintReceipt`` or ``FormatReceipt`` (``✓ <Summary>``
       with contextual ``Details``, ``URL:``, and ``Next:`` step hints).

.. _module-pw_ghish-developing-new-command-checklist:

New command checklist
=====================
Follow this checklist when adding or updating a runnable Cobra command:

#. **Declare** ``ghish:kind`` inline in the ``cobra.Command`` literal:
   ``Annotations: map[string]string{KindAnnotation: KindView}`` (use
   ``pw_ghish.KindAnnotation`` from ``pw_ghish/worktree``). Pick one kind from
   ``pw_ghish/command_kind.go``:

   * ``mutation``: Changes Gerrit, Buganizer, LUCI, git, or local state.
     Commands that both mutate and print are mutations; give a read-only mode
     its own command rather than an alias.
   * ``view``: Renders one object in detail.
   * ``list``: Renders a collection.
   * ``stream``: Long-running, streaming, or passthrough output.
   * ``meta``: Plumbing invoked by tools rather than people.

   When a flag changes the behavior (for example ``pr checks --watch``),
   declare the default kind.
#. **mutation**: End with a ``PrintReceipt`` or ``FormatReceipt`` receipt that
   carries a ``Next:`` hint on its success path.
#. **view** and **list**: Include a ``Verdict:`` line and a ``Next:`` hint, and
   fold output past its budget behind ``--all`` or ``--path`` with a 1-line
   unfold hint. If stdout is a raw artifact meant for other tools (such as
   ``pr diff``), declare ``OutputAnnotation: OutputRaw`` instead.
#. **Register in the kind smoke harness**: Add one ``kindSmokeArgs`` entry in
   ``pw_ghish/kind_contract_test.go`` keyed by the command path (for example
   ``"gh-ish pr view": {"pr", "view", "12345"}``), using ``kindSmokeSetup`` for
   any unmeasured preparation steps (see
   :ref:`module-pw_ghish-developing-kind-smoke`).
#. **Stay within subprocess and RPC budgets**: Construct all external processes
   via ``ExecCommand`` / ``ExecCommandContext`` in ``pw_ghish/procexec.go`` and
   stay within the default count budgets for the command's kind. If extra work
   is inherent, pin upper bounds in ``kindSmokeBudgetPins`` (or exact counts in
   ``perfBudgetPins`` in ``pw_ghish/perfbudget_test.go``) or set
   ``ghish:budget-exempt`` (``BudgetExemptAnnotation``) with a reason, and
   justify the budget in the CL description.
#. **Classify flags and update docs**: Add a ``//ghish:flag`` directive above
   every new flag registration (see :ref:`module-pw_ghish-flag-compatibility`),
   document new flags and subcommands in the corresponding ``pw_ghish/*.rst``
   reference page, and ensure ``--json <fields>`` flags reject unknown fields
   with ``UnknownJSONFieldsError`` (``Renderer`` handles this automatically;
   see :ref:`module-pw_ghish-developing-json-contract`).

---------------------------------------
Testing UI, JSON, and performance rules
---------------------------------------

.. _module-pw_ghish-developing-kind-smoke:

Kind smoke harness
==================
Adding a ``kindSmokeArgs`` entry in ``pw_ghish/kind_contract_test.go``
automatically tests your command in a hermetic fixture
(``pw_ghish/kind_fixture_test.go``) providing a temporary git repository on
change ``12345``, a mock Buganizer issue ``345678``, mock Gerrit/Buildbucket/Issue
Tracker servers, and an isolated ``wt`` pool with ``PATH`` restricted to ``git``
and non-loopback HTTP blocked.

Each command is checked against its kind's contract and default budgets:

.. list-table::
   :header-rows: 1

   * - Kind
     - Output contract
     - Default budget (bytes / subprocesses / RPCs)
   * - ``mutation``
     - A ``PrintReceipt`` receipt (``✓ <Summary>`` with ``Next:``).
     - 1024 / 4 / 8
   * - ``view``
     - A ``Verdict:`` line.
     - 2048 / 2 / 6
   * - ``list``
     - A ``Next:`` line.
     - 2048 / 2 / 8

Every command is also checked for decorative noise and duplicate identical HTTP
requests or subprocesses. Do not add entries to ``kindSmokeContractGaps``,
``kindSmokeDuplicateSubprocesses``, or ``kindSmokeSkip`` for new code; fix the
command instead.

.. _module-pw_ghish-developing-golden:

Golden snapshots
================
When adding or changing major user-facing output layouts, add or update a case
in ``pw_ghish/golden_test.go`` (snapshots live under
``pw_ghish/testdata/golden/<case>.expected``). Keep fixtures deterministic by
using fixed timestamps and stable ordering.

When you intentionally change command output, regenerate the snapshots and
review the resulting ``.expected`` diff alongside your code change:

.. code-block:: console

   $ bazelisk run //pw_ghish:pw_ghish_test -- -test.run TestGolden -update

Delete stale ``.expected`` files manually if you remove or rename a test case.

.. _module-pw_ghish-developing-json-contract:

JSON output contract
====================
Every command with a ``--json`` flag is automatically tested via its
``kindSmokeArgs`` entry in ``pw_ghish/json_contract_test.go``. When adding or
modifying ``--json`` output, follow three rules:

* **Single JSON value on stdout**: Write only the JSON payload to
  ``cmd.OutOrStdout()``. Send progress, warnings, or receipts to
  ``cmd.ErrOrStderr()`` (or omit receipts in JSON mode).
* **Bare ASCII enum values**: Enum-like fields (such as ``status``, ``state``,
  ``bucket``, ``kind``, ``residency``, ``mode``, ``priority``, or keys ending in
  ``_status``, ``_state``, ``Status``, ``State``, or ``Badge``) must contain
  bare ASCII identifiers (``"LOCAL_WIP"``), never terminal glyphs
  (``"● LOCAL_WIP"``). Attach glyphs only in text rendering (for example via
  ``StatusBadge.Display``).
* **Non-null empty collections**: Initialize empty slices and maps so they
  marshal as ``[]`` and ``{}``, never ``null``.

Performance budgets
===================
Performance tests in ``pw_ghish/perfbudget_test.go`` and
``pw_ghish/worktree/perfbudget_test.go`` pin deterministic counts (constructed
subprocesses via ``ExecCommand`` and HTTP requests), never wall-clock time.

When a budget test fails, the failure output lists every recorded subprocess or
HTTP request:

* Prefer removing the extra work by memoizing on ``Config`` / ``GitClient``,
  batching Gerrit queries, or reusing prefetched state.
* If a count change is intentional (including a decrease that triggers the
  stale-budget check), update the pin in ``perfbudget_test.go`` or
  ``kindSmokeBudgetPins`` in the same CL and explain why in the CL description.

To run informational wall-clock benchmarks locally against production Gerrit:

.. code-block:: console

   $ go test -v -tags=live ./pw_ghish -run TestLiveBench

Set ``GH_ISH_BIN`` to benchmark a prebuilt binary and
``GH_ISH_PERF_UPDATE_BASELINE=1`` to update
``pw_ghish/testdata/perf_baseline.json``.

--------------------
Contributor sections
--------------------
* :ref:`Compat policy <module-pw_ghish-flag-compatibility>`: Rules for aligning
  flags with upstream GitHub CLI, unbound single-letter shorthands, and the
  checklist for adding new flags.
* :ref:`Agent evaluation <module-pw_ghish-agent-eval>`: Live integration test
  runbook (``live_test.go``) and coding agent behavioral evaluation scenarios.
* :ref:`Status & roadmap <module-pw_ghish-roadmap>`: Current subcommand status
  and planned work for multi-project configuration.
* `Source code <https://cs.opensource.google/pigweed/pigweed/+/main:pw_ghish/>`_:
  Browse the ``pw_ghish`` Go implementation in Pigweed Code Search.
* `Open issues <https://issues.pigweed.dev/issues?q=pw_ghish%20status:open>`_:
  View open ``pw_ghish`` issues in Buganizer.
