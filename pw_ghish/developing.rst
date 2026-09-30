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
* **Commit trailer preservation**: Commands that modify commit messages (such as
  ``pr edit`` and ``issue create --amend``) parse and preserve existing Git
  trailers (``Change-Id:``, ``Bug:``, ``Fixed:``, ``Reviewed-on:``).
* **Actionable error messages**: Errors state what failed, why (the precondition
  or server state), the exact command to fix it, and an inspection command to
  check current state.

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
