.. _docs-module-structure:

----------------
Module Structure
----------------
The Pigweed :ref:`module <docs-glossary-module>` structure is designed to keep
as much code as possible for a particular slice of functionality in one place.
That means including the code from multiple languages, as well as all the
related documentation and tests.

Additionally, the structure is designed to limit the number of places a file
could go, so that when reading call sites it is obvious where a header is from.
That is where the duplicated ``<module>`` occurrences in file paths comes from.

.. important::

   Many Pigweed modules do not currently conform to the module structure
   guidelines, particularly for C++. These modules will be migrated over time.

   Use the module structure described herein for all new modules.

Example module structure
------------------------
.. code-block:: text

   pw_foo/...

     # Docs go into docs/
     docs/docs.rst         # Docs landing page (required)
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

     # C++ and C source code goes into cpp/
     cpp/...

     # Public headers go under cpp/public/<module>/
     cpp/public/pw_foo/foo.h
     cpp/public/pw_foo/baz.h

     # Exposed private headers go under cpp/public/<module>/internal/
     cpp/public/pw_foo/internal/bar.h
     cpp/public/pw_foo/internal/qux.h

     # Public override headers must go in cpp/public_overrides/
     cpp/public_overrides/gtest/gtest.h
     cpp/public_overrides/string.h

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
     py/BUILD.bazel  # Python packages are declared in Bazel using py_library
     py/BUILD.gn     # Python packages are declared in GN using pw_python_package
     py/setup.py     # Python files are structured as standard Python packages
     py/foo_test.py  # Tests go in py/ but outside of the Python package
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

Module name
-----------
Pigweed upstream modules are always named with a prefix ``pw_`` to enforce
namespacing. Projects using Pigweed that wish to make their own modules can use
whatever name they like, but we suggest picking a short prefix to namespace
your product (e.g. for an Internet of Toast project, perhaps the prefix could
be ``it_``).

C++ module structure
--------------------

C++ public headers
~~~~~~~~~~~~~~~~~~
Located ``{pw_module_dir}/public/<module>``. These headers are the public
interface for the module.

**Public headers** should take the form:

``{pw_module_dir}/public/<module>/*.h``

**Exposed private headers** should take the form:

``{pw_module_dir}/public/<module>/internal/*.h``

Examples:

.. code-block::

   pw_foo/...
     public/pw_foo/foo.h
     public/pw_foo/a_header.h
     public/pw_foo/baz.h

For headers that must be exposed due to C++ limitations (i.e. are included from
the public interface, but are not intended for use), place the headers in a
``internal`` subfolder under the public headers directory; as
``{pw_module_dir}/public/<module>/internal/*.h``. For example:

.. code-block::

   pw_foo/...
     public/pw_foo/internal/secret.h
     public/pw_foo/internal/business.h

.. note::

  These headers must not override headers from other modules. For
  that, there is the ``public_overrides/`` directory.

C++ public override headers
~~~~~~~~~~~~~~~~~~~~~~~~~~~
Located ``{pw_module_dir}/public_overrides/<module>``. In general, the Pigweed
philosophy is to avoid having "things hiding under rocks", and having header
files with the same name that can override each other is considered a rock
where surprising things can hide. Additionally, a design goal of the Pigweed
module structure is to make it so there is ideally exactly one obvious place
to find a header based on an ``#include``.

However, in some cases header overrides are necessary to enable flexibly
combining modules. To make this as explicit as possible, headers that override
other headers must go in

``{pw_module_dir}/public_overrides/...```

For example, the ``pw_unit_test`` module provides a header override for
``gtest/gtest.h``. The structure of the module is (omitting some files):

.. code-block::

   pw_unit_test/...

     light_public_overrides/pw_unit_test/framework_backend.h
     googletest_public_overrides/pw_unit_test/framework_backend.h

     public_overrides/gtest
     public_overrides/gtest/gtest.h

     public/pw_unit_test
     public/pw_unit_test/simple_printing_event_handler.h
     public/pw_unit_test/event_handler.h

Note that the overrides are in a separate directory ``public_overrides``.

C++ implementation files
~~~~~~~~~~~~~~~~~~~~~~~~
Located ``{pw_module_dir}/``. C++ implementation files go at the top level of
the module. Implementation files must always use "" style includes.

Example:

.. code-block::

   pw_unit_test/...
     main.cc
     framework.cc
     test.gni
     BUILD.gn
     README.md

.. _module-structure-compile-time-configuration:

Compile-time configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~
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
^^^^^^^^^^^^^^^^^^^^^^^
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

.. code-block::

   pw_foo/...

     # Publicly accessible configuration header
     public/pw_foo/config.h

     # Internal configuration header that is included by other module headers
     public/pw_foo/internal/config.h

     # Internal configuration header
     pw_foo_private/config.h

The configuration header is provided by a build system library. This library
acts as a :ref:`facade<docs-module-structure-facades>`. The details depend on
the build system.

GN compile-time configuration
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
The facade uses a variable such as ``pw_foo_CONFIG``. In upstream Pigweed, all
config facades default to the ``pw_build_DEFAULT_MODULE_CONFIG`` backend. The
config facade is declared as follows:

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
     visibility = [":*"]  # Only allow this module to depend on ":config"
     friend = [":*"]  # Allow this module to access the config.h header.
   }

   # Internal configuration header
   pw_source_set("config") {
     public = [ "pw_foo_private/config.h" ]
     public_deps = [ pw_foo_CONFIG ]
     visibility = [":*"]  # Only allow this module to depend on ":config"
   }

Bazel compile-time configuration
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
The module that uses configuration depends on a ``label_flag``, conventionally
named ``config``, that by default points to the
``//pw_build:default_module_config``. For example,

.. code-block:: python

   # A module with a public config. That config doesn't need to be broken out
   # into a separate cc_library.
   cc_library(
     name = "pw_foo",
     hdrs = ["config.h"],
     deps = [":config"],
   )

   label_flag(
     name = "config",
     build_setting_default = "//pw_build:default_module_config",
   )

   # A module with an internal config that's included by other module headers.
   cc_library(
     name = "pw_bar",
     deps = [":internal_config"],
   )

   cc_library(
     name = "internal_config",
     hdrs = ["config.h"],
     deps = [":config"],
     visibility = ["//visibility:private"],
   )

   label_flag(
     name = "config",
     build_setting_default = "//pw_build:default_module_config",
   )

   # A module with a private config.
   cc_library(
     name = "pw_bar",
     implementation_deps = [":private_config"],
   )

   cc_library(
     name = "private_config",
     hdrs = ["config.h"],
     deps = [":config"],
     visibility = ["//visibility:private"],
   )

   label_flag(
     name = "config",
     build_setting_default = "//pw_build:default_module_config",
   )

Overriding configuration
^^^^^^^^^^^^^^^^^^^^^^^^
As noted above, all module configuration facades default to the same backend
(``pw_build_DEFAULT_MODULE_CONFIG`` in GN, ``//pw_build:default_module_config``
in Bazel). This allows projects to override configuration values for multiple
modules from a single configuration backend, if desired. The configuration
values may also be overridden individually by setting backends for the
individual module configurations (e.g. in GN, ``pw_foo_CONFIG =
"//configuration:my_foo_config"``, in Bazel
``--//pw_foo:config=//configuration:my_foo_config``).

Configuration options are overridden by setting macros in the config backend.
Override a module's config by setting its configuration header macro (e.g.
``PW_FOO_CONFIG_HEADER``) to point to a header that provides macro overrides.
Configuration options may also be set directly  with macro definitions in the
build system (e.g. ``-DPW_FOO_INPUT_BUFFER_SIZE_BYTES=256``), though its better
to use a header when the module supports it. It is not recommended to to use
compilation options like ``-include`` since these are not supported in all build
systems (including Bazel).

GN config override example
..........................
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

   # This configuration sets PW_FOO_INPUT_BUFFER_SIZE_BYTES in a header file.
   pw_source_set("include_overrides") {
     public_configs = [ ":set_options_in_header_file" ]

     # Header file with #define PW_FOO_INPUT_BUFFER_SIZE_BYTES 256
     sources = [ "my_config_overrides.h" ]
   }

   config("set_options_in_header_file") {
     cflags = [
       "-include",
       rebase_path("my_config_overrides.h", root_build_dir),
     ]
   }

Bazel config override example
.............................
This shows the only supported way to configure a module in Bazel.

.. code-block:: python

   # To use these overrides for all modules, set the global module config label
   # flag,
   #
   # --@pigweed//pw_build:default_module_config=//path_to:config_overrides
   #
   # To use them just for one module, set the module-specific config label
   # flag,
   #
   # --@pigweed//pw_foo:config_override=//path_to:config_overrides
   cc_library(
     name = "config_overrides",
     defines = [
        "PW_FOO_INPUT_BUFFER_SIZE_BYTES=256",
     ],
   )

To conditionally enable targets based on whether a particular config override is
enabled, a ``config_setting`` can be defined that looks at the config_override
label flag value. This allows use of ``target_compatible_with`` to enable
targets based on the configuration.

.. code-block:: python

   # Setup config_setting that is enabled when a particular config override is
   # set.
   config_setting(
     name = "config_override_setting",
     flag_values = {
       "--@pigweed//pw_foo:config_override": ":config_overrides",
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
     ...
   )


Why this config pattern is preferred
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Alternate patterns for configuring a module include overriding the module's
config header or having that header optionally include a header at a known
path (e.g. ``pw_foo/config_overrides.h``). There are a few downsides to these
approaches:

*  The module needs its own config header that defines, provides defaults for,
   and validates the configuration options. Replacing this header with a
   user-defined header would require defining all options in the user's header,
   which is cumbersome and brittle, and would bypass validation in the module's
   header.
*  Including a config override header at a particular path would prevent
   multiple modules from sharing the same configuration file. Multiple headers
   could redirect to the same configuration file, but this would still require
   creating a separate header and setting the config backend variable for each
   module.
*  Optionally including a config override header requires boilerplate code that
   would have to be duplicated in every configurable module.
*  An optional config override header file would silently be excluded if the
   file path were accidentally misspelled.

Python module structure
-----------------------
Python code is structured as described in the :ref:`docs-python-build-structure`
section of :ref:`docs-python-build`.

.. _docs-module-structure-facades:

Facades
-------
In Pigweed, facades represent a dependency that can be swapped at compile time.
Facades are similar in concept to a virtual interface, but the implementation is
set by the build system. Runtime polymorphism with facades is not
possible, and each facade may only have one implementation (backend) per
toolchain compilation.

In the simplest sense, a facade is just a dependency represented by a variable.
For example, the ``pw_log`` facade is represented by the ``pw_log_BACKEND``
build variable. Facades typically are bundled with a build system library that
depends on the backend.

Facades are essential in some circumstances:

* Low-level, platform-specific features (:ref:`module-pw_cpu_exception`).
* Features that require a macro or non-virtual function interface
  (:ref:`module-pw_log`, :ref:`module-pw_assert`).
* Highly leveraged code where a virtual interface or callback is too costly or
  cumbersome (:ref:`module-pw_tokenizer`).

.. caution::

  Modules should only use facades when necessary. Facades are permanently locked
  to a particular implementation at compile time. Multiple backends cannot be
  used in one build, and runtime dependency injection is not possible, which
  makes testing difficult. Where appropriate, modules should use other
  mechanisms, such as virtual interfaces, callbacks, or templates, in place of
  facades.

The GN build system provides the
:ref:`pw_facade template<module-pw_build-facade>` as a convenient way to declare
facades.

Multiple Facades
~~~~~~~~~~~~~~~~
A module may contain multiple facades. Each facade's public override headers
must be contained in separate folders in the backend implementation, so that
it's possible to use multiple backends for a module.

.. code-block::

   # pw_foo contains 2 facades, foo and bar
   pw_foo/...
     # Public headers
     # public/pw_foo/foo.h #includes pw_foo_backend/foo.h
     # public/pw_foo/bar.h #includes pw_foo_backend/bar.h
     public/pw_foo/foo.h
     public/pw_foo/bar.h

   pw_foo_backend/...

     # Public override headers for facade1 and facade2 go in separate folders
     foo_public_overrides/pw_foo_backend/foo.h
     bar_public_overrides/pw_foo_backend/bar.h

Documentation
-------------
See :ref:`seed-0102`.

Creating a new Pigweed module
-----------------------------
New Pigweed modules can be easily created using
``./pw module create MODULE_NAME`` (refer to the `Module name`_ guidelines).

.. tip::

  Connect with the Pigweed community (by `mailing the Pigweed list
  <https://groups.google.com/forum/#!forum/pigweed>`_ or `raising your idea
  in the Pigweed chat <https://discord.gg/M9NSeTA>`_) to discuss your module
  idea before getting too far into the implementation. This can prevent
  accidentally duplicating work, or avoiding writing code that won't get
  accepted.

This command will create a folder with the provided module name as well as a
number of basic build files. After this command has run, new code should be
added in the following locations:

1. Add `C++ public headers`_ files in
   ``{pw_module_dir}/public/{pw_module_name}/``
2. Add `C++ implementation files`_ files in ``{pw_module_dir}/``
3. Add module documentation to ``{pw_module_dir}/docs.rst``. See
   :ref:`seed-0102` for additional documentation options.
4. Add GN build targets to ``{pw_module_dir}/BUILD.gn``.

   - Add new ``pw_test`` targets to the list in ``pw_test_group("tests")``.
   - Add any additional ``.rst`` documentation files to
     ``pw_docs_group("docs")``.

5. Add Bazel build targets to ``{pw_module_dir}/BUILD.bazel``.

6. Add CMake build targets to ``{pw_module_dir}/CMakeLists.txt``.

7. Run :ref:`module-pw_module-module-check` to ensure that the new module has
   been properly configured.

    - ``$ ./pw module check {pw_module_dir}``

8. Contribute your module to upstream Pigweed (optional but encouraged!)
