.. _module-pw_ide-bazel-rust:

======================
Rust code intelligence
======================
.. pigweed-module-subpage::
   :name: pw_ide

Pigweed IDE for Bazel supports Rust code intelligence
using `rust-analyzer <https://rust-analyzer.github.io/>`_.

Similar to C/C++ compilation database generation for ``clangd``, Pigweed IDE
generates ``rust-project.json`` project workspaces from your project's Bazel
build graph. This ensures that ``rust-analyzer`` understands your custom
embedded targets, dependencies, and build configurations without requiring
manual setup.

---------------------------------------
Configuring Rust targets in BUILD.bazel
---------------------------------------
To generate Rust code intelligence, define one or more
``pw_compile_commands_generator`` targets in your top-level ``BUILD.bazel``
file and specify ``rust_target_patterns``.

Example configuration:

.. code-block:: bazel

   load(
       "@pigweed//pw_ide/bazel/compile_commands:pw_compile_commands_generator.bzl",
       "pw_compile_commands_generator",
   )

   pw_compile_commands_generator(
       name = "update_compile_commands",
       deps = [
           ":update_host_kernel_compile_commands",
           ":update_rp2350_kernel_compile_commands",
       ],
   )

   pw_compile_commands_generator(
       name = "update_host_kernel_compile_commands",
       config = "k_host",
       display_name = "Host Kernel Rust",
       rust_target_patterns = [
           "//pw_kernel/...",
       ],
   )

   pw_compile_commands_generator(
       name = "update_rp2350_kernel_compile_commands",
       config = "k_rp2350",
       display_name = "RP2350 Kernel Rust",
       rust_target_patterns = [
           "//pw_kernel/...",
       ],
   )

Generator attributes for Rust
=============================
The ``pw_compile_commands_generator`` rule supports the following arguments for
Rust code intelligence:

* ``rust_target_patterns``: List of Bazel target patterns to analyze for
  generating ``rust-project.json``.

* ``config``: Optional Bazel build configuration name to use for compilation
  and platform inference (for example, ``k_host`` or ``k_rp2350``).

* ``bazel_args``: Optional list of extra Bazel command-line arguments used when
  evaluating target patterns or building platforms.

* ``display_name``: Optional human-readable label shown in the Visual Studio
  Code target selection panel.

-------------------------------------
Generating and selecting Rust targets
-------------------------------------
You can inspect, generate, and switch active Rust target platforms directly
from Visual Studio Code.

Target selection panel
======================
The recommended way to manage Rust compile commands is through the **Pigweed**
extension panel in Visual Studio Code (accessible from the Activity Bar icon).

Under **Select or generate compile commands**, locate the **Rust Targets**
table. Each row displays:

* **Target**: The display name of your configured Rust target (for example,
  ``Host Kernel Rust`` or ``RP2350 Kernel Rust``).
* **Last generated at**: A relative timestamp indicating when compile commands
  were last generated (for example, ``1 month ago`` or ``Never``).
* **Action**: A button to **Generate** (if never generated) or **Regenerate**
  the compile commands for that target.

Clicking **Generate** or **Regenerate** triggers generation of the
``rust-project.json`` workspace for that target and automatically selects it as
your active Rust target for editor code intelligence.

When a Rust target is selected, Pigweed IDE automatically:

#. Creates or updates a symlink from your project root's ``rust-project.json``
   to the generated target file in ``.compile_commands/<target>/``.

#. Configures Visual Studio Code's ``rust-analyzer.linkedProjects`` and
   ``rust-analyzer.check.overrideCommand`` settings for that target.

#. Reloads the ``rust-analyzer`` workspace.

Status bar and command palette
==============================
You can also view and change the currently-selected Rust target platform from
the Visual Studio Code status bar or by running
``Pigweed: Select Code Analysis Target`` from the command palette.

C++ and Rust target platforms are selected independently. You can have an active
C++ target (for ``clangd``) and an active Rust target (for ``rust-analyzer``) at
the same time.

------------------------------
Rust editor settings reference
------------------------------
Pigweed IDE uses the following settings to manage Rust code intelligence in
Visual Studio Code:

.. list-table::
   :widths: 40 60
   :header-rows: 1

   * - Setting
     - Description
   * - ``pigweed.rustAnalysisTarget``
     - The build target to use for editor Rust code intelligence
       (``rust-analyzer``).
   * - ``pigweed.rustAnalysisTargetDir``
     - The directory containing ``rust-project.json`` for the selected Rust
       target.

-------------
Other editors
-------------
If you use an editor other than Visual Studio Code (such as Neovim, Vim, or
Emacs), you can use the generated ``rust-project.json`` files with any editor
that supports ``rust-analyzer``.

#. Run your compile commands generator target from the command line:

   .. code-block:: bash

      bazel run //:update_compile_commands

#. Point your editor's ``rust-analyzer`` configuration to the generated
   ``.compile_commands/<target>/rust-project.json`` file for your desired target
   platform, or symlink it to ``rust-project.json`` in your project root.
