.. _docs-bazel-dependencies:

===============================================
Manage external dependencies in a Bazel project
===============================================
Bazel acts as both a build system and a package manager. This page explains the
core Bzlmod constructs used to declare, configure, and debug external
dependencies in a Bazel project. For Pigweed's broader dependency management
philosophy and architecture, see :ref:`docs-dependencies` and
:ref:`docs-build-dependency-architecture`.

.. _docs-bazel-dependencies-declare:

-----------------
Declare a project
-----------------
Bazel projects are defined as `Bazel modules
<https://bazel.build/external/module>`__ via a ``MODULE.bazel`` file at the
project root:

.. code-block:: bazel

   # MODULE.bazel
   module(name = "my_project", version = "0.0.1")

.. note::

   Older Bazel projects may use a ``WORKSPACE`` file instead of
   ``MODULE.bazel``. ``WORKSPACE`` is deprecated; new projects should use
   ``MODULE.bazel``.

.. _docs-bazel-dependencies-overview:

--------
Overview
--------
Every external resource—whether source code, language rules, or prebuilt
binaries—should be declared in ``MODULE.bazel`` and exposed to the build as an
isolated `repository <https://bazel.build/external/overview#repository>`__.
Bazel provides three mechanisms for managing external repositories:

- :ref:`docs-bazel-dependencies-overview-modules`
- :ref:`docs-bazel-dependencies-overview-extensions`
- :ref:`docs-bazel-dependencies-overview-repository-rules`

.. _docs-bazel-dependencies-overview-modules:

Bazel modules
=============
`Bazel modules <https://bazel.build/external/module>`__ are the standard unit of
dependency composition in Bazel. A module can depend on other modules, configure
module extensions, and instantiate repository rules.

Key properties of Bazel modules:

#. **Single resolved version**: Bazel resolves transitive module dependencies to
   a **single** version of each module across the entire build graph.
#. **Strict direct visibility**: Transitive ``bazel_dep`` dependencies propagate
   automatically, but your project cannot reference targets in a transitive
   module unless it declares its own ``bazel_dep`` on that module.
#. **Registry resolution**: Modules are resolved from `registries
   <https://bazel.build/external/registry>`__, defaulting to the `Bazel Central
   Registry (BCR) <https://registry.bazel.build/>`__.

Languages and integrations
--------------------------
Language rules and core build utilities are distributed as Bazel modules rather
than built into Bazel itself. Common examples include:

* `rules_cc <https://registry.bazel.build/modules/rules_cc>`__: C/C++ build
  rules.
* `rules_rust <https://registry.bazel.build/modules/rules_rust>`__: Rust build
  rules.
* `rules_python <https://registry.bazel.build/modules/rules_python>`__: Python
  build rules.
* `platforms <https://registry.bazel.build/modules/platforms>`__: Standard
  constraint definitions for OS and CPU target platforms.
* `bazel_skylib <https://registry.bazel.build/modules/bazel_skylib>`__: Common
  Starlark helpers and utility rules.

Example: Using rules_cc
-----------------------
Add a ``bazel_dep`` entry to ``MODULE.bazel`` to depend on ``rules_cc``:

.. code-block:: bazel

   # MODULE.bazel
   module(name = "my_project", version = "0.0.1")

   bazel_dep(name = "rules_cc", version = "0.2.4")

Targets and files in ``rules_cc`` can then be referenced as
``@rules_cc//path/to/directory:file_or_target``.

When to use a module
--------------------
* Starting a new Bazel project.
* Depending on other Bazel projects.
* Publishing reusable build rules, repository rules, or module extensions for
  other projects to consume.

.. _docs-bazel-dependencies-overview-extensions:

Module extensions
=================
`Module extensions <https://bazel.build/external/extension>`__ aggregate
configuration across the module graph to generate external repositories—commonly
to bridge external package managers such as `pip
<https://pip.pypa.io/en/stable/>`__ or `Cargo
<https://doc.rust-lang.org/cargo/>`__.

An extension works in four steps:

#. The extension defines a configuration schema made up of `tag classes
   <https://bazel.build/rules/lib/globals/bzl#tag_class>`__.
#. Modules pass configuration metadata by calling those tag methods in
   ``MODULE.bazel``.
#. The extension implementation reads the collected tags across all modules and
   instantiates one or more external repositories.
#. Modules call ``use_repo()`` to bring the generated repositories they directly
   reference into scope.

.. _docs-bazel-dependencies-overview-extensions-example:

Example: libusb
---------------
The example below configures the ``libusb`` extension from ``rules_libusb``:

.. code-block:: bazel

   # MODULE.bazel
   module(name = "my_project", version = "0.0.1")

   bazel_dep(name = "rules_libusb", version = "0.1.0-rc2")

   libusb = use_extension("@rules_libusb//:extensions.bzl", "libusb")

   # Optional if another module's default configuration is sufficient.
   libusb.source_release(min_version = "1.0.27")

   # Required only if this module directly references @libusb targets.
   use_repo(libusb, "libusb")

.. _docs-bazel-dependencies-overview-extensions-usage:

When to use a module extension
------------------------------
* Integrating an external package manager (such as ``rules_python`` for ``pip``
  or ``rules_rust`` for ``cargo``).
* Consuming or authoring external repositories that must be shared across
  multiple modules and resolved from combined cross-module metadata.

.. _docs-bazel-dependencies-overview-repository-rules:

Individual repository rules
===========================
`Repository rules <https://bazel.build/rules/lib/globals/bzl.html#repository_rule>`__
can also be invoked directly in ``MODULE.bazel`` via `use_repo_rule()
<https://bazel.build/rules/lib/globals/module.html#use_repo_rule>`__ to fetch a
module-private resource. The repository remains functional when your module is
consumed as a dependency, but no other module can reference it directly.

.. caution::

   If two modules instantiate identical repositories via ``use_repo_rule()``,
   Bazel fetches and materializes them as separate duplicate repositories.

Example: http_archive
---------------------
.. code-block:: bazel

   # MODULE.bazel
   module(name = "my_project", version = "0.0.1")

   http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

   http_archive(
       name = "arm_gcc_linux-aarch64",
       build_file = "//bazel/toolchain:gcc_arm_none_eabi.BUILD",
       sha256 = "8fd8b4a0a8d44ab2e195ccfbeef42223dfb3ede29d80f14dcf2183c34b8d199a",
       strip_prefix = "arm-gnu-toolchain-13.2.Rel1-aarch64-arm-none-eabi",
       url = "https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-aarch64-arm-none-eabi.tar.xz",
   )

When to use individual repository rules
---------------------------------------
* Fetching an isolated external resource that only the declaring module needs to
  reference.

---------------------------
Debug external repositories
---------------------------
Override a dependency with a local directory
============================================
* **Bazel modules**: Pass
  ``--override_module=dep_to_override=/path/to/local/dep_to_override``.
* **Extension or repository-rule repos**: Look up the repository's `canonical
  name <https://bazel.build/external/overview#canonical-repo-name>`__, then pass
  ``--override_repository``:

  .. code-block:: console

     $ bazel mod dump_repo_mapping "" | jq -r '.my_repo_to_override'
     +_repo_rules5+my_repo_to_override
     $ bazel build --override_repository=+_repo_rules5+my_repo_to_override=/path/to/local/repo //...

To check in persistent module overrides for a monorepo layout, configure a local
Bazel registry via ``--registry``.

Inspect materialized external repositories
==========================================
Bazel materializes external repositories under
``$(bazel info output_base)/external``:

.. code-block:: console

   $ ls $(bazel info output_base)/external
   rules_rust++rust+rust_toolchains
   rules_shell+
   rules_uv+
   stardoc+
   toolchains_protoc+

.. caution::

   Do not edit files inside ``output_base/external``; Bazel does not track
   manual edits there and may produce inconsistent build results.

Additional debugging flags
==========================
* ``--registry``: Point module resolution to an alternate or ``file://`` Bazel
  registry.
* ``--sandbox_debug``: Retain the sandbox directory of failing build actions to
  inspect missing or misconfigured inputs.

---------------------
Avoid common pitfalls
---------------------
Avoid duplicate repository definitions
======================================
Repository names introduced by ``bazel_dep``, ``use_repo``, and
``use_repo_rule`` can coexist in parallel if given different apparent names,
allowing the same dependency to enter a build twice under separate repositories.

Do not use an ``external/`` directory
=====================================
``external/`` is reserved by Bazel (`bazelbuild/bazel#4508
<https://github.com/bazelbuild/bazel/issues/4508>`__); do not place project
files under a top-level ``external/`` directory.

Ignore nested Bazel workspaces
==============================
Nested ``MODULE.bazel`` projects inside a repository cause build errors unless
excluded. Add fixed paths to ``.bazelignore``, or use `ignore_directories()
<https://bazel.build/rules/lib/globals/repo#ignore_directories>`__ in
``REPO.bazel`` when wildcard patterns are needed.

---------
Resources
---------
* `MODULE.bazel reference <https://bazel.build/rules/lib/globals/module>`__
* `External dependencies overview <https://bazel.build/external/overview>`__
* `Bazel modules <https://bazel.build/external/module>`__
* `Bazel registries <https://bazel.build/external/registry>`__
* `Module extensions <https://bazel.build/external/extension>`__
* `Vendor mode <https://bazel.build/external/vendor>`__
