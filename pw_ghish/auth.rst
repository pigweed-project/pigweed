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
3. **Google Issue Tracker / Buganizer** (``issuetracker.corp.googleapis.com``
   or ``issuetracker.googleapis.com``)

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
       ``authenticated``, ``gerrit``, ``luci``, ``buganizer``).

------------------------------
Checking authentication status
------------------------------
Run ``./gh auth status`` to verify your credentials:

.. code-block:: console

   $ ./gh auth status
   Authentication Mode: googler (auto-detected (gob-curl on PATH))

   pigweed-review.googlesource.com (Gerrit)
     ✓ Logged in to pigweed-review.googlesource.com as Keir Mierle <keir@google.com> (account #1000020)
     - Method: gob-curl

   cr-buildbucket.appspot.com (LUCI Buildbucket & LogDog)
     ✓ Authenticated with cr-buildbucket.appspot.com via luci-auth (internal & public builders visible)
     - Method: luci-auth

   issuetracker.corp.googleapis.com (Google Issue Tracker)
     ✓ Authenticated with issuetracker.corp.googleapis.com via sso_client + luci-auth (quota project: pigweed-gce)
     - Method: sso_client + luci-auth
     - Quota project: pigweed-gce

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
     - **(Default)** Selects ``googler`` if ``gob-curl`` or ``sso_client`` is
       on ``PATH``, the Git ``user.email`` ends in ``@google.com``, or the
       ``origin`` remote uses ``sso://``; otherwise selects ``community``.
   * - ``googler``
     - Requires authenticated access to both Gerrit and LUCI Buildbucket so
       internal tryjob buckets (such as ``pigweed/internal.try``) are included
       in query results. Does not fall back to anonymous requests; if
       credentials are missing or expired, commands exit with code ``4`` and
       print remediation instructions.
   * - ``community``
     - Authenticates Gerrit via ``~/.gitcookies``, ``GERRIT_TOKEN``, or
       ``~/.netrc``, and uses ``luci-auth`` for LUCI when logged in. Permits
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

Gerrit Code Review
==================
When ``GH_ISH_AUTH_METHOD`` is ``auto`` (the default), ``pw_ghish`` looks up
Gerrit credentials in the following order:

1. ``GERRIT_TOKEN`` environment variable (HTTP Bearer or Basic token).
2. ``gob-curl`` on ``PATH``.
3. Git cookie file from ``git config http.cookiefile`` or ``~/.gitcookies``.
4. Machine credentials in ``~/.netrc`` or ``~/_netrc``.
5. Anonymous HTTPS (permitted in ``community`` and ``none`` modes for public
   reads; rejected in ``googler`` mode with exit code ``4``).

LUCI Buildbucket & LogDog
=========================
LUCI Buildbucket pRPC requests (``pr checks``, ``pr view``, ``pr status``,
``run view``, ``run list``, ``run rerun``) and LogDog log requests look up
OAuth2 tokens in the following order:

1. ``GHISH_LUCI_TOKEN`` or ``LUCI_TOKEN`` environment variable.
2. ``luci-auth token`` (located on ``PATH`` or in the repository's bootstrapped
   CIPD environment).
3. ``gcloud auth application-default print-access-token`` or
   ``gcloud auth print-access-token``.

In ``googler`` mode, an active LUCI OAuth2 token is required so that
``buildbucket.v2.Builds/SearchBuilds`` returns both public and internal CQ
builders. To log in or refresh your LUCI session:

.. code-block:: console

   $ luci-auth login

Google Issue Tracker (Buganizer)
================================
``./gh issue`` looks up credentials in the following order:

1. ``GHISH_ISSUE_TOKEN`` or ``BUGANIZER_TOKEN`` environment variable.
2. ``luci-auth token -scopes "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform"``
3. ``gcloud auth application-default print-access-token`` or
   ``gcloud auth print-access-token``.

When ``sso_client`` is on ``PATH``, requests use
``https://issuetracker.corp.googleapis.com/v1`` to access both public and
internal Buganizer components.

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
     - Outputs ``mode``, ``modeReason``, ``authenticated``, ``gerrit``,
       ``luci``, and ``buganizer`` status objects.
   * - ``--auth-mode``
     - *(n/a)* (``gh auth status`` binds ``-a`` to ``--active``).
     - Global **ghish-only** flag (``auto``, ``googler``, ``community``,
       ``none``). Leaves ``-a`` unbound to avoid colliding with ``--active``.
   * - ``auth login`` / ``logout``
     - Interactive OAuth flow managed directly by ``gh``.
     - Delegated to each backend's native credential tool (``gob-curl`` /
       ``.gitcookies``, ``luci-auth login``, and ``gcloud auth``);
       ``./gh auth status`` prints the exact login command when a service is
       unauthenticated.

For the flag compatibility policy across all subcommands, see
:ref:`module-pw_ghish-flag-compatibility`.
