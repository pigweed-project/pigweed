.. _docs-os-embos-setup:

=====
Setup
=====
The ``//third_party/embos`` directory in Pigweed contains build system
integration helpers for SEGGER embOS. Pigweed's embOS support is currently
focused on the GN build system.

-------------
Build support
-------------
To use Pigweed's embOS integration in your GN build, set the following variables:

#. Set the GN variable ``dir_pw_third_party_embos_include`` to the path of the
   embOS include directory.
#. Set ``pw_third_party_embos_PORT`` to the path of a ``pw_source_set`` that
   provides the embOS port-specific includes and sources.

After these are configured, a ``pw_source_set`` for the embOS library is created
at ``$pw_external_embos``.

-----------------------
Toolchain configuration
-----------------------
To use the embOS backends, configure your toolchain's backend variables to point
to the embOS backend targets.

GN toolchain configuration
==========================
In your toolchain configuration file (typically a ``.gni`` file defining your
target's toolchain), set the following backend variables:

.. code-block:: none

   # Chrono backend
   pw_chrono_SYSTEM_CLOCK_BACKEND = "$dir_pw_chrono_embos:system_clock"
   pw_chrono_SYSTEM_TIMER_BACKEND = "$dir_pw_chrono_embos:system_timer"

   # Sync backends
   pw_sync_MUTEX_BACKEND = "$dir_pw_sync_embos:mutex"
   pw_sync_TIMED_MUTEX_BACKEND = "$dir_pw_sync_embos:timed_mutex"
   pw_sync_INTERRUPT_SPIN_LOCK_BACKEND = "$dir_pw_sync_embos:interrupt_spin_lock"
   pw_sync_BINARY_SEMAPHORE_BACKEND = "$dir_pw_sync_embos:binary_semaphore"
   pw_sync_COUNTING_SEMAPHORE_BACKEND = "$dir_pw_sync_embos:counting_semaphore"

   # ThreadNotification backends (uses binary semaphores)
   pw_sync_THREAD_NOTIFICATION_BACKEND = "$dir_pw_sync:binary_semaphore_thread_notification_backend"
   pw_sync_TIMED_THREAD_NOTIFICATION_BACKEND = "$dir_pw_sync:binary_semaphore_timed_thread_notification_backend"

   # Thread backends
   pw_thread_ID_BACKEND = "$dir_pw_thread_embos:id"
   pw_thread_SLEEP_BACKEND = "$dir_pw_thread_embos:sleep"
   pw_thread_THREAD_BACKEND = "$dir_pw_thread_embos:thread"
   pw_thread_YIELD_BACKEND = "$dir_pw_thread_embos:yield"

Clock backend verification
==========================
Pigweed's embOS synchronization and thread sleep backends require the embOS
system clock backend to function correctly. By default, a build-time assertion
verifies that ``pw_chrono_SYSTEM_CLOCK_BACKEND`` is set to
``$dir_pw_chrono_embos:system_clock``.

If you have a custom clock configuration and need to bypass this assertion, you
can set:

* ``pw_sync_OVERRIDE_SYSTEM_CLOCK_BACKEND_CHECK = true`` (in ``pw_sync_embos``)
* ``pw_thread_OVERRIDE_SYSTEM_CLOCK_BACKEND_CHECK = true`` (in ``pw_thread_embos``)

--------------
Initialization
--------------
embOS requires that ``OS_Init()`` is invoked before any other embOS API is used.
This applies to synchronization primitives initialized during global C++ static construction.

If you are using :ref:`module-pw_boot_cortex_m`, you should invoke ``OS_Init()``
inside ``pw_boot_PreStaticConstructorInit()`` to guarantee correct ordering.

-------------
Configuration
-------------
You can customize the ``pw_thread_embos`` backend compile-time settings by
defining a backend configuration override target and pointing the GN variable
``pw_thread_embos_CONFIG`` to it.

For details on the configurable compile-time options, see the
:ref:`module-pw_thread_embos` reference documentation. The primary
configurable macros include:

* ``PW_THREAD_EMBOS_CONFIG_JOINING_ENABLED``
* ``PW_THREAD_EMBOS_CONFIG_MINIMUM_STACK_SIZE_WORDS``
* ``PW_THREAD_EMBOS_CONFIG_DEFAULT_STACK_SIZE_WORDS``
* ``PW_THREAD_EMBOS_CONFIG_MAX_THREAD_NAME_LEN``
* ``PW_THREAD_EMBOS_CONFIG_MIN_PRIORITY``
* ``PW_THREAD_EMBOS_CONFIG_DEFAULT_PRIORITY``
* ``PW_THREAD_EMBOS_CONFIG_DEFAULT_TIME_SLICE_INTERVAL``
* ``PW_THREAD_EMBOS_CONFIG_LOG_LEVEL``
