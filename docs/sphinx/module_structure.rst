.. _docs-module-structure:

================
Module structure
================
The Pigweed :ref:`module <docs-glossary-module>` structure is designed to keep
as much code as possible for a particular slice of functionality in one place.
That means including the code from multiple languages, as well as all the
related documentation and tests.

Additionally, the structure is designed to limit the number of places a file
could go, so that when reading call sites it is obvious where a header is from.
That is where the duplicated ``<module>`` occurrences in file paths come from.

.. important::

   Many Pigweed modules do not currently conform to the module structure
   guidelines, particularly for C++ and documentation. These modules will be
   migrated over time.

   Use the module structure described herein for all new modules.

------------------------
Example module structure
------------------------
.. code-block:: text

   pw_foo/...

     # Docs go into docs/
     docs/index.rst        # Docs landing page (required)
     docs/concepts.rst     # Conceptual docs (optional)
     docs/design.rst       # Design docs (optional)
     docs/guides.rst       # How-to guides (optional)
     docs/api.rst          # API reference (optional)
     docs/cli.rst          # CLI reference (optional)
     docs/gui.rst          # GUI reference (optional)
     docs/tutorials/*.rst  # Tutorials (optional)

     BUILD.bazel
     BUILD.gn
     CMakeLists.txt
     module.toml           # Module metadata (upstream Pigweed)

     # C++ and C source code goes into cpp/
     cpp/...
     cpp/BUILD.bazel
     cpp/BUILD.gn
     cpp/CMakeLists.txt

     # Public headers go under cpp/public/<module>/
     cpp/public/pw_foo/foo.h
     cpp/public/pw_foo/baz.h

     # Exposed private headers go under cpp/public/<module>/internal/
     cpp/public/pw_foo/internal/bar.h
     cpp/public/pw_foo/internal/qux.h

     # Public override headers must go in cpp/public_overrides/<build_target_name>/
     cpp/public_overrides/gtest/gtest/gtest.h
     cpp/public_overrides/libc_string/string.h

     # Private headers go into cpp/private/<module>_*/
     cpp/private/pw_foo_internal/zap.h
     cpp/private/pw_foo_private/zip.h

     # Implementations go in cpp/
     cpp/foo_impl.cc
     cpp/foo.cc
     cpp/baz.cc
     cpp/bar.cc
     cpp/zap.cc
     cpp/zip.cc
     cpp/alxx.cc

     # Tests also go in cpp/
     cpp/foo_test.cc
     cpp/bar_test.cc
     cpp/zip_test.cc

     # Python files go into py/<module>/
     py/BUILD.bazel     # Python packages are declared in Bazel using py_library
     py/BUILD.gn        # Python packages are declared in GN using pw_python_package
     py/pyproject.toml  # Python files are structured as standard Python packages
     py/setup.cfg
     py/foo_test.py     # Tests go in py/ but outside of the Python package
     py/bar_test.py
     py/pw_foo/__init__.py
     py/pw_foo/__main__.py
     py/pw_foo/bar.py
     py/pw_foo/py.typed  # Indicates that this package has type annotations

     # Rust crates go into rust/
     rust/BUILD.bazel
     rust/crate_one.rs          # Single file crates are in rust/<crate_name>.rs
     rust/crate_two/lib.rs      # Multi-file crate's top level source in:
                                #   rust/<crate>/lib.rs
     rust/crate_two/mod_one.rs  # Multi-file crate's modules in:
     rust/crate_two/mod_two.rs  #   rust/<crate>/<module_name>.rs
                                # Prefer not using mod.rs files.

     # Go files go into go/
     go/...

     # Examples go in examples/, either mixing different languages or nested
     # under the language directory
     examples/BUILD.bazel  # Only the Bazel build is required
     examples/demo.py
     examples/demo.go
     cpp/examples/demo.cc

     # Size reports go under <language>/size_report/
     cpp/size_report/BUILD.bazel  # Only the Bazel build is required
     cpp/size_report/base.cc
     cpp/size_report/use_case_a.cc
     cpp/size_report/use_case_b.cc

     # Protobuf definition files go into <module>_protos/...
     pw_foo_protos/foo.proto
     pw_foo_protos/internal/zap.proto

     # Other directories are fine, but should be private.
     data/...
     graphics/...
     collection_of_tests/...
     code_relating_to_subfeature/...

-----------
Module name
-----------
Pigweed upstream modules are always named with a prefix ``pw_`` to enforce
namespacing. Projects using Pigweed that wish to make their own modules can use
whatever name they like, but we suggest picking a short prefix to namespace
your product (e.g. for an Internet of Toast project, perhaps the prefix could
be ``it_``).

--------------------
C++ module structure
--------------------
All C and C++ source files, headers, tests, and C++-specific build files live
under ``{pw_module_dir}/cpp/``.

C++ public headers
==================
Located in ``{pw_module_dir}/cpp/public/<module>/``. These headers are the
public interface for the module.

**Public headers** should take the form:

``{pw_module_dir}/cpp/public/<module>/*.h``

**Exposed private headers** should take the form:

``{pw_module_dir}/cpp/public/<module>/internal/*.h``

Examples:

.. code-block:: text

   pw_foo/...
     cpp/public/pw_foo/foo.h
     cpp/public/pw_foo/a_header.h
     cpp/public/pw_foo/baz.h

For headers that must be exposed due to C++ limitations (i.e. are included from
the public interface, but are not intended for public use), place the headers in
an ``internal`` subfolder under the public headers directory:
``{pw_module_dir}/cpp/public/<module>/internal/*.h``. For example:

.. code-block:: text

   pw_foo/...
     cpp/public/pw_foo/internal/secret.h
     cpp/public/pw_foo/internal/business.h

.. note::

   These headers must not override headers from other modules. For that, there
   is the ``cpp/public_overrides/`` directory.

C++ public override headers
===========================
Located in
``{pw_module_dir}/cpp/public_overrides/<build_target_name>/<path>/*.h``. In
general, the Pigweed philosophy is to avoid having "things hiding under rocks",
and having header files with the same name that can override each other is
considered a rock where surprising things can hide. Additionally, a design goal
of the Pigweed module structure is to make it so there is ideally exactly one
obvious place to find a header based on an ``#include``.

However, in some cases header overrides are necessary to enable flexibly
combining modules. To make this as explicit as possible, headers that override
other headers must go in:

``{pw_module_dir}/cpp/public_overrides/<build_target_name>/<path>/*.h``

For example, ``pw_sync_freertos`` (:cs:`pw_sync_freertos/public_overrides/`, not
yet migrated under ``cpp/``) provides header overrides for ``pw_sync`` facades.
Following the ``cpp/`` directory layout, the structure of the module is
(omitting some files):

.. code-block:: text

   pw_sync_freertos/...

     cpp/public_overrides/binary_semaphore/pw_sync_backend/binary_semaphore_inline.h
     cpp/public_overrides/binary_semaphore/pw_sync_backend/binary_semaphore_native.h
     cpp/public_overrides/mutex/pw_sync_backend/mutex_inline.h
     cpp/public_overrides/mutex/pw_sync_backend/mutex_native.h

     cpp/public/pw_sync_freertos/binary_semaphore_inline.h
     cpp/public/pw_sync_freertos/binary_semaphore_native.h
     cpp/public/pw_sync_freertos/mutex_inline.h
     cpp/public/pw_sync_freertos/mutex_native.h

Note that the overrides are in a separate ``cpp/public_overrides`` directory.

C++ private headers
===================
Located in ``{pw_module_dir}/cpp/private/<module>_*/``. Private headers that are
only used within the module's implementation and tests (and are not transitively
included by public headers) go under ``cpp/private/``, namespaced with a
``<module>_*`` directory (such as ``<module>_private`` or ``<module>_internal``)
so ``#include`` paths remain unambiguous:

.. code-block:: text

   pw_foo/...
     cpp/private/pw_foo_internal/zap.h
     cpp/private/pw_foo_private/zip.h

C++ implementation and test files
=================================
Located in ``{pw_module_dir}/cpp/``. C++ implementation files (``*.cc``) and
unit tests (``*_test.cc``) go in the ``cpp/`` directory alongside C++ build
files. Implementation files must always use ``""``-style includes for module and
project headers.

Example:

.. code-block:: text

   pw_cobs/...
     BUILD.bazel
     BUILD.gn
     cpp/BUILD.bazel
     cpp/BUILD.gn
     cpp/cobs.cc
     cpp/cobs_test.cc
     cpp/public/pw_cobs/cobs.h

.. _module-structure-compile-time-configuration:

Compile-time configuration
==========================
Pigweed modules are intended to be used in a wide variety of environments.
In support of this, some modules expose compile-time configuration options.
Pigweed has an established pattern for declaring and overriding module
configuration.

.. tip::

   Compile-time configuration provides flexibility, but also imposes
   restrictions. A module can only have one configuration in a given build.
   This makes testing modules with compile-time configuration more difficult.
   Where appropriate, consider alternatives such as C++ templates or runtime
   configuration.

Declaring configuration
-----------------------
Configuration options are declared in a header file as macros. If the macro is
not already defined, a default definition is provided. Otherwise, nothing is
done. Configuration headers may include ``static_assert`` statements to validate
configuration values.

All configuration headers **must** include a check for a user-provided override
header. This allows a project to override configuration options with a header
file, rather than through individual defines. Bazel does not support propagating
compiler options like ``-include``, so this is the only way to provide a config
override header.

The override header check should be placed before any default definitions.

.. code-block:: c++

   // Example configuration header

   /// User-provided header to optionally override options in this file.
   #if defined(PW_FOO_CONFIG_HEADER)
   #include PW_FOO_CONFIG_HEADER
   #endif  // defined(PW_FOO_CONFIG_HEADER)

   #ifndef PW_FOO_INPUT_BUFFER_SIZE_BYTES
   #define PW_FOO_INPUT_BUFFER_SIZE_BYTES 128
   #endif  // PW_FOO_INPUT_BUFFER_SIZE_BYTES

   static_assert(PW_FOO_INPUT_BUFFER_SIZE_BYTES >= 64);

The configuration header may go in one of three places in the module, depending
on whether the header should be exposed by the module or not.

.. code-block:: text

   pw_foo/...

     # Publicly accessible configuration header
     cpp/public/pw_foo/config.h

     # Internal configuration header that is included by other module headers
     cpp/public/pw_foo/internal/config.h

     # Private configuration header used only by .cc files
     cpp/private/pw_foo_private/config.h

The configuration header is provided by a build system library. This library
acts as a :ref:`facade <docs-module-structure-facades>`. The details depend on
the build system.

GN compile-time configuration
-----------------------------
The facade uses a variable such as ``pw_foo_CONFIG``. In upstream Pigweed, all
config facades default to the ``pw_build_DEFAULT_MODULE_CONFIG`` backend. In
``cpp/BUILD.gn``, the config facade is declared as follows:

.. code-block::

   declare_args() {
     # The build target that overrides the default configuration options for this
     # module. This should point to a source set that provides defines through a
     # public config (which may -include a file or add defines directly).
     pw_foo_CONFIG = pw_build_DEFAULT_MODULE_CONFIG
   }

   # An example source set for each potential config header location follows.

   # Publicly accessible configuration header (most common)
   pw_source_set("config") {
     public = [ "public/pw_foo/config.h" ]
     public_configs = [ ":public_include_path" ]
     public_deps = [ pw_foo_CONFIG ]
   }

   # Internal configuration header that is included by other module headers
   pw_source_set("config") {
     sources = [ "public/pw_foo/internal/config.h" ]
     public_configs = [ ":public_include_path" ]
     public_deps = [ pw_foo_CONFIG ]
     visibility = [ ":*" ]  # Only allow this module to depend on ":config"
     friend = [ ":*" ]  # Allow this module to access the config.h header.
   }

   # Private configuration header
   pw_source_set("config") {
     public = [ "private/pw_foo_private/config.h" ]
     public_configs = [ ":private_include_path" ]
     public_deps = [ pw_foo_CONFIG ]
     visibility = [ ":*" ]  # Only allow this module to depend on ":config"
   }

Bazel compile-time configuration
--------------------------------
In ``cpp/BUILD.bazel``, the module that uses configuration depends on a
``label_flag``, conventionally named ``config_override``, that by default points
to ``//pw_build:default_module_config`` (often re-exported or aliased at the
module root as ``//pw_foo:config_override``). For example:

.. code-block:: bazel

   # A module with a public config.
   cc_library(
       name = "config",
       hdrs = ["public/pw_foo/config.h"],
       strip_include_prefix = "public",
       deps = [":config_override"],
   )

   cc_library(
       name = "pw_foo",
       # ...
       deps = [":config"],
   )

   label_flag(
       name = "config_override",
       build_setting_default = "//pw_build:default_module_config",
   )

   # A module with an internal config that's included by other module headers.
   cc_library(
       name = "pw_bar",
       # ...
       deps = [":internal_config"],
   )

   cc_library(
       name = "internal_config",
       hdrs = ["public/pw_bar/internal/config.h"],
       strip_include_prefix = "public",
       visibility = ["//visibility:private"],
       deps = [":config_override"],
   )

   label_flag(
       name = "config_override",
       build_setting_default = "//pw_build:default_module_config",
   )

   # A module with a private config.
   cc_library(
       name = "pw_baz",
       # ...
       implementation_deps = [":private_config"],
   )

   cc_library(
       name = "private_config",
       hdrs = ["private/pw_baz_private/config.h"],
       strip_include_prefix = "private",
       visibility = ["//visibility:private"],
       deps = [":config_override"],
   )

   label_flag(
       name = "config_override",
       build_setting_default = "//pw_build:default_module_config",
   )

Overriding configuration
------------------------
As noted above, all module configuration facades default to the same backend
(``pw_build_DEFAULT_MODULE_CONFIG`` in GN, ``//pw_build:default_module_config``
in Bazel). This allows projects to override configuration values for multiple
modules from a single configuration backend, if desired. The configuration
values may also be overridden individually by setting backends for the
individual module configurations (e.g. in GN, ``pw_foo_CONFIG =
"//configuration:my_foo_config"``, in Bazel
``--//pw_foo/cpp:config_override=//configuration:my_foo_config`` or
``--//pw_foo:config_override=//configuration:my_foo_config``).

Configuration options are overridden by setting macros in the config backend.
Override a module's config by setting its configuration header macro (e.g.
``PW_FOO_CONFIG_HEADER``) to point to a header that provides macro overrides.
Configuration options may also be set directly with macro definitions in the
build system (e.g. ``-DPW_FOO_INPUT_BUFFER_SIZE_BYTES=256``), though it's better
to use a header when the module supports it. It is not recommended to use
compilation options like ``-include`` since these are not supported in all build
systems (including Bazel).

GN config override example
^^^^^^^^^^^^^^^^^^^^^^^^^^
This example shows two ways to configure a module in the GN build system.

.. code-block::

   # In the toolchain, set either pw_build_DEFAULT_MODULE_CONFIG or pw_foo_CONFIG
   pw_build_DEFAULT_MODULE_CONFIG = get_path_info(":define_overrides", "abspath")

   # This configuration sets PW_FOO_INPUT_BUFFER_SIZE_BYTES using the -D flag.
   pw_source_set("define_overrides") {
     public_configs = [ ":define_options" ]
   }

   config("define_options") {
     defines = [ "PW_FOO_INPUT_BUFFER_SIZE_BYTES=256" ]
   }

   # This configuration sets PW_FOO_INPUT_BUFFER_SIZE_BYTES in a header file via
   # PW_FOO_CONFIG_HEADER.
   pw_source_set("header_overrides") {
     public_configs = [ ":set_config_header" ]
     public = [ "public/my_config/foo_config.h" ]
   }

   config("set_config_header") {
     include_dirs = [ "public" ]
     defines = [ "PW_FOO_CONFIG_HEADER=\"my_config/foo_config.h\"" ]
   }

Bazel config override example
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
This shows how to configure a module in Bazel using either a config header or
defines.

.. code-block:: bazel

   # To use these overrides for all modules, set the global module config label
   # flag:
   #
   # --@pigweed//pw_build:default_module_config=//path_to:config_overrides
   #
   # To use them just for one module, set the module-specific config label
   # flag:
   #
   # --@pigweed//pw_foo/cpp:config_override=//path_to:config_overrides
   cc_library(
       name = "config_overrides",
       hdrs = ["public/my_config/foo_config.h"],
       defines = [
           "PW_FOO_CONFIG_HEADER=\"my_config/foo_config.h\"",
       ],
       strip_include_prefix = "public",
   )

To conditionally enable targets based on whether a particular config override is
enabled, a ``config_setting`` can be defined that looks at the
``config_override`` label flag value. This allows use of
``target_compatible_with`` to enable targets based on the configuration.

.. code-block:: bazel

   # Set up config_setting that is enabled when a particular config override is
   # set.
   config_setting(
       name = "config_override_setting",
       flag_values = {
           "@pigweed//pw_foo/cpp:config_override": ":config_overrides",
       },
   )

   # For targets that need some specific config setting to build, conditionally
   # enable them.
   pw_cc_test(
       name = "test",
       target_compatible_with = select({
           ":config_override_setting": [],
           "//conditions:default": ["@platforms//:incompatible"],
       }),
       # ...
   )

Why this config pattern is preferred
------------------------------------
Alternate patterns for configuring a module include overriding the module's
config header or having that header optionally include a header at a known
path (e.g. ``pw_foo/config_overrides.h``). There are a few downsides to these
approaches:

* The module needs its own config header that defines, provides defaults for,
  and validates the configuration options. Replacing this header with a
  user-defined header would require defining all options in the user's header,
  which is cumbersome and brittle, and would bypass validation in the module's
  header.
* Including a config override header at a particular path would prevent
  multiple modules from sharing the same configuration file. Multiple headers
  could redirect to the same configuration file, but this would still require
  creating a separate header and setting the config backend variable for each
  module.
* An optional config override header file via ``__has_include`` would silently
  be excluded if the file path or include directory were accidentally
  misconfigured.

-----------------------
Python module structure
-----------------------
Python code is structured as described in the :ref:`docs-python-build-structure`
section of :ref:`docs-python-build`.

.. _docs-module-structure-facades:

-------
Facades
-------
In Pigweed, facades represent a dependency that can be swapped at compile time.
Facades are similar in concept to a virtual interface, but the implementation is
set by the build system. Runtime polymorphism with facades is not
possible, and each facade may only have one implementation (backend) per
toolchain compilation. See :ref:`docs-facades` for a conceptual overview.

In the simplest sense, a facade is just a dependency represented by a build
setting or variable (for example, the ``//pw_log:backend`` ``label_flag`` in
Bazel or the ``pw_log_BACKEND`` build variable in GN and CMake). Facades
typically are bundled with a build system library that depends on the backend.

Facades are essential in some circumstances:

* Low-level, platform-specific features (:ref:`module-pw_cpu_exception`).
* Features that require a macro or non-virtual function interface
  (:ref:`module-pw_log`, :ref:`module-pw_assert`).
* Highly leveraged code where a virtual interface or callback is too costly or
  cumbersome (:ref:`module-pw_tokenizer`).

.. caution::

   Modules should only use facades when necessary. Facades are permanently
   locked to a particular implementation at compile time. Multiple backends
   cannot be used in one build, and runtime dependency injection is not
   possible, which makes testing difficult. Where appropriate, modules should
   use other mechanisms, such as virtual interfaces, callbacks, or templates, in
   place of facades.

Both Bazel (:ref:`pw_facade macro <module-pw_build-bazel-pw_facade>`) and GN
(:ref:`pw_facade template <module-pw_build-facade>`) provide helpers to declare
facades.

Multiple facades
================
A module may contain multiple facades. Each facade's public override headers
must be contained in separate ``cpp/public_overrides/<build_target_name>/``
folders in the backend implementation, so that it's possible to use multiple
backends for a module.

.. code-block:: text

   # pw_foo contains 2 facades, foo and bar
   pw_foo/...
     # Public headers
     # cpp/public/pw_foo/foo.h #includes pw_foo_backend/foo.h
     # cpp/public/pw_foo/bar.h #includes pw_foo_backend/bar.h
     cpp/public/pw_foo/foo.h
     cpp/public/pw_foo/bar.h

   pw_foo_backend/...
     # Public override headers for foo and bar go in separate folders
     cpp/public_overrides/foo/pw_foo_backend/foo.h
     cpp/public_overrides/bar/pw_foo_backend/bar.h

-------------
Documentation
-------------
Module documentation lives under ``{pw_module_dir}/docs/``, with
``docs/index.rst`` as the landing page. In upstream Pigweed, module metadata is
configured in ``{pw_module_dir}/module.toml``. See
:ref:`docs-contrib-docs-modules` for detailed guidelines on authoring module
documentation.

-----------------------------
Creating a new Pigweed module
-----------------------------
.. tip::

   Connect with the Pigweed community (by `mailing the Pigweed list
   <https://groups.google.com/forum/#!forum/pigweed>`_ or `raising your idea
   in the Pigweed chat <https://discord.gg/M9NSeTA>`_) to discuss your module
   idea before getting too far into the implementation. This can prevent
   accidentally duplicating work or writing code that won't get accepted.

New Pigweed modules can be scaffolded with :ref:`module-pw_module-module-create`
(``./pw module create MODULE_NAME``; refer to the `Module name`_ guidelines).

.. note::

   ``./pw module create`` and ``./pw module check`` do not yet generate or
   validate the nested ``cpp/`` and ``docs/`` directories; move generated C++
   and documentation files into ``cpp/`` and ``docs/`` to match this guide.

When creating a new module, add files in the following locations:

1. Add `C++ public headers`_ in ``{pw_module_dir}/cpp/public/<module>/``.
2. Add `C++ implementation and test files`_ in ``{pw_module_dir}/cpp/``.
3. Add module documentation to ``{pw_module_dir}/docs/index.rst`` and (for
   upstream Pigweed) module metadata to ``{pw_module_dir}/module.toml``. See
   :ref:`docs-contrib-docs-modules` for documentation guidelines.
4. Add Bazel build targets to ``{pw_module_dir}/BUILD.bazel`` (including
   ``sphinx_docs_library(name = "docs", ...)``) and
   ``{pw_module_dir}/cpp/BUILD.bazel``.
5. Add GN build targets to ``{pw_module_dir}/BUILD.gn`` (forwarding
   ``pw_test_group("tests")`` to ``"cpp:tests"``) and
   ``{pw_module_dir}/cpp/BUILD.gn``.
6. Add CMake build targets to ``{pw_module_dir}/CMakeLists.txt``
   (``add_subdirectory(cpp)``) and ``{pw_module_dir}/cpp/CMakeLists.txt``.
7. Contribute your module to upstream Pigweed (optional, but encouraged!).
