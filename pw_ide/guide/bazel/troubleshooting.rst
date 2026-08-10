.. _module-pw_ide-bazel-troubleshooting:

===============
Troubleshooting
===============
.. pigweed-module-subpage::
   :name: pw_ide

This doc provides troubleshooting advice specific to the Pigweed Visual Studio
Code extension.

.. _project-type:

----------------------------
Can't determine project type
----------------------------
The Pigweed extension wasn't able to infer whether your Pigweed project is a
:ref:`Bazel project<module-pw_ide-contributing-design-projects-bazel>` or a
:ref:`bootstrap project<module-pw_ide-contributing-design-projects-bootstrap>`. This may
happen if you have an unusual project structure.

You can resolve it by :ref:`explicitly setting the project type<module-pw_ide-bazel-configuration-project-type>`
in your settings.

.. _project-root:

-----------------------
Can't find project root
-----------------------
The Pigweed extension wasn't able to infer the path to your
:ref:`Pigweed project root<module-pw_ide-contributing-design-projects-project-root>`. This
may happen if you have an unusual project structure.

You can resolve it by :ref:`explicitly setting the project root<module-pw_ide-bazel-configuration-project-root>`
in your settings.

.. _failed_to_refresh_code_intelligence:

----------------------------------------
Failed to refresh code intelligence data
----------------------------------------
The Pigweed extension failed to refresh the compilation databases
(``compile_commands.json`` for C/C++ or ``rust-project.json`` for Rust) used to
provide code intelligence. Some troubleshooting steps:

* Check the output panel (``Pigweed: Open Output Panel``) to find more specific
  information about what went wrong.

* Verify that the Bazel ``refresh_compile_commands`` target is configured
  properly in the top-level ``//BUILD.bazel`` file. There will be relevant
  content in the output panel if this is not configured correctly.

.. _bazel_no_targets:

---------------------------------
Couldn't find any targets (Bazel)
---------------------------------
The Pigweed extension couldn't find any Bazel targets or target platforms to use
for code intelligence. Here are some troubleshooting steps to follow:

Check the output panel for errors that may have occurred
========================================================
Run ``Pigweed: Open Output Panel`` from the command palette. You should see the
output from the most recent attempt to generate compile commands. If you *don't*
see that output, you can manually start the process by running
The most common failure here is actually an upstream failure in the Bazel build,
like missing dependencies or invalid Starlark files. Resolve those problems,
and the refresh process should restart automatically.

Confirm that the ``refresh_compile_commands`` target is correctly defined
=========================================================================
The top-level ``BUILD.bazel`` file contains a call to
``pw_compile_commands_generator`` that defines the compile commands generator
target. Any syntax or configuration errors in that call will prevent the refresh
process from running successfully.

Most importantly, ensure that ``target_patterns``, ``rust_target_patterns``, or
``deps`` attributes are defined. At least one must be present to generate code
intelligence data.
