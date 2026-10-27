.. _module-pw_system:

=========
pw_system
=========
.. pigweed-module::
   :name: pw_system

.. warning::
  This module is an early work-in-progress towards an opinionated framework for
  new projects built on Pigweed. It is under active development, so stay tuned!

pw_system is quite different from typical Pigweed modules. Rather than providing
a single slice of vertical functionality, pw_system pulls together many modules
across Pigweed to construct a working system with RPC, Logging, an OS
Abstraction layer, and more. pw_system exists to greatly simplify the process
of starting a new project using Pigweed by drastically reducing the required
configuration space required to go from first signs of on-device life to a more
sophisticated production-ready system.

--------------------
Trying out pw_system
--------------------
If you'd like to give pw_system a spin and have a STM32F429I Discovery board,
refer to the board's
:ref:`target documentation<target-stm32f429i-disc1-stm32cube>` for instructions
on how to build the demo and try things out

If you don't have a discovery board, there's a simulated device variation that
you can run on your local machine with no additional hardware. Check out the
steps for trying this out :ref:`here<target-host-device-simulator>`.

--------------
Target Bringup
--------------
Bringing up a new device is as easy as 1-2-3! (Kidding, this is a work in
progress)

#. **Configure the build.** How exactly to do this depends on the build
   system.

   *  **GN**: Create a ``pw_system_target`` in your GN build. This is what will
      control the configuration of your target from a build system level. This
      includes which compiler will be used, what architecture flags will be
      used, which backends will be used, and more. A large quantity of
      configuration will be pre-set to work with pw_system after you select the
      CPU and scheduler your target will use, but your target will likely need
      to set a few other things to get to a fully working state.

   *  **Bazel**: Add a dependency on ``@pigweed//pw_system`` to your ``cc_binary``,
      and set one `label flag
      <https://bazel.build/extending/config#label-typed-build-settings>`__,
      ``@pigweed//pw_system:extra_platform_libs``. Point it to a ``cc_library``
      containing any platform-dependent dependencies of your ``pw_system``
      instantiation. In particular, this should include platform-specific
      initialization code (see next point) and the custom
      :ref:`pw_linker_script <module-pw_build-bazel-pw_linker_script>` (if any)
      to use when linking the ``pw_system`` binary.

      .. warning::

         You should always add the ``alwayslink = 1`` attribute to the target
         you point ``@pigweed//pw_system:extra_platform_libs`` to. This is
         because Bazel `links files in topological order
         <https://stackoverflow.com/a/73006724/24291280>`__, but the
         dependencies from ``extra_platform_libs`` may appear before the
         objects they are used in. The ``alwayslink = 1`` will prevent the
         linker from erroneously garbage-collecting them.

#. **Write target-specific initialization.**
   Most embedded devices require a linker script, manual initialization of
   memory, and some clock initialization. pw_system leaves this to users to
   implement as the exact initialization sequence can be very project-specific.
   All that's required is that after early memory initialization and clock
   configuration is complete, your target initialization should call
   ``pw::system::Init()`` and then start the RTOS scheduler (e.g.
   ``vTaskStartScheduler()``).
#. **Implement** ``pw::system::UserAppInit()`` **in your application.**
   This is where most of your project's application-specific logic goes. This
   could be starting threads, registering RPC services, turning on Bluetooth,
   or more. In ``UserAppInit()``, the RTOS will be running so you're free to use
   OS primitives and use features that rely on threading (e.g. RPC, logging).

Pigweed's ``stm32f429i_disc1_stm32cube`` target demonstrates what's required by
the first two steps. The third step is where you get to decide how to turn your
new platform into a project that does something cool! It might be as simple as
a blinking LED, or something more complex like a Bluetooth device that brews you
a cup of coffee whenever ``pw watch`` kicks off a new build.

.. note::
  Because of the nature of the hard-coded conditions in ``pw_system_target``,
  you may find that some options are missing for various RTOSes and
  architectures. The design of the GN integration is still a work-in-progress
  to improve the scalability of this, but in the meantime the Pigweed team
  welcomes contributions to expand the breadth of RTOSes and architectures
  supported as ``pw_system_target``\s.

GN Target Toolchain Template
============================
This module includes a target toolchain template called ``pw_system_target``
that reduces the amount of work required to declare a target toolchain with
pre-selected backends for pw_log, pw_assert, pw_malloc, pw_thread, and more.
The configurability and extensibility of this template is relatively limited,
as this template serves as a "one-size-fits-all" starting point rather than
being foundational infrastructure.

.. code-block::

   # Declare a toolchain with suggested, compiler, compiler flags, and default
   # backends.
   pw_system_target("stm32f429i_disc1_stm32cube_size_optimized") {
     # These options drive the logic for automatic configuration by this
     # template.
     cpu = PW_SYSTEM_CPU.CORTEX_M4F
     scheduler = PW_SYSTEM_SCHEDULER.FREERTOS

     # Optionally, override pw_system's defaults to build with clang.
     system_toolchain = pw_toolchain_arm_clang

     # The pre_init source set provides things like the interrupt vector table,
     # pre-main init, and provision of FreeRTOS hooks.
     link_deps = [ "$dir_pigweed/targets/stm32f429i_disc1_stm32cube:pre_init" ]

     # These are hardware-specific options that set up this particular board.
     # These are declared in ``declare_args()`` blocks throughout Pigweed. Any
     # build arguments set by the user will be overridden by these settings.
     build_args = {
       pw_third_party_freertos_CONFIG = "$dir_pigweed/targets/stm32f429i_disc1_stm32cube:stm32f4xx_freertos_config"
       pw_third_party_freertos_PORT = "$pw_external_freertos:arm_cm4f"
       pw_sys_io_BACKEND = dir_pw_sys_io_stm32cube
       dir_pw_third_party_stm32cube = dir_pw_third_party_stm32cube_f4
       pw_third_party_stm32cube_PRODUCT = "STM32F429xx"
       pw_third_party_stm32cube_CONFIG =
           "//targets/stm32f429i_disc1_stm32cube:stm32f4xx_hal_config"
       pw_third_party_stm32cube_CORE_INIT = ""
       pw_boot_cortex_m_LINK_CONFIG_DEFINES = [
         "PW_BOOT_FLASH_BEGIN=0x08000200",
         "PW_BOOT_FLASH_SIZE=2048K",
         "PW_BOOT_HEAP_SIZE=7K",
         "PW_BOOT_MIN_STACK_SIZE=1K",
         "PW_BOOT_RAM_BEGIN=0x20000000",
         "PW_BOOT_RAM_SIZE=192K",
         "PW_BOOT_VECTOR_TABLE_BEGIN=0x08000000",
         "PW_BOOT_VECTOR_TABLE_SIZE=512",
       ]
     }
   }

   # Example for the Emcraft SmartFusion2 system-on-module
   pw_system_target("emcraft_sf2_som_size_optimized") {
     cpu = PW_SYSTEM_CPU.CORTEX_M3
     scheduler = PW_SYSTEM_SCHEDULER.FREERTOS

     link_deps = [ "$dir_pigweed/targets/emcraft_sf2_som:pre_init" ]
     build_args = {
       pw_log_BACKEND = dir_pw_log_basic #dir_pw_log_tokenized
       pw_log_tokenized_HANDLER_BACKEND = "//pw_system:log"
       pw_third_party_freertos_CONFIG = "$dir_pigweed/targets/emcraft_sf2_som:sf2_freertos_config"
       pw_third_party_freertos_PORT = "$pw_external_freertos:arm_cm3"
       pw_sys_io_BACKEND = dir_pw_sys_io_emcraft_sf2
       dir_pw_third_party_smartfusion_mss = dir_pw_third_party_smartfusion_mss_exported
       pw_third_party_stm32cube_CONFIG =
           "//targets/emcraft_sf2_som:sf2_mss_hal_config"
       pw_third_party_stm32cube_CORE_INIT = ""
       pw_boot_cortex_m_LINK_CONFIG_DEFINES = [
         "PW_BOOT_FLASH_BEGIN=0x00000200",
         "PW_BOOT_FLASH_SIZE=200K",

         # TODO: b/235348465 - Currently "pw_tokenizer/detokenize_test" requires at
         # least 6K bytes in heap when using pw_malloc:bucket_allocator.
         # The heap size required for tests should be investigated.
         "PW_BOOT_HEAP_SIZE=7K",
         "PW_BOOT_MIN_STACK_SIZE=1K",
         "PW_BOOT_RAM_BEGIN=0x20000000",
         "PW_BOOT_RAM_SIZE=64K",
         "PW_BOOT_VECTOR_TABLE_BEGIN=0x00000000",
         "PW_BOOT_VECTOR_TABLE_SIZE=512",
       ]
     }
   }

-------
Metrics
-------
The log backend is tracking metrics to illustrate how to use pw_metric and
retrieve them using `Device.get_and_log_metrics()`.

-------
Console
-------
The ``pw-system-console`` can be used to interact with the targets.
See :ref:`module-pw_system-cli` for detailed CLI usage information.

.. toctree::
   :hidden:
   :maxdepth: 1

   cli

-------------------
Multi-endpoint mode
-------------------

The default configuration serves all its traffic with the same
channel ID and RPC address. There is an alternative mode that assigns a separate
channel ID and address for logging. This can be useful if you want to separate logging and primary RPC to
``pw_system`` among multiple clients.

To use this mode, add the following to ``gn args out``:

.. code-block::

   pw_system_USE_MULTI_ENDPOINT_CONFIG = true

The settings for the channel ID and address can be found in the target
``//pw_system:multi_endpoint_rpc_overrides``.

.. _module-pw_system-logchannel:

---------------------
Extra logging channel
---------------------
In multi-processor devices, logs are typically forwarded to a primary
application-class core. By default, ``pw_system`` assumes a simpler device
architecture where a single processor is communicating with an external host
system (e.g. a Linux workstation) for developer debugging. This means that
logging and RPCs are expected to coexist on the same channel. It is possible
to redirect the logs to a different RPC channel output by configuring
``PW_SYSTEM_LOGGING_CHANNEL_ID`` to a different channel ID, but this would
still mean that logs would inaccessible from either the application-class
processor, or the host system.

The logging multisink can be leveraged to address this completely by forwarding
a copy of the logs to the application-class core without impacting the behavior
of the debug communication channel. This allows ``pw-system-console`` work as
usual, while also ensuring logs are available from the larger application-class
processor. Additionally, this allows the debug channel to easily be disabled in
production environments while maintaining the log forwarding path through the
larger processor.

An example configuration is provided below:

.. code-block::

   config("extra_logging_channel") {
     defines = [ "PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID=2" ]
   }

   pw_system_target("my_system") {
     global_configs = [ ":extra_logging_channel" ]
   }

Once you have configured pw_system as shown in the example above, you will
still need to define an RPC channel for the channel ID that you selected so
the logs can be routed to the appropriate destination.

---------------
pw_system:async
---------------
``pw_system:async`` is a new version of ``pw_system`` based on
:ref:`module-pw_async2` and :ref:`module-pw_channel`. It provides an async
dispatcher, which may be used to run async tasks, including with C++20
coroutines.

To use ``pw_system:async``, add a dependency on ``@pigweed//pw_system:async`` in
Bazel. After performing any initialization, call
:cc:`pw::system::StartAndClobberTheStack` with a
:cc:`pw::channel::ByteReaderWriter` to use for IO to start the RTOS scheduler.

.. important::

  Some RTOSes (and with FreeRTOS, with some but not all architecture ports) the
  call to start the scheduler takes over the callers stack, and will trash any
  values stored on it. As this could lead to hard to diagnose runtime behavior,
  we recommend guarding against accidentally storing any registration structures
  on the stack by performing any initialization in a function called from main
  that then returns back to main for the
  :cc:`pw::system::StartAndClobberTheStack` call.

  This is shown in the example below, or you can open up
  :cs:`pw_system/system_async_test.cc`.

.. literalinclude:: system_async_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_system-async-example-main]
   :end-before: [pw_system-async-example-main]

pw_system:async Linux example
=============================
``//pw_system/system_async_host_simulator_example`` is an example app for
running ``pw_system:async`` on a Linux host. Running the example requires two
terminals. In the first terminal, start the ``pw_system:async`` instance:

.. code-block:: console

   $ bazelisk run //pw_system/system_async_host_simulator_example

That will wait for a TCP connection from the ``pw_system`` console. To connect
to it from the console, run the following:

.. code-block:: console

   $ bazelisk run //pw_system/py:pw_system_console -- -s 127.0.0.1:33000

---------
Debugging
---------

Debugging pw_system_console with VSCode
=======================================
When running a python script through bazel, python is run inside a bazel sandbox,
which can make re-creating this environment difficult when running the script
outside of bazel to attach a debugger.

To simplify debugging setup, Pigweed makes available the `debugpy <https://github.com/microsoft/debugpy>`__
package to ease attaching ``pw_system_console``.

First configure VSCode to add the following to the configuration section of ``launch.json``.
This file can be automatically opened by selecting ``Run -> Open Configurations```, or
``Run -> Add Configuration`` if there is no existing ``launch.json``.

.. TODO: b/372079357 - can this be automated by the VSCode plugin?
.. code-block:: json

   "configurations": [
   {
     "name": "Python Debugger: Remote Attach",
     "type": "debugpy",
     "request": "attach",
     "connect": {
       "host": "localhost",
       "port": 5678
     },
     "pathMappings": [
       {
         "localRoot": "${workspaceFolder}",
         "remoteRoot": "."
       }
     ]
   }

  ]

Next, run the console through bazel, adding the argument(s) ``--debugger-listen`` and optionally
``--debugger-wait-for-client`` to pause the console until the debugger attached.  For example:

.. code-block:: console

   $ bazelisk run //pw_system/py:pw_system_console -- --debugger-listen

Once the console has been started, simply select ``Run -> Start Debugging`` and the VS code debugger
will automatically attach to the running python console.

---------
Benchmark
---------

pw_system.benchmark_runner
==========================
.. automodule:: pw_system.benchmark_runner
  :members:
    Runner

-------------
API reference
-------------
Moved: :cc:`pw_system`
