.. _module-pw_ide-contributing-testing:

============================
Manual Test Procedure
============================
.. pigweed-module-subpage::
   :name: pw_ide

This document outlines manual test procedures for ``pw_ide``.

Add note to the commit message
==============================

Add a ``Testing:`` line to your commit message and mention the steps
executed. For example:

.. code-block:: text

   Testing: First Start Experience Steps 1-8

Test Sections
=============

First Start Experience
^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | Clean up any existing VSCode, clangd, or Bazel artifacts:
       | ``rm -rf .vscode .compile_commands .clangd external && bazel clean``
     - | Workspace is cleaned.
     - |checkbox|

   * - 2
     - | Open the ``pw_ide/ts/pigweed_vscode`` directory in VSCode.
     - | The project opens in VSCode.
     - |checkbox|

   * - 3
     - | In the VSCode terminal, run:
       | ``npm install``
     - | Dependencies are installed successfully.
     - |checkbox|

   * - 4
     - | In the VSCode terminal, run:
       | ``npm run watch:bundle``
     - | The extension bundle is built and watch mode starts.
     - |checkbox|

   * - 5
     - | Press :kbd:`F5` or select :guilabel:`Run > Start Debugging` from the menu.
     - | A new VSCode instance (Extension Development Host) launches.
       | The ``pw_ide`` extension is running in this new instance.
     - |checkbox|

   * - 6
     - | If prompted to update clangd, proceed with the update.
     - | Clangd is updated to the required version.
       | (If not prompted, clangd is already up-to-date).
     - |checkbox|

   * - 7
     - | In the Extension Development Host VSCode instance, open your main Pigweed project directory (the root of the Pigweed repository).
     - | The Pigweed project opens.
     - |checkbox|

   * - 8
     - | Observe ``.cc`` and ``.hpp`` files in the file explorer.
     - | All ``.cc`` and ``.hpp`` files initially appear inactive (e.g., grayed out or with a specific icon indicating they are not yet part of an active compilation database).
     - |checkbox|

Bazel Build and C/C++ Intellisense Verification
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These steps assume you have completed the "First Start Experience" steps.

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | In the Extension Development Host VSCode instance, open the Command Palette:
       | macOS: :kbd:`Cmd-Shift-P`
       | Linux/Windows: :kbd:`Ctrl-Shift-P`
       | Type ``Pigweed: Activate Bazelisk Terminal`` and select it.
     - | A new terminal opens with the Pigweed environment activated.
       | The terminal prompt may indicate the Pigweed environment (e.g., shows ``(pigweed)`` or similar decorators).
     - |checkbox|

   * - 2
     - | In the newly activated Bazelisk terminal, run:
       | ``bazelisk build //pw_rpc``
     - | The ``bazelisk build //pw_rpc`` command completes successfully.
       | Compile commands (e.g. ``compile_commands.json``) are generated or updated in the workspace root.
     - |checkbox|

   * - 3
     - | In the VSCode file explorer, navigate to the ``pw_rpc`` directory.
       | Observe the appearance of ``.cc`` and ``.h`` files within ``pw_rpc``.
       | Open a file like ``pw_rpc/public/pw_rpc/server.h``.
     - | ``.cc`` and ``.h`` files within ``pw_rpc`` should now appear active (not grayed out).
       | Code intelligence features should be functional:
       | - Includes are recognized (no unexpected squiggles under Pigweed or standard library headers).
       | - Hovering over symbols provides tooltips with type information.
       | - "Go to Definition" (e.g., right-click a symbol) navigates correctly.
     - |checkbox|

Manual Bazel compile command database generation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
These checks ensure manually-initiated compile commands database generation
works as intended.

.. admonition:: Note

   These steps assume you have completed the "First Start Experience" steps.

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | In the Extension Development Host VSCode instance, open the Command Palette:
       | macOS: :kbd:`Cmd-Shift-P`
       | Linux/Windows: :kbd:`Ctrl-Shift-P`
       | Type ``Pigweed: Activate Bazelisk Terminal`` and select it.
     - | A new terminal opens with the Pigweed environment activated.
       | The terminal prompt may indicate the Pigweed environment (e.g., shows ``(pigweed)`` or similar decorators).
     - |checkbox|

   * - 2
     - | In the newly activated Bazelisk terminal, run:
       | ``bazelisk run @pigweed//pw_ide/bazel:update_compile_commands -- -- build //pw_json/...``
     - | The ``bazelisk run`` command completes successfully and a compile
       | commands directory (``.compile_commands``) is generated or updated in
       | the workspace root.
     - |checkbox|

   * - 3
     - | In the VSCode file explorer, navigate to the ``pw_json`` directory.
       | Observe the appearance of ``.cc`` files within the ``pw_json``
       | directory (e.g. ``pw_json/builder_test.cc``).
     - | ``.cc`` files within ``pw_json`` should now appear active
       | (NOT grayed out, and NO "ℹ️" indicator).
       | Code intelligence features should be functional:
       | - Includes are recognized (no unexpected squiggles under Pigweed or standard library headers).
       | - Hovering over symbols provides tooltips with type information.
       | - "Go to Definition" (e.g., right-click a symbol) navigates correctly.
     - |checkbox|

Multi-platform compile command database generation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
These checks ensure multi-platform handling of compile commands databases works
as expected.

This check should be run when making changes to the Bazel interceptor script
generation logic in ``pw_ide/ts/pigweed_vscode/src/clangd/compileCommandsUtils.ts``.

.. admonition:: Note

   These steps assume you have completed the "First Start Experience" steps.

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | In the Extension Development Host VSCode instance, open the Command Palette:
       | macOS: :kbd:`Cmd-Shift-P`
       | Linux/Windows: :kbd:`Ctrl-Shift-P`
       | Type ``Pigweed: Activate Bazelisk Terminal`` and select it.
     - | A new terminal opens with the Pigweed environment activated.
       | The terminal prompt may indicate the Pigweed environment (e.g., shows ``(pigweed)`` or similar decorators).
     - |checkbox|

   * - 2
     - | In the newly activated Bazelisk terminal, run:
       | ``bazelisk build //pw_system:rp2040_system_example``
     - | The ``bazelisk build`` command completes successfully and a compile
       | commands directory (``.compile_commands``) is generated or updated in
       | the workspace root.
     - |checkbox|

   * - 3
     - | Click the platform selection button in the VSCode
       | `status bar <https://code.visualstudio.com/api/ux-guidelines/status-bar>`__
       | at the bottom of the window.
     - | ``rp2040_pw_system_demo-fastbuild`` should appear in the list.
     - |checkbox|

   * - 4
     - | Select ``rp2040_pw_system_demo-fastbuild`` from the popup list.
     - | Some files should become inactive as the new compile commands load.
     - |checkbox|

   * - 5
     - | In the VSCode file explorer, navigate to the ``targets/rp2040`` directory.
       | Observe the appearance of ``targets/rp2040/boot.cc`` files within the
       | directory.
     - | ``targets/rp2040/boot.cc`` should now appear active
       | (NOT grayed out, and NO "ℹ️" indicator).
       | Code intelligence features should be functional:
       | - Includes are recognized (no unexpected squiggles under Pigweed or standard library headers).
       | - Hovering over symbols provides tooltips with type information.
       | - "Go to Definition" (e.g., right-click a symbol) navigates correctly.
     - |checkbox|

Fish Shell Verification
^^^^^^^^^^^^^^^^^^^^^^^
Ensures IntelliSense works when users have defaulted to fish as their terminal
profile of choice.

This check should be run when making changes to the Bazel interceptor script
generation logic in ``pw_ide/ts/pigweed_vscode/src/clangd/compileCommandsUtils.ts``.

.. admonition:: Note

   These steps assume you have completed the "First Start Experience" steps.

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | Change your default shell to ``fish``.
       | In the Extension Development Host, open a new "Pigweed: Activate Bazelisk Terminal".
       | Run: ``bazelisk build //pw_log``
     - | The build completes successfully.
       | Files in ``pw_log`` should become active.
       | Intellisense should work for files like ``pw_log/public/pw_log/log.h``.
     - |checkbox|

Clangd Argument Restoration
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This test verifies that the extension automatically corrects the ``clangd``
arguments in the VSCode settings if they are missing or incorrect, both upon
activation and after a build.

.. list-table::
   :widths: 5 45 45 5
   :header-rows: 1

   * - #
     - Test Action
     - Expected Result
     - ✅

   * - 1
     - | Follow the "First Start Experience" and "Bazel Build..." sections to build a target (e.g., ``//pw_rpc``).
     - | A ``.vscode/settings.json`` file is created with a ``clangd.arguments`` setting.
     - |checkbox|

   * - 2
     - | Open ``.vscode/settings.json`` in the editor.
     - | The ``clangd.arguments`` setting is visible and contains several arguments (e.g., ``--compile-commands-dir``).
     - |checkbox|

   * - 3
     - | Delete the entire ``clangd.arguments`` line from ``.vscode/settings.json`` and save the file.
     - | The setting is removed.
     - |checkbox|

   * - 4
     - | In the Bazelisk terminal, run the build command again:
       | ``bazelisk build //pw_rpc``
     - | The build completes.
       | The ``clangd.arguments`` setting is automatically restored in ``.vscode/settings.json``.
     - |checkbox|

   * - 5
     - | Delete the ``clangd.arguments`` line from ``.vscode/settings.json`` again and save.
     - | The setting is removed.
     - |checkbox|

   * - 6
     - | Close the "Extension Development Host" VSCode instance.
     - | The window closes.
     - |checkbox|

   * - 7
     - | Press :kbd:`F5` in the main VSCode instance to start debugging again.
     - | A new "Extension Development Host" instance opens.
       | The ``clangd.arguments`` setting is automatically restored in ``.vscode/settings.json``.
     - |checkbox|

.. |checkbox| raw:: html

    <input type="checkbox">
