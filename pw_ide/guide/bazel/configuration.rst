.. _module-pw_ide-bazel-configuration:

=====================
Extension enforcement
=====================
.. pigweed-module-subpage::
   :name: pw_ide

.. _module-pw_ide-bazel-configuration-enforcement:

One of Pigweed's most powerful benefits is that it provides all developers on
a project team with a consistent development environment. Some teams want to
extend this consistency to the IDE by declaring that a certain set of extensions
should be enabled (or disabled) when working on their project.

Visual Studio Code provides two mechanisms for indicating that users *should*
have certain extensions:

#. Extensions can have other extensions as dependencies, such that installing
   the extension also installs its dependencies.

#. Projects can include *recommendations* in ``.vscode/extensions.json``.

The problem with the first option is that it is only available to extensions.
Projects can't declare extension dependencies.

The problem with the second option is that they are just *recommendations*, and
there is no built-in mechanism to enforce them or even install them by default.

Because there's not a natural way to achieve the goal of editor consistency
natively in Visual Studio Code, the Pigweed extension provides an additional
mechanism that teams can use instead.

-----
Usage
-----
Just enable ``pigweed.enforceExtensionRecommendations``.

Now, extensions listed under ``recommendations`` in the project's
``.vscode/extensions.json`` will need to be installed and enabled for the
project, and the user will be prompted to do so until all of the extensions are
installed and enabled, or until the user manually disables the notification.

.. tip::

   Although installed extensions are available globally, they don't need to be
   enabled in other projects or instances of Visual Studio Code.

Likewise, extensions listed under ``unwantedRecommendations`` will need to be
uninstalled or disabled, and the user will be prompted to do so until all of the
extensions are installed and enabled, or until the user manually disables the
notification.

.. tip::

   Unwanted recommendations don't need to be uninstalled if you use them in
   other projects. Just disable them for this project.

.. _module-pw_ide-bazel-configuration-disable-compile-commands-file-watcher:
.. _module-pw_ide-bazel-configuration-disable-inactive-file-code-intelligence:
.. _module-pw_ide-bazel-configuration-hide-inactive-file-indicators:
.. _module-pw_ide-bazel-configuration-project-type:
.. _module-pw_ide-bazel-configuration-project-root:

----------------
Other Settings
----------------

.. list-table::
   :widths: 40 60
   :header-rows: 1

   * - Setting
     - Description
   * - ``pigweed.disableCompileCommandsFileWatcher``
     - Disables automatic file watcher that refreshes compile commands.
   * - ``pigweed.disableInactiveFileCodeIntelligence``
     - Disables code intelligence for inactive files.
   * - ``pigweed.hideInactiveFileIndicators``
     - Hides inactive file indicators (graying out files).
   * - ``pigweed.projectType``
     - Explicitly set the project type (e.g., ``bazel`` or ``bootstrap``).
   * - ``pigweed.projectRoot``
     - Explicitly set the project root directory.
