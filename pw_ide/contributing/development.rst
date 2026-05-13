.. _module-pw_ide-contributing-development:

===========
Development
===========
.. pigweed-module-subpage::
   :name: pw_ide

This guide covers how to set up a development environment to build, run, and
debug the Pigweed Visual Studio Code extension.

The extension is written in TypeScript and bundled using webpack. It uses
standard Node.js tooling (``npm``) for development, not the main Pigweed build
systems (Bazel, GN). While there are Bazel build files for integration purposes,
you will not use them for active development.

-------------
Prerequisites
-------------
- `Node.js <https://nodejs.org/>`_ (which includes ``npm``)
- `Visual Studio Code <https://code.visualstudio.com/>`_

-----
Setup
-----
The development workflow for the Pigweed VSCode extension is self-contained
within its source directory.

1. **Open the Extension Directory in VSCode**

   For the best experience, you should open the extension's specific directory
   in VSCode, not the root of the Pigweed repository. This ensures that VSCode
   loads the correct settings and launch configurations for the extension.

   Open the following directory in VSCode:

   .. code-block:: shell

      <pigweed-repo-root>/pw_ide/ts/pigweed_vscode

2. **Install Recommended Extensions**

   When you open the directory, VSCode may prompt you to install recommended
   extensions. Please do so, as they provide essential tooling for TypeScript
   development and for matching the project's code style.

   One of the key recommended extensions is the
   `TypeScript + Webpack Problem Matchers
   <https://marketplace.visualstudio.com/items?itemName=amodio.tsl-problem-matcher>`_.
   **This is required for the build and debug process to work correctly.**

3. **Install Dependencies**

   Open a terminal within VSCode (**View > Terminal**) and install the required
   ``npm`` packages (if using external terminal, ``cd pw_ide/ts/pigweed_vscode``
   first):

   .. code-block:: shell

      npm install

----------------------------------
Building and Running the Extension
----------------------------------
The most effective way to work on the extension is to use the provided launch
configuration, which handles building, bundling, and launching a new VSCode
instance for testing.

1. **Open the Run and Debug View**

   In the Activity Bar on the left side of the VSCode window, click the "Run and
   Debug" icon (it looks like a play button with a bug).

2. **Start the "Run Extension" Configuration**

   At the top of the "Run and Debug" panel, you will see a dropdown menu.
   Select **Run Extension** from the list and press the green play button (or
   press **F5**).

   This command will:
    - Compile the TypeScript code.
    - Start a webpack watcher that will automatically rebuild the extension
      whenever you save a file.
    - Launch a new VSCode window called the **Extension Development Host**. This
      window has your development version of the Pigweed extension installed.

3. **Verify the Extension is Running**

   In the **Extension Development Host** window, you can verify that your
   extension is running by opening the Command Palette (``Cmd+Shift+P`` or
   ``Ctrl+Shift+P``) and typing "Pigweed". You should see the commands
   contributed by the extension, like "Pigweed: Open Output Panel".

---------
Debugging
---------
You can set breakpoints directly in your TypeScript source code in your main
VSCode window. When you perform an action in the **Extension Development Host**
window that triggers that code, the debugger will pause, and you can inspect
variables and step through the code.

---------------
Troubleshooting
---------------

"Command is already registered" errors on reload
================================================
When using the **Developer: Reload Window** command in the Extension
Development Host, you may encounter errors in the debug console like:

.. code-block:: text

   Error: command 'pigweed.file-bug' already exists

This happens because VSCode's reload functionality does not always fully clean
up the state from the previous activation, especially for commands and
configuration properties defined in ``package.json``. When the extension
activates again, it tries to re-register these, causing a conflict.

The most reliable way to test changes is to completely restart the Extension
Development Host session:

1. Stop the "Run Extension" task in your main VSCode window (the red square).
2. Relaunch it by pressing **F5**.

This ensures a clean environment for each run. While **Reload Window** can be
faster, it's prone to these state-related issues.

"The task 'npm: watch:bundle' has not exited and doesn't have a 'problemMatcher' defined."
==========================================================================================
This error occurs if the **TypeScript + Webpack Problem Matchers** extension is
not installed or enabled. The project's ``.vscode/tasks.json`` file is
configured to use a problem matcher from this extension (``$ts-webpack-watch``)
to correctly parse build output.

To fix this:

1. Go to the **Extensions** view in VSCode.
2. Search for ``@recommended`` to see workspace recommendations.
3. Find **TypeScript + Webpack Problem Matchers** and ensure it is installed
   and enabled.
4. Restart VSCode and try launching the extension again.

-------
Testing
-------
The extension has both unit and end-to-end tests. You can run these from the
command line in the ``pw_ide/ts/pigweed_vscode`` directory after bootstrapping
(``. ./bootstrap.sh`` in the Pigweed root):

Unit Tests
==========
.. code-block:: shell

   npm run test:unit

End-to-End Tests
================
.. code-block:: shell

   npm run test:e2e

All Tests
=========
.. code-block:: shell

   npm run test:all

-----------------------
Packaging the Extension
-----------------------
To create a ``.vsix`` package file for distribution, run the following command:

.. code-block:: shell

   npm run package

This will generate a file like ``pigweed-1.3.3.vsix`` in the current directory.
