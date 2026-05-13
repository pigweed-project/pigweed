.. _module-pw_ide-contributing-design-projects:

========
Projects
========
.. pigweed-module-subpage::
   :name: pw_ide

Pigweed projects support :ref:`multiple build systems
<docs-build-systems-overview>` and two methods of providing toolchains and a
hermetic developer environment:

* **Bazel**, in which Bazel is the build system, Bazel manages development
  environment dependencies, and the ``bazel`` (or ``bazelisk``) command is the
  entry point into all tooling

* **Bootstrap**, in which a development environment is populated by sourcing
  binary dependencies from CIPD, and is accessed by setting environment
  variables via ``bootstrap`` and ``activate`` shell scripts. Usually, the build
  system is GN or CMake.

Pigweed IDE supports either or both of these in a project. For example, Pigweed
itself has a GN build, supported by Bootstrap tooling, as well as a Bazel build,
supported by Bazel tooling. Pigweed IDE will provide code intelligence for both.

.. _module-pw_ide-contributing-design-projects-project-root:

-----------------
Project structure
-----------------
Pigweed IDE looks for the presence of a ``pigweed.json`` file to indicate the
root of the project directory.

.. _module-pw_ide-contributing-design-projects-bootstrap:

Bootstrap projects
==================
This project structure has the following properties:

* Development tools and dependences are located in an ``environment`` directory.

.. note::

   The location and name of the ``environment`` can be actually customized when
   running ``bootstrap.sh``, and multiple independent environments are
   supported.

* Access to the development environment is provided through mutations to the
  shell environment; the ``$PATH`` is modified to point to locations in
  ``environment``, and Pigweed-specific environment variables are used to
  configure the development environment.

* A ``bootstrap.sh`` script creates the ``environment`` directory, populates it
  with development tools and dependencies, and modifies the environment of the
  shell in which the script was run to provide access to the development
  environment.

* As a side effect of the bootstrap process, an ``actions.json`` file is created
  in the ``environment`` directory that describes the mutations to the shell
  environment required to provide access to the development environment.

* Pigweed commands are made available via an executable called ``pw``.

Assuming the environment has already been bootstrapped, exposing the development
environment to an IDE or other developer tooling requires:

* Finding the ``environment`` directory, or potentially multiple ``environment``
  directories. We do this by looking for directories with an ``actions.json``
  file.

* Executing commands within a shell environment modified per the
  ``actions.json`` file. How this is done depends on the language and runtime
  of the IDE. An example in JavaScript:

.. code-block:: js

   // Assuming `actionsJson` is a parsed `actions.json` file, and `activateEnv`
   // is a function that applies the mutations described in `actionsJson` to
   // a shell environment.
   const activatedEnv = activateEnv(process.env, actionsJson);
   child_process.exec("clang", activatedEnv);

If the environment has not been bootstrapped yet, we just need to run
``bootstrap.sh``. It's not necessary to capture the mutated shell environment
that results from running the script, since we have access to it through the
steps described above.

.. _module-pw_ide-contributing-design-projects-bazel:

Bazel projects
==============
This project structure has the following properties:

* Development tools and dependencies are managed by Bazel.
  `bzlmod <https://docs.bazel.build/versions/5.1.0/bzlmod.html>`_ uses a few
  forms of version resolution to ensure all tooling and external resources are
  available when they're needed. Regardless of the mechanism used, Bazel manages
  the tools and dependencies, and their location on disk.

* The Bazel environment is created and updated by running a Bazel build command.

* Access to tools and commands in the development environment is provided by
  Bazel itself. In other words, the ``bazel`` executable is the entry point to
  all project operations.

So all that's required to expose the development environment to an IDE is to
execute ``bazel run`` commands.

Managing Bazel environments
---------------------------
It's important to use the version of Bazel specified by the project, and to use
a single Bazel environment and build directory regardless of the tools the
developer is using. For example, a developer should be able to edit code in
their IDE and run Bazel commands from a separate terminal and be sure that all
of those actions will execute within the same environment.

We ensure that behavior by using ``bazelisk`` instead of plain ``bazel``.
It wraps ``bazel`` and ensures that your project uses the exact version of Bazel
specified in the project's ``.bazelversion`` file.

A developer can have multiple installations of ``bazelisk`` from different
sources, and invocations of any of those executables on the same project will
all run in the same Bazel environment. So we can actually bundle ``bazelisk``
with the IDE, ensuring that it's available to IDE functionality, without
worrying about it conflicting with the developer's use of ``bazelisk`` in the
terminal or in other tools.
