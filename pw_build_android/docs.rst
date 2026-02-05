.. _module-pw_build_android:

================
pw_build_android
================
.. pigweed-module::
   :name: pw_build_android

``pw_build_android`` provides simple utilities and guidelines for building with
Soong.

----------
Quickstart
----------
Use the ``pw_android_common_backends`` and ``pw_android_common_target_support``
```cc_defaults`` in the Pigweed module library or binary rule to use a
preselected set of backends common to most Android platform projects.

.. code-block:: androidbp

   cc_binary {
       name: "my_app",
       defaults: [
           "pw_android_common_target_support",
           "pw_android_common_backends",
       ],
       srcs: [
           "main.cc",
       ],
   }


----------------------------
Basic blueprint files format
----------------------------
All Pigweed Soong blueprint files must be named ``Android.bp``, and include the
folowing copyright header with the year adjusted and package.

.. code-block:: androidbp

   // Copyright 2024 The Pigweed Authors
   //
   // Licensed under the Apache License, Version 2.0 (the "License"); you may not
   // use this file except in compliance with the License. You may obtain a copy of
   // the License at
   //
   //     https://www.apache.org/licenses/LICENSE-2.0
   //
   // Unless required by applicable law or agreed to in writing, software
   // distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
   // WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
   // License for the specific language governing permissions and limitations under
   // the License.

   package {
       default_applicable_licenses: ["external_pigweed_license"],
   }

The ``bpfmt`` tool in Android can be used to format a blueprint file, e.g.
``bpfmt -w Android.bp``.

.. _module-pw_build_android-common-backends:

-----------------------
Common Android backends
-----------------------
Soong doesn't provide a simple way to select custom Pigweed backends, as is
found in other build systems. Fortunately, most Android platform binaries will
likely use the same common set of backends. Thus, Pigweed provides a
``cc_defaults`` rule named ``pw_android_common_backends`` with a selected set of
common backends for Android platform binaries. This
``pw_android_common_backends`` rule will be used in all Pigweed modules and
backends defined using Soong.

This table lists the backends selected by this rule:

.. list-table:: ``pw_android_common_backends``
   :align: left
   :header-rows: 1

   * - Facade
     - Selected Backend
   * - :ref:`module-pw_assert`
     - :ref:`module-pw_assert_log`
   * - :ref:`module-pw_log`
     - :ref:`module-pw_log_android`
   * - :ref:`module-pw_chrono`
     - :ref:`module-pw_chrono_stl`
   * - :ref:`module-pw_sync`
     - :ref:`module-pw_sync_stl`
   * - :ref:`module-pw_thread`
     - :ref:`module-pw_thread_stl`

.. _module-pw_build_android-module-libraries:

----------------
Module libraries
----------------
Module libraries are defined as ``cc_library_static`` rules and include the
common Android backends via the ``pw_android_common_backends`` defaults.

.. note::

   Every dependency has to be added as ``whole_static_libs`` to avoid dropping
   symbols on transitive dependencies.

.. note::

   The ``cc_library_static`` rule should be used even if the module includes no
   source files (only headers); ``cc_library_headers`` should not be used for
   normal Pigweed modules.

   Rationale: In Soong, dependencies are listed by type (e.g.
   ``cc_library_headers`` modules are referenced in a ``header_libs`` list,
   while ``cc_library_static`` modules are referenced in a ``static_libs``
   list).  That means the type of a module cannot be changed without breaking
   consumers.

.. code-block:: androidbp

   cc_library_static {
       name: "pw_<MODULE_NAME>",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
           "pw_android_common_backends",
       ],
       export_include_dirs: ["public"],
       header_libs: [
           // List of cc_library_headers dependencies needed to support
           // #include directives in the module's header files.
       ],
       export_header_lib_headers: [
           // List of cc_library_headers dependencies needed to support #include
           // directives in the module's public header files.
           // These entries must also be present in header_libs.
       ],
       whole_static_libs: [
           // List of cc_library_static dependencies, listed as whole libraries
           // to avoid dropping symbols in transitive dependencies.
       ],
       export_static_lib_headers: [
           // List of cc_library_static dependencies needed to support #include
           // directives in the module's public header files.
           // These entries must also be present in whole_static_libs.
       ],
       srcs: [
           // List of source (.cc) files.
       ],
   }

Module libraries with custom backends
-------------------------------------
If a Pigweed module needs to be used with a backend different than the common
Android backend, it should be defined as a ``cc_defaults`` rule following the
``pw_<MODULE_NAME>_no_backends`` name format, with the source files listed in a
``filegroup`` following the ``pw_<MODULE_NAME>_src_files`` name format and the
include directories defined as a ``cc_library_headers`` following the
``pw_<MODULE_NAME>_include_dirs`` name format. A ``cc_static_library`` with the
common Android backend should still be provided, which uses the mentioned
``cc_defaults``.

.. note::

   ``filegroup`` captures the absolute paths of the listed source files, so they
   can be addressed properly when the ``cc_defaults`` rule is used.

.. warning::

   Do not include ``vendor_available: true`` or other similar flags in a
   ``cc_defaults`` as this will conflict with downstream users of the defaults.

.. code-block:: androidbp

   filegroup {
       name: "pw_<MODULE_NAME>_src_files",
       srcs: [
           // List of source (.cc) files.
       ],
   }

   cc_library_headers {
       name: "pw_<MODULE_NAME>_include_dirs",
       defaults: [
           "pw_android_common_target_support",
       ],
       export_include_dirs: [
           "public",
       ],
   }

   cc_defaults {
       name: "pw_<MODULE_NAME>_no_backends",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
       ],

       header_libs: [
           // Header library list for all the libraries in #include directives.
           "pw_<MODULE_NAME>_include_dirs"
       ],
       export_header_lib_headers: [
           // Header library list for all the libraries in #include directives
           // in public header files only.
           // These entries must also be present in header_libs.
       ],
       whole_static_libs: [
           // Static library list for all static library dependencies, listed as
           // whole libraries to avoid dropping symbols in transitive
           // dependencies.
       ],
       export_static_lib_headers: [
           // Static library list for static libraries in #include directives in
           // public header files only.
           // These entries must also be present in whole_static_libs.
       ],
       srcs: [
           "pw_<MODULE_NAME>_src_files",
       ],
   }

   cc_library_static {
       name: "pw_<MODULE_NAME>",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
           "pw_android_common_backends",
           "pw_<MODULE_NAME>_no_backends",
       ],
       export_include_dirs: ["public"],
   }

Module libraries with configurable build flags
----------------------------------------------
If a Pigweed module provides user-configurable build flags it should be defined
as a ``cc_defaults`` rule following the ``pw_<MODULE_NAME>_defaults`` name
format. This hints the user that the rule must be initialized with the desired
build flag values. Source files must be listed in a ``filegroup`` following the
``pw_<MODULE_NAME>_src_files`` name format and the include directories defined
as a ``cc_library_headers`` following the ``pw_<MODULE_NAME>_include_dirs`` name
format.

.. code-block:: androidbp

   filegroup {
       name: "pw_<MODULE_NAME>_src_files",
       srcs: [
           // List of source (.cc) files.
       ],
   }

   cc_library_headers {
       name: "pw_<MODULE_NAME>_include_dirs",
       defaults: [
           "pw_android_common_target_support",
       ],
       export_include_dirs: [
           "public",
       ],
   }

   cc_defaults {
       name: "pw_<MODULE_NAME>_defaults",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
       ],

       header_libs: [
           // Header library list for all the libraries in #include directives.
           "pw_<MODULE_NAME>_include_dirs"
       ],
       export_header_lib_headers: [
           // Header library list for all the libraries in #include directives
           // in public header files only.
           // These entries must also be present in header_libs.
       ],
       whole_static_libs: [
           // Static library list for all static library dependencies, listed as
           // whole libraries to avoid dropping symbols in transitive
           // dependencies.
       ],
       export_static_lib_headers: [
           // Static library list for static libraries in #include directives in
           // public header files only.
           // These entries must also be present in whole_static_libs.
       ],
       srcs: [
           "pw_<MODULE_NAME>_src_files",
       ],
   }

A downstream user can instantiate the ``pw_<MODULE_NAME>_defaults`` rule as
follows.

.. note::

   To avoid collisions the rule using the ``cc_defaults`` must have a unique
   name that distinguishes it from other rule names in Pigweed and other
   projects. It is recommended to suffix the project name.

.. code-block:: androidbp

   cc_library_static {
       name: "pw_<MODULE_NAME>_<PROJECT_NAME>",
       cflags: [
           "-DPW_<MODULE_NAME>_<FLAG_NAME>=<FLAG_VALUE>",
       ],
       defaults: [
           "pw_android_common_target_support",
           "pw_<MODULE_NAME>_defaults",
       ],
   }

-------
Facades
-------
All facades must be defined as ``cc_library_headers`` if they don’t have
``srcs`` list. The facade names follow the ``pw_<MODULE_NAME>.<FACADE_NAME>``.
In the case there is only one facade in the module or ``<MODULE_NAME>`` is
the same as ``<FACADE_NAME>`` follow ``pw_<MODULE_NAME>``, e.g. ``pw_log``.

.. note::
   Facade names should not be suffixed with ``_headers``.

.. code-block:: androidbp

   cc_library_headers {
       name: "pw_<MODULE_NAME>.<FACADE_NAME>",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
       ],
       export_include_dirs: ["public"],
   }

If a facade requires a ``srcs`` list, it must be defined as a ``cc_defaults``
rule instead, with the source files listed in a ``filegroup`` following the
``pw_<MODULE_NAME>.<FACADE_NAME>_files`` name format or
``pw_<MODULE_NAME>_files`` if applicable.

.. note::

   ``filegroup`` captures the absolute paths of the listed source files, so they
   can be addressed properly when the ``cc_defaults`` rule is used.

.. note::

   Facades cannot be defined as ``cc_static_library`` because it wouldn’t be
   possible to build the facade without a backend.

.. code-block:: androidbp

   filegroup {
       name: "pw_<MODULE_NAME>.<FACADE_NAME>_files",
       srcs: [
           // List of source (.cc) files.
       ],
   }

   cc_defaults {
       name: "pw_<MODULE_NAME>.<FACADE_NAME>",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
       ],
       export_include_dirs: ["public"],
       srcs: [
           "pw_<MODULE_NAME>.<FACADE_NAME>_files",
       ],
   }

To assign a backend to a facade defined as ``cc_defaults`` the ``cc_defaults``
rule can be placed in the ``defaults`` list of a ``cc_static_library`` rule or
binary rule that lists the facade's backend as a dependency.

.. code-block:: androidbp

   cc_static_library {
       name: "user_of_pw_<MODULE_NAME>.<FACADE_NAME>",
       cpp_std: "c++20",
       defaults: [
           "pw_android_common_target_support",
           "pw_<MODULE_NAME>.<FACADE_NAME>",
       ],
       static_libs: [
           "backend_of_pw_<MODULE_NAME>.<FACADE_NAME>",
       ],
   }

Alternatively, the ``cc_defaults`` rule can be placed in the ``defaults`` list
of another ``cc_defaults`` rule where the latter rule may or may not list the
facade's backend. ``cc_defaults`` rules can be inherited many times. Facades
can be used as long as the backends are assigned in ``cc_static_library`` or
binary rules using the final ``cc_defaults`` as explained above.

--------
Backends
--------
Backends are defined the same way as
:ref:`module-pw_build_android-module-libraries`. They must follow the
``pw_<MODULE_NAME>.<FACADE_NAME>_<BACKEND_NAME>`` name format or
``pw_<MODULE_NAME>_<BACKEND_NAME>`` if applicable.

-----------
Build flags
-----------
Some build flags should be set for all Android targets; these flags are
specified in ``pw_android_common_backends``. These flags are as follows:

``PW_FUNCTION_ENABLE_DYNAMIC_ALLOCATION``
-----------------------------------------
As discussed in :ref:`module-pw_function-dynamic-allocation`, this flag enables
dynamic allocation of :cpp:type:`pw::Function`, allowing it to exceed the
inline size limit.

Android targets support dynamic allocation since the Android environment is not
memory constrained. Thus, ``PW_FUNCTION_ENABLE_DYNAMIC_ALLOCATION`` is enabled
in ``pw_android_common_backends``. Components built with dynamic allocation
disabled cannot be linked against components with dynamic allocation enabled.
