.. _zephyr-backends:

====================
Backends and linking
====================
How a Zephyr application chooses Pigweed's backends with Kconfig, and how
Pigweed libraries are linked into a Zephyr image.

.. note:: The version of Zephyr Pigweed is tested against is v4.4.

.. _zephyr-backend-selection:

---------------------------
How backend selection works
---------------------------
Each facade with a Pigweed Zephyr backend (see the modules listed in
:ref:`docs-os-zephyr`) is a Kconfig ``choice``. For example, ``choice
PIGWEED_CHRONO_SYSTEM_CLOCK_BACKEND`` has two members:

* ``PIGWEED_CHRONO_SYSTEM_CLOCK`` -- use Pigweed's Zephyr backend.
* ``PIGWEED_CHRONO_SYSTEM_CLOCK_NONE`` -- provide your own backend.

Each module's docs list its choices (for example,
:ref:`module-pw_chrono_zephyr`, :ref:`module-pw_sync_zephyr`).

Every choice defaults to Pigweed's Zephyr backend; where a facade has
several Pigweed backends, such as logging, that's the plain Zephyr one.
Selecting a backend only tells CMake which library implements the facade;
nothing is linked unless something uses it. The exception is
``pw_sys_io``, which defaults to "none" because its backend registers
itself with ``SYS_INIT`` and always links in (see
:ref:`module-pw_sys_io_zephyr`).

If a facade you use has no backend, ``pw_build`` fails the build and names
it:

.. code-block:: text

   ERROR: Attempted to build the pw_sync.mutex facade with no backend
   set. Configure the pw_sync.mutex backend using pw_set_backend or
   remove all dependencies on it. See https://pigweed.dev/pw_build.

Using your own backend
======================
To replace one of Pigweed's Zephyr backends with your own library:

#. Select the facade's ``_NONE`` member in your ``prj.conf``. Otherwise
   Pigweed's default backend replaces yours.

   .. code-block:: kconfig

      CONFIG_PIGWEED_SYNC_MUTEX_NONE=y

#. Call ``pw_set_backend()`` in your application's ``CMakeLists.txt``
   *before* ``find_package(Zephyr)``. Zephyr processes Pigweed's CMake inside
   ``find_package(Zephyr)``, and each facade reads its backend when it's
   created.

   .. code-block:: cmake

      include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
      include($ENV{PW_ROOT}/pw_sync/backend.cmake)
      pw_set_backend(pw_sync.mutex my_mutex_backend)

      find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

#. Define ``my_mutex_backend`` anywhere in your build, for example with
   ``pw_add_library()``. It provides the facade's backend headers, as
   described in the facade's own docs.

.. _zephyr-backend-selection-cmake:

Adding a backend to Pigweed
===========================
Pigweed's own Zephyr backends are wired up in ``zephyr/backends.cmake`` with
``pw_set_zephyr_backend_ifdef()``, keyed on a Kconfig ``choice`` member
declared in the backend module's ``Kconfig``:

.. code-block:: cmake

   pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK
       pw_chrono.system_clock pw_chrono_zephyr.system_clock
       ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_chrono/backend.cmake)

This calls ``pw_set_backend()`` when the Kconfig condition is set. If two
different conditions set different backends for the same facade, the build
fails and names both; put the options in one ``choice`` to fix it.

.. _zephyr-linking-libraries:

-----------------------------------
Linking libraries in a Zephyr build
-----------------------------------
In a Zephyr build, every non-``INTERFACE`` library created with
``pw_add_library()`` depends on ``zephyr_generated_headers``, so headers
Zephyr generates at build time (such as ``heap_constants.h``) exist before
it compiles.

Link the Pigweed libraries you use into your application, for example with
``target_link_libraries(app PRIVATE pw_rpc.server)``. Enabling a
``CONFIG_PIGWEED_*`` option doesn't link anything by itself. Never link a
Pigweed static library *into* ``zephyr_interface`` (for example with
``zephyr_link_libraries()``): Zephyr's header generators inherit
``zephyr_interface``, so that creates a dependency cycle.

To give your own library Zephyr's compile flags and include paths, use
``pw_zephyrize_libraries_ifdef()``:

.. code-block:: cmake

   pw_zephyrize_libraries_ifdef(CONFIG_MY_FEATURE my_library)

This also gives Zephyr's own sources the library's include paths, for headers
Zephyr includes from the application, such as ``zephyr_custom_log.h`` with
``CONFIG_LOG_CUSTOM_HEADER``. It needs CMake 3.27 or newer.

It does **not** link the library, or add it to ``ZEPHYR_LIBS``, the list
``zephyr_library()`` uses. Everything in ``ZEPHYR_LIBS`` is linked with
``--whole-archive``, which Pigweed libraries don't need and which turns
harmless static-library cycles (such as ``pw_rpc`` and ``pw_log``) into
"multiple definition" link errors.

Use ``zephyr_append_cmake_library()`` only for code that no application
target can depend on. Pigweed does this for two libraries:

* ``pw_sys_io_zephyr``, which registers itself with ``SYS_INIT`` and has no
  callers.
* ``pw_log_zephyr.pigweed_overrides``, when Zephyr's log macros are mapped to
  Pigweed (for example with ``CONFIG_PIGWEED_LOG_STRING``). Zephyr's own code
  then logs through ``pw_log``, so this library links ``pw_log``, with its
  backend, and ``pw_hex_dump.log_bytes``.

Migrating from Kconfig-linked libraries
=======================================
Pigweed libraries used to be linked into every Zephyr application whose
Kconfig enabled them. They're now linked only when a CMake target depends on
them, like on other build systems. If your build now fails with undefined
references to Pigweed symbols, link the libraries your code uses:

.. code-block:: cmake

   target_link_libraries(app PRIVATE pw_rpc.server pw_log)

To find the target for a ``CONFIG_PIGWEED_*`` option, look in
``zephyr/CMakeLists.txt`` and ``zephyr/backends.cmake``, or in the module's
own ``CMakeLists.txt``. ``CONFIG_PIGWEED_ALLOCATOR`` provides
``pw_allocator_zephyr.system_heap_allocator`` and
``pw_allocator_zephyr.synchronized_heap_allocator``.
