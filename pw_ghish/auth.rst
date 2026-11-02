.. _module-pw_ghish-auth:

========================
Authentication (gh auth)
========================
.. pigweed-module-subpage::
   :name: pw_ghish

``./gh auth`` checks authentication status across the three backends used by
``pw_ghish``:

1. **Gerrit Code Review** (e.g., ``pigweed-review.googlesource.com``)
2. **LUCI Buildbucket & LogDog** (``cr-buildbucket.appspot.com``,
   ``logs.chromium.org``)
3. **Google Issue Tracker / Buganizer** (``issuetracker.corp.googleapis.com``)

-----------------
Command reference
-----------------
.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Command
     - Description
   * - ``./gh auth status``
     - Check authentication status across Gerrit, LUCI Buildbucket, and
       Buganizer. Exits ``0`` when all required services for the active mode
       are authenticated, or ``4`` (``ExitCodeAuth``) with remediation steps if
       required credentials are missing or expired.
   * - ``./gh auth status --json [<fields>]``
     - Output JSON authentication status (fields: ``mode``, ``modeReason``,
       ``authenticated``, ``healthy``, ``gerrit``, ``luci``, ``buganizer``).

------------------------------
Checking authentication status
------------------------------
Run ``./gh auth status`` to verify your credentials:

.. code-block:: console

   $ ./gh auth status
   Authentication Mode: googler (auto-detected (gosso on PATH))

   pigweed-review.googlesource.com (Gerrit)
     ✓ Logged in to pigweed-review.googlesource.com as Keir Mierle <keir@google.com> (account #1000020)
     - Method: token (luci-auth)

   cr-buildbucket.appspot.com (LUCI Buildbucket & LogDog)
     ✓ Authenticated with cr-buildbucket.appspot.com via luci-auth (internal & public builders visible)
     - Method: luci-auth

   issuetracker.corp.googleapis.com (Google Issue Tracker)
     ✓ Authenticated with issuetracker.corp.googleapis.com via gosso + luci-auth
     - Method: gosso + luci-auth

   Verdict: authenticated (googler mode; all required services healthy)
   Next:    gh pr status

Pass ``--json`` to output specific fields for scripts or tools:

.. code-block:: console

   $ ./gh auth status --json mode,authenticated,gerrit,luci

.. _module-pw_ghish-auth-modes:

--------------------
Authentication modes
--------------------
``pw_ghish`` supports four authentication modes via ``--auth-mode`` or
``GH_ISH_AUTH_MODE``:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Mode
     - Behavior
   * - ``auto``
     - **(Default)** Selects ``googler`` if ``gosso`` is on ``PATH``,
       ``/usr/bin/sso-cred-helper`` exists, the Git ``user.email``
       ends in ``@google.com``, or the ``origin`` remote uses ``sso://`` or
       ``rpc://``; otherwise selects ``community``.
   * - ``googler``
     - Requires authenticated access to both Gerrit and LUCI Buildbucket so
       internal tryjob buckets (such as ``pigweed/internal.try``) are included
       in query results. Does not fall back to anonymous requests; if
       credentials are missing or expired, commands exit with code ``4`` and
       print remediation instructions.
   * - ``community``
     - Authenticates Gerrit via ``GERRIT_TOKEN`` or (on ``*.googlesource.com``)
       ``luci-auth``, and uses ``luci-auth`` for LUCI when logged in. Permits
       unauthenticated read queries against public Gerrit changes and public
       LUCI Buildbucket buckets (such as ``pigweed/try``).
   * - ``none``
     - Uses unauthenticated HTTP requests for all services.

Configuration precedence
========================
The active authentication mode is resolved in the following order:

1. **CLI flag**: ``--auth-mode <auto|googler|community|none>``
2. **Environment variable**: ``GH_ISH_AUTH_MODE=<auto|googler|community|none>``
3. **Git configuration**: ``git config ghish.authmode <auto|googler|community|none>``
4. **Default**: ``auto``

---------------------
Credential resolution
---------------------

One login for every service
===========================
``pw_ghish`` accesses several separate backends—Gerrit, LUCI Buildbucket,
Android Build, and Google Issue Tracker—and uses a single ``luci-auth`` login
across them based on your :ref:`authentication mode <module-pw_ghish-auth-modes>`:

* ``googler``: Covers Gerrit, LUCI, Android Build, and Google Issue Tracker, so
  working across Pigweed and Android requires only one login.
* ``community``: Covers Gerrit and LUCI.
* ``none``: Disables authentication.

To log in:

* **Corp Linux**: Run ``gcert``. No separate ``luci-auth login`` step is needed.
* **Elsewhere (including macOS)**: Run ``./gh auth status``. It detects your
  mode and prints the exact ``luci-auth login`` command to run when login is
  needed (as do authentication errors from any ``./gh`` command).

If you are a Googler on a machine detected as ``community`` (for example,
without Corp tools on ``PATH`` and with a non-``@google.com`` ``user.email``),
set ``GH_ISH_AUTH_MODE=googler`` or
``git config --global ghish.authmode googler`` before logging in.

Gerrit Code Review & Git Push
=============================
``pw_ghish`` resolves Gerrit REST credentials in the following order:

1. ``GERRIT_TOKEN`` environment variable (OAuth2 Bearer token). A value
   containing ``:`` (a ``username:password`` pair for HTTP Basic auth) is
   rejected with exit code ``4``; HTTP Basic auth (used by most non-Google
   Gerrit servers) isn't supported yet.
2. Shared ``luci-auth`` credentials on Google-hosted Gerrit
   (``*.googlesource.com``; see `One login for every service`_). For other
   Gerrit hosts, set ``GERRIT_TOKEN``.
3. Anonymous HTTPS (permitted in ``community`` and ``none`` modes for public
   reads; rejected in ``googler`` mode with exit code ``4``).

.. note::

   ``GH_ISH_AUTH_METHOD`` has been removed. If it is set, commands (except
   ``./gh hook``) exit with code ``4`` and print replacement credential steps.
   To disable authentication, use ``--auth-mode=none`` or
   ``GH_ISH_AUTH_MODE=none``.

Community contributor onboarding
--------------------------------
To authenticate for creating, pushing, and reviewing changes outside Google
Corp:

1. **Provision your Gerrit account**: Sign in once at
   ``https://<host>-review.googlesource.com`` (for example,
   ``https://pigweed-review.googlesource.com``) in a web browser. Gerrit
   provisions a new user account on first web sign-in; REST and ``git push``
   requests before first web login return ``HTTP 400``.
2. **Sign the Google CLA**: Sign the Contributor License Agreement at
   ``https://cla.developers.google.com/`` and ensure ``git config user.email``
   matches.
3. **Log in with luci-auth**: Run ``luci-auth`` (bundled in
   ``environment/cipd/packages/luci/luci-auth`` via ``source bootstrap.sh`` /
   ``activate.sh``, or in ``depot_tools``) and verify with
   ``./gh auth status``:

   .. code-block:: console

      $ luci-auth login -scopes "https://www.googleapis.com/auth/gerritcodereview https://www.googleapis.com/auth/userinfo.email"
      $ ./gh auth status

4. **Push over HTTPS without cookies**: ``./gh pr create`` and ``./gh pr push``
   first push with your own git credentials. If a push to
   ``https://<tenant>.googlesource.com/`` is rejected for authentication, they
   retry once with the ``luci-auth`` (or ``GERRIT_TOKEN``) token in an
   ``Authorization: Bearer`` header, so ``~/.gitcookies`` is not required. No
   token is sent when ``--auth-mode=none``.

Automatic ``git push`` fallbacks (``pr create`` / ``pr push``)
--------------------------------------------------------------
On Google Corp workstations, ``gcert --lifetime=168h`` issues a 7-day
``LOAS2`` certificate (used by ``rpc://`` and ``sso-cred-helper``), whereas the
``corp/normal`` SSO ticket in ``ssh-agent`` (used by ``sso://``) is capped at 20
hours. When a push to an ``sso://<tenant>/...`` remote fails because the
20-hour SSO ticket has expired, ``pw_ghish`` automatically retries:

1. **Corp Linux (Cloudtop / gLinux)**: If ``git-remote-rpc`` is on ``PATH``,
   retries with ``-c url.rpc://<tenant>/.insteadOf=sso://<tenant>/`` using the
   7-day ``LOAS2`` certificate.
2. **Corp macOS & Linux**: If ``git-remote-rpc`` is unavailable (such as on
   Corp macOS) or ``rpc://`` also fails, mints a Gerrit OAuth2 token via
   ``luci-auth`` and retries over ``https://<tenant>.googlesource.com/`` with
   an ``Authorization: Bearer`` header.

LUCI Buildbucket & LogDog
=========================
LUCI Buildbucket and LogDog requests look up credentials in the following
order:

1. ``GHISH_LUCI_TOKEN`` or ``LUCI_TOKEN`` environment variable.
2. Shared ``luci-auth`` credentials (see `One login for every service`_).

In ``googler`` mode, LUCI authentication is required so queries include both
public and internal CQ builders. Run ``gcert`` on Corp Linux, or
``./gh auth status`` elsewhere to see the login command when needed.

Google Issue Tracker (Buganizer)
================================
``./gh issue`` looks up credentials in the following order:

1. ``GHISH_ISSUE_TOKEN`` or ``BUGANIZER_TOKEN`` environment variable.
2. Shared ``luci-auth`` credentials in ``googler`` mode (see
   `One login for every service`_). In ``community`` mode, set
   ``GHISH_ISSUE_TOKEN`` or ``BUGANIZER_TOKEN``.

``./gh issue`` requires Google Corp credentials; community contributors can
view and file issues on the web at ``https://issues.pigweed.dev``.

.. _module-pw_ghish-auth-comparison:

------------------------------------
Comparison with GitHub CLI (gh auth)
------------------------------------
.. list-table::
   :header-rows: 1
   :widths: 26 34 40

   * - Feature / Flag
     - Upstream ``gh auth``
     - ``./gh auth`` in ``pw_ghish``
   * - ``auth status``
     - Checks authentication against a single GitHub host (``github.com`` or
       GitHub Enterprise).
     - Checks credentials across three distinct backends: **Gerrit**, **LUCI
       Buildbucket**, and **Google Issue Tracker (Buganizer)**.
   * - ``auth status --json``
     - Outputs ``hosts`` status fields for ``github.com``.
     - Outputs ``mode``, ``modeReason``, ``authenticated``, ``healthy``,
       ``gerrit``, ``luci``, and ``buganizer`` status objects.
   * - ``--auth-mode``
     - *(n/a)* (``gh auth status`` binds ``-a`` to ``--active``).
     - Global **ghish-only** flag (``auto``, ``googler``, ``community``,
       ``none``). Leaves ``-a`` unbound to avoid colliding with ``--active``.
   * - ``auth login`` / ``logout``
     - Interactive OAuth flow managed directly by ``gh``.
     - Delegated to native credential tools (``luci-auth login`` and
       ``gcert``);
       ``./gh auth status`` prints the exact login command when a service is
       unauthenticated.

For the flag compatibility policy across all subcommands, see
:ref:`module-pw_ghish-flag-compatibility`.
