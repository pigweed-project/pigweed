.. _module-pw_ide-contributing-design-cpp:

=======================
C/C++ code intelligence
=======================
.. pigweed-module-subpage::
   :name: pw_ide

Pigweed projects have a few characteristics that make it challenging for some
IDEs and language servers to support C/C++ code intelligence out of the box:

* Pigweed projects generally *don't* use the default host toolchain.

* Pigweed projects usually use *multiple* toolchains for separate build targets.

* Pigweed projects rely on the build system to define the relationship between
  :ref:`facades and backends<docs-module-structure-facades>` for each target.

These challenges are common to embedded projects in general.

We've found that the best solution is to use the
`clangd <https://clangd.llvm.org/>`_ language server or alternative language
servers that use the same
`compilation database format <https://clang.llvm.org/docs/JSONCompilationDatabase.html>`_
and comply with the language server protocol. Usually, your build system
generates the compilation database, and we supplement that with Pigweed
tools to produce target-specific compilation databases that work well with
language servers.

-------------------------------------------
Supporting ``clangd`` for embedded projects
-------------------------------------------
There are three main challenges that often prevent ``clangd`` from working
out-of-the-box with embedded projects:

#. Embedded projects cross-compile using alternative toolchains, rather than
   using the system toolchain. ``clangd`` doesn't know about those toolchains
   by default.

#. Embedded projects (particularly Pigweed projects) often have *multiple*
   targets that use *multiple* toolchains. Most build systems that generate
   compilation databases put all compile commands in a single database, meaning
   a single file can have multiple, conflicting compile commands. ``clangd``
   will typically use the first one it finds, which may not be the one you want.

#. Pigweed projects have build steps that use languages other than C/C++. These
   steps are not relevant to ``clangd`` but some build systems will include them
   in the compilation database anyway.

To deal with these challenges, ``pw_ide`` processes the compilation database(s)
you provide, yielding one or more compilation databases that are valid,
consistent, and specific to a particular target and toolchain combination.
This enables code intelligence and navigation features that accurately reflect
a specific build.

After processing a compilation database, ``pw_ide`` knows what targets are
available and provides tools for selecting which target the language server
should use.  Then ``clangd``'s configuration is changed to use the compilation
database associated with that target.

----------------------
Implementation details
----------------------

Bazel
=====
Bazel doesn't have native support for generating compile commands, so Pigweed
IDE uses Bazel queries to create ``clangd`` compile commands that reflect the
Bazel build graph.

.. _module-pw_ide-contributing-design-cpp-gn:

GN (Generate Ninja)
===================
Invoking GN with the ``--export-compile-commands`` flag (e.g.
``gn gen out --export-compile-commands``) will output a compilation database
in the ``out`` directory along with all of the other GN build outputs (in other
words, it produces the same output as ``gn gen out`` but *additionally* produces
the compilation database). You can remove the need to explicitly provide the
flag by adding this to your ``.gn`` file:
``export_compile_commands = [ ":*" ]``.

The database that GN produces will have all compile commands for all targets,
which is a problem for the reasons described above. It will also include
invocations of certain Pigweed Python scripts that are not valid C/C++ compile
commands and cause ``clangd`` to fault. So these files need to be processed to:

* Filter out invalid compile commands

* Separate compile commands into separate databases for each target

* Resolve relative paths so ``clangd`` can find tools in the Pigweed
   environment

Pigweed IDE does this for you.

.. _module-pw_ide-contributing-design-cpp-cmake:

CMake
=====

Enabling the ``CMAKE_EXPORT_COMPILE_COMMANDS`` option ensures that compilation
databases are produced along with the rest of the CMake build. This can be done
either in your project's ``CMakeLists.txt`` (i.e.
``set(CMAKE_EXPORT_COMPILE_COMMANDS ON)``), or by setting the flag when invoking
CMake (i.e. ``-DCMAKE_EXPORT_COMPILE_COMMANDS=ON``).

CMake will generate separate ``compile_commands.json`` files that are specific
to each target in the target's build output directory. These files are *already*
in the format we need them to be in, so the only thing Pigweed needs to provide
is a mechanism for discovering those targets and pointing ``clangd`` at the
appropriate file.
