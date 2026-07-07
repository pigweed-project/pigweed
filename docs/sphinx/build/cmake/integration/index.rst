.. _cmake-toolchains: https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html

.. _cmake-integration:

=======================
CMake integration guide
=======================
This page provides guidance on various topics related to using Pigweed in
CMake projects.

* For basic setup guidance and to see a minimal, complete example of Pigweed
  integrated into a CMake project, see :ref:`cmake-quickstart`.

* For help with the :ref:`docs-glossary-upstream` CMake build system,
  see :ref:`docs-contributing-build-cmake`.

.. _cmake-integration-facade:

--------------------
Facades and backends
--------------------
The CMake build uses CMake cache variables for configuring
:ref:`facades and backends <docs-module-structure-facades>`. Cache variables are
similar to GN's build args set with ``gn args``. Unlike GN, CMake does not
support multi-toolchain builds, so these variables have a single global value
per build directory.

.. tip::
   Because CMake requires separate build directories for different toolchains
   (e.g., host vs. device), you can use the :ref:`module-pw_build-project_builder`
   (via the ``pw build`` command) to orchestrate and run builds across multiple
   directories in parallel.

The :ref:`cmake-api-pw_add_facade` function declares a cache variable
named ``<module_name>_BACKEND`` for each facade. Cache variables can be awkward
to work with, since their values only change when they're assigned, but then
persist across CMake invocations. These variables should be set in one of the
following ways:

* Prior to setting a backend, your application should include
  ``$ENV{PW_ROOT}/backends.cmake``. This file will set up all the backend targets
  such that any misspelling of a facade or backend will yield a warning.

  .. note::

     Zephyr developers do not need to do this, backends can be set automatically
     by enabling the appropriate Kconfig options.

* Call ``pw_set_backend`` to set backends appropriate for the target in the
  target's toolchain file. The toolchain file is provided to ``cmake`` with
  ``-DCMAKE_TOOLCHAIN_FILE=<toolchain file>``.

* Call ``pw_set_backend`` in the top-level ``CMakeLists.txt`` before other
  CMake code executes.

* Set the backend variable at the command line with the ``-D`` option.

  .. code-block:: console

     cmake -B out/cmake_host -S "$PW_ROOT" -G Ninja \
         -DCMAKE_TOOLCHAIN_FILE=$PW_ROOT/pw_toolchain/host_clang/toolchain.cmake \
         -Dpw_log_BACKEND=pw_log_basic

* Temporarily override a backend by setting it interactively with ``ccmake`` or
  ``cmake-gui``.

If the backend is set to a build target that does not exist, there will be an
error message like the following:

.. code-block:: text

   CMake Error at pw_build/pigweed.cmake:257 (message):
     my_module.my_facade's INTERFACE dep "my_nonexistent_backend" is not
     a target.
   Call Stack (most recent call first):
     pw_build/pigweed.cmake:238:EVAL:1 (_pw_target_link_targets_deferred_check)
     CMakeLists.txt:DEFERRED

.. _cmake-integration-toolchain:

---------------
Toolchain setup
---------------
In CMake, the toolchain is configured by setting CMake variables, as described
in `cmake-toolchains`_.  These variables are typically set in a toolchain
CMake file passed to ``cmake`` with the ``-D`` option
(``-DCMAKE_TOOLCHAIN_FILE=path/to/file.cmake``).  For Pigweed embedded builds,
set ``CMAKE_SYSTEM_NAME`` to the empty string (``""``).

Toolchains may set the ``pw_build_WARNINGS`` variable to a list of ``INTERFACE``
libraries with compilation options for Pigweed's upstream libraries. This
defaults to a strict set of warnings. Projects may need to use less strict
compilation warnings to compile backends exposed to Pigweed code (such as
``pw_log``) that cannot compile with Pigweed's flags. If desired, projects can
access these warnings by depending on ``pw_build.warnings``.

.. _cmake-integration-3p:

---------------------
Third-party libraries
---------------------
The CMake build includes third-party libraries similarly to the GN build. A
``dir_pw_third_party_<library>`` cache variable is defined for each third-party
dependency. The variable must be set to the absolute path of the library in
order to use it. If the variable is empty
(``if("${dir_pw_third_party_<library>}" STREQUAL "")``), the dependency is not
available.

Third-party dependencies are not automatically added to the build. They can be
manually added with ``add_subdirectory`` or by setting the
``pw_third_party_<library>_ADD_SUBDIRECTORY`` option to ``ON``.

Third party variables are set like any other cache global variable in CMake. It
is recommended to set these in one of the following ways:

* Set with the CMake ``set`` function in the toolchain file or a
  ``CMakeLists.txt`` before other CMake code executes.

  .. code-block:: cmake

     set(dir_pw_third_party_nanopb ${CMAKE_CURRENT_SOURCE_DIR}/external/nanopb CACHE PATH "" FORCE)

* Set the variable at the command line with the ``-D`` option.

  .. code-block:: console

     cmake -B out/cmake_host -S "$PW_ROOT" -G Ninja \
         -DCMAKE_TOOLCHAIN_FILE=$PW_ROOT/pw_toolchain/host_clang/toolchain.cmake \
         -Ddir_pw_third_party_nanopb=/path/to/nanopb

* Set the variable interactively with ``ccmake`` or ``cmake-gui``.

.. _cmake-integration-sandbox:

---------------------------------
Optional sandboxing for C++ and C
---------------------------------
Libraries declared with :ref:`cmake-api-pw_add_library` or
:ref:`cmake-api-pw_add_library_generic` can optionally compile from a sandbox.
This feature is enabled globally by setting the ``pw_ENABLE_CC_SANDBOX``
option:

.. code-block:: cmake

   set(pw_ENABLE_CC_SANDBOX ON CACHE BOOL "")

Individual libraries may enable or disable sandboxing by setting the ``SANDBOX``
option to ``ON`` or ``OFF``:

.. literalinclude:: ../../../pw_build/CMakeLists.txt
   :language: cmake
   :start-after: # DOCSTAG[pw_build-cmake-sandbox-example]
   :end-before: # DOCSTAG[pw_build-cmake-sandbox-example]

Sandboxed libraries can only see files they list as sources or headers during
compilation. Files declared in sandboxed libraries cannot be seen by other
libraries unless those libraries express that dependency.

This feature helps ensure build correctness. It can avoid serious issues, like
failing to add a config dependency and getting a default configuration instead
of a customized one. Bugs like that can result in ODR violations and undefined
behavior.

Pigweed's CMake sandboxing feature is similar to Bazel's, but it is much simpler
and less robust. It makes no guarantees of hermeticity.

Sandboxing is enabled by default for upstream Pigweed host builds.
