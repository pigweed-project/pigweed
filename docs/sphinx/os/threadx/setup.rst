.. _docs-os-threadx-setup:

=====
Setup
=====
The ``//third_party/threadx`` directory in Pigweed contains build system
integration helpers for ThreadX. Pigweed's ThreadX support is currently focused
on the GN build system.

-------------
Build support
-------------
To use Pigweed's ThreadX integration in your GN build, set the following variables:

#. Set the GN variable ``dir_pw_third_party_threadx_include`` to the path of
   the ThreadX include directory.
#. Set ``pw_third_party_threadx_PORT`` to the path of a ``pw_source_set`` that
   provides the ThreadX port-specific includes and sources.

After these are configured, a ``pw_source_set`` for the ThreadX library is created
at ``$pw_external_threadx``.

-----------------------
Toolchain configuration
-----------------------
To use the ThreadX backends, configure your toolchain's backend variables to point
to the ThreadX backend targets.

GN toolchain configuration
==========================
In your toolchain configuration file (typically a ``.gni`` file defining your
target's toolchain), set the following backend variables:

.. code-block:: none

   # Chrono backend
   pw_chrono_SYSTEM_CLOCK_BACKEND = "$dir_pw_chrono_threadx:system_clock"

   # Sync backends
   pw_sync_MUTEX_BACKEND = "$dir_pw_sync_threadx:mutex"
   pw_sync_TIMED_MUTEX_BACKEND = "$dir_pw_sync_threadx:timed_mutex"
   pw_sync_INTERRUPT_SPIN_LOCK_BACKEND = "$dir_pw_sync_threadx:interrupt_spin_lock"
   pw_sync_BINARY_SEMAPHORE_BACKEND = "$dir_pw_sync_threadx:binary_semaphore"
   pw_sync_COUNTING_SEMAPHORE_BACKEND = "$dir_pw_sync_threadx:counting_semaphore"

   # ThreadNotification backends (uses binary semaphores)
   pw_sync_THREAD_NOTIFICATION_BACKEND = "$dir_pw_sync:binary_semaphore_thread_notification_backend"
   pw_sync_TIMED_THREAD_NOTIFICATION_BACKEND = "$dir_pw_sync:binary_semaphore_timed_thread_notification_backend"

   # Thread backends
   pw_thread_ID_BACKEND = "$dir_pw_thread_threadx:id"
   pw_thread_SLEEP_BACKEND = "$dir_pw_thread_threadx:sleep"
   pw_thread_THREAD_BACKEND = "$dir_pw_thread_threadx:thread"
   pw_thread_YIELD_BACKEND = "$dir_pw_thread_threadx:yield"

Clock backend verification
==========================
Pigweed's ThreadX synchronization and thread sleep backends require the ThreadX
system clock backend to function correctly. By default, a build-time assertion
verifies that ``pw_chrono_SYSTEM_CLOCK_BACKEND`` is set to
``$dir_pw_chrono_threadx:system_clock``.

If you have a custom clock configuration and need to bypass this assertion, you
can set:

- ``pw_sync_OVERRIDE_SYSTEM_CLOCK_BACKEND_CHECK = true`` (in ``pw_sync_threadx``)
- ``pw_thread_OVERRIDE_SYSTEM_CLOCK_BACKEND_CHECK = true`` (in ``pw_thread_threadx``)

-------------
Configuration
-------------
You can customize the ``pw_thread_threadx`` backend compile-time settings by
defining a backend configuration override target and pointing the GN variable
``pw_thread_threadx_CONFIG`` to it.

For details on the configurable compile-time options, see the
:ref:`module-pw_thread_threadx` reference documentation. The primary
configurable macros include:

* ``PW_THREAD_THREADX_CONFIG_JOINING_ENABLED``
* ``PW_THREAD_THREADX_CONFIG_DEFAULT_STACK_SIZE_WORDS``
* ``PW_THREAD_THREADX_CONFIG_MAX_THREAD_NAME_LEN``
* ``PW_THREAD_THREADX_CONFIG_DEFAULT_TIME_SLICE_INTERVAL``
* ``PW_THREAD_THREADX_CONFIG_MIN_PRIORITY``
* ``PW_THREAD_THREADX_CONFIG_DEFAULT_PRIORITY``
* ``PW_THREAD_THREADX_CONFIG_LOG_LEVEL``
