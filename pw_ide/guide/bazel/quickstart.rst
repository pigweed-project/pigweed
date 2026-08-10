.. _module-pw_ide-bazel-quickstart:

===============
Getting Started
===============

.. _Pigweed extension: https://marketplace.visualstudio.com/items?itemName=pigweed.pigweed

All you need to do is install the `Pigweed extension`_ from the extension
marketplace. It is also available on `Open VSX`_ for VS Code forks like
Cursor, AntiGravity, etc.

.. _Open VSX: https://open-vsx.org/extension/pigweed/pigweed

If you start your project from one of Pigweed's quickstart or
showcase example projects, you will be prompted to install the extension as soon
as you open the project.

Once installed, use the preconfigured targets in your project to get code intelligence.

Preconfigured targets
=====================
To get code intelligence, define target patterns in your build file that specify
which targets to generate compilation databases for (e.g., for a shared team
configuration or CI).

1. **Define a generator target**: Add a ``pw_compile_commands_generator`` target
   to your ``BUILD.bazel`` file. You can specify C/C++ target patterns for
   ``clangd`` and Rust target patterns for ``rust-analyzer``.

   .. code-block:: bazel

      load(
          "@pigweed//pw_ide/bazel/compile_commands:pw_compile_commands_generator.bzl",
          "pw_compile_commands_generator",
      )

      pw_compile_commands_generator(
          name = "refresh_compile_commands",
          target_patterns = [
              "//...",
          ],
          rust_target_patterns = [
              "//...",
          ],
      )

   .. tip::

      You can also specify specific platforms, build configs, or dependencies.
      See :ref:`module-pw_ide-bazel-usage` for advanced
      configuration.

2. **Refresh compile commands**: Run the target you created:

   .. code-block:: bash

      bazel run //:refresh_compile_commands

3. **Select the target**: Open the **Pigweed** extension panel from the Visual
   Studio Code Activity Bar. Under **Select or generate compile commands**,
   click **Generate** next to your configured C++ and Rust target platforms to
   generate their compilation databases and enable code intelligence.

You can also select target platforms from the status bar items at the bottom
of your window or by running the ``Pigweed: Select Code Analysis Target``
command.

Once you select a target platform, your editor will be automatically configured
to use the generated compile commands for C/C++ (``clangd``) and Rust
(``rust-analyzer``).

Other IDEs
==========
If you use an editor other than VS Code (e.g. Vim, Emacs, etc.), you can still
benefit from Pigweed's code intelligence by using the preconfigured targets
workflow.

1. **Generate compile commands**: Follow the steps in `Preconfigured targets`_
   above to configure and generate your compilation databases.

2. **Configure language servers**: Point your editor's ``clangd`` configuration
   to the generated ``compile_commands.json`` file, and your ``rust-analyzer``
   configuration to the generated ``rust-project.json`` file. You can find these
   files in the ``.compile_commands/`` directory in your project root after
   running the generator target.
