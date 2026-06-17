.. _module-pw_kernel-quickstart:

==========
Quickstart
==========
.. pigweed-module-subpage::
   :name: pw_kernel

The easiest and fastest way to get started with ``pw_kernel`` is to copy our
full-featured adventure example (:cs:`pw_kernel/examples/adventure`) and strip
out whatever you don't need. Although the code is in
:ref:`docs-glossary-upstream` the example is structured as an out-of-tree (i.e.
self-contained) Bazel project, so you should be able to get it up and running
within minutes.

.. _module-pw_kernel-quickstart-setup:

-----
Setup
-----
1. Clone the upstream Pigweed repo:

   .. code-block:: console

      git clone https://pigweed.googlesource.com/pigweed/pigweed --depth 1

2. Move the :cs:`pw_kernel/examples/adventure` directory out of upstream
   Pigweed:

   .. code-block:: console

      mv pigweed/pw_kernel/examples/adventure app

   You can call it whatever you want but we'll assume ``app`` throughtout
   this quickstart.

3. Make ``app`` your working dir:

   .. code-block:: console

      cd app

4. Open ``//MODULE.bazel``, uncomment the `git_override`_ invocation, and
   delete (or comment out) the `local_path_override`_ invocation.

   .. literalinclude:: ./examples/adventure/MODULE.bazel
      :language: starlark
      :start-after: DOCSTAG: [bazel_dep]
      :end-before: DOCSTAG: [bazel_dep]
      :linenos:

   .. note::

      This step essentially sets up ``app`` to be a completely standalone
      project. Recall that this code example is hosted within the upstream
      Pigweed repository. In that context, it's OK to assume that the
      ``pigweed`` dependency can be found at the local path ``../../..``.
      When using ``app`` as the starting point for your own project, however,
      there's no need to separately download the upstream Pigweed repo to a
      certain path. You can instead use ``git_override`` and let Bazel fetch
      the dependency over the network.

.. _module-pw_kernel-quickstart-build:

-----
Build
-----
1. Build the app for RV32 QEMU:

   .. code-block:: console

      $ bazelisk build //targets/riscv_qemu/simulated:adventure

   See :ref:`docs-install-bazel` if you don't have ``bazelisk`` installed.

   .. dropdown:: Expected output

      .. code-block:: text

         INFO: Analyzed target //targets/riscv_qemu/simulated:adventure
         (0 packages loaded, 0 targets configured).
         INFO: Found 1 target...
         Target //targets/riscv_qemu/simulated:adventure up-to-date:
           bazel-bin/targets/riscv_qemu/simulated/adventure.elf
           bazel-bin/targets/riscv_qemu/simulated/adventure.bin
         INFO: Elapsed time: 21.401s, Critical Path: 20.03s
         INFO: 48 processes: 5 internal, 43 linux-sandbox.
         INFO: Build completed successfully, 48 total actions

.. _module-pw_kernel-quickstart-build-troubleshooting:

Troubleshooting: ``No MODULE.bazel, REPO.bazel, or WORKSPACE file found``
=========================================================================
If you see this error:

.. code-block:: text

   Starting local Bazel server (9.1.0) and connecting to it...
   ERROR: fetching pigweed+: java.io.IOException: No MODULE.bazel, REPO.bazel,
   or WORKSPACE file found in …/external/pigweed+
   ERROR: Error computing the main repository mapping: error during computation
   of main repo mapping: No MODULE.bazel, REPO.bazel, or WORKSPACE file found
   in …/external/pigweed+

You need to modify ``MODULE.bazel`` as explained in
:ref:`module-pw_kernel-quickstart-setup`.

.. _module-pw_kernel-quickstart-simulate:

--------
Simulate
--------
1. Simulate the app on RV32 QEMU:

   .. code-block:: console

      $ bazelisk run @pigweed//pw_kernel/tooling:qemu_runner -- \
          --cpu rv32 \
          --machine virt \
          --semihosting \
          --image $(pwd)/bazel-bin/targets/riscv_qemu/simulated/adventure.elf

   .. dropdown:: Expected output

      .. code-block:: text

         [INF] Initializing PLIC 0xc000000
         [INF] Welcome to Maize on Adventure Example (RISCV QEMU)!
         [INF] Starting monotonic timer
         [INF] Created initial thread; bootstrapping
         [INF] Context switching to first thread
         [INF] Welcome to the first thread, continuing bootstrap
         [INF] Starting thread 'idle' (0x81000090)
         [INF] Allocating non-privileged process 'supervisor'
         [INF] Allocating non-privileged thread 'main_thread'
         [INF] Starting thread 'main_thread' (0x81002d38)
         [INF] Allocating non-privileged process 'uart_driver_mock'
         [INF] Allocating non-privileged thread 'driver_thread'
         [INF] Starting thread 'driver_thread' (0x81002de0)
         [INF] Allocating non-privileged process 'adventure_game'
         [INF] Allocating non-privileged thread 'game_thread'
         [INF] Starting thread 'game_thread' (0x81002d8c)
         [INF] Supervisor started
         [INF] Mock UART Driver started
         [INF] Adventure Game started!
         [INF] Game waiting for input...
         [INF] Game received command: help
         [INF] Game waiting for input...
         [INF] Game received command: look
         [INF] Game waiting for input...
         [INF] Game received command: go north
         [INF] Game waiting for input...
         [INF] Game received command: crash
         [INF] Exception frame 0x81001130:
         [INF] ra  0x800bfbc0 t0 0x00000005 t1  0x800c01f8 t2  0x00000030
         [INF] t3  0x00000008 t4 0x00000000 t5  0x8103fdbc t6  0x00000000
         [INF] a0  0x00000000 a1 0x0000000c a2  0x00000010 a3  0x8103fe18
         [INF] a4  0x00000008 a5 0x00f397da a6  0x00000000 a7  0x00000010
         [INF] tp  0x00000000 gp 0x00000800 sp  0x8103fe40
         [INF] mstatus 0x000000a0
         [INF] mcause 0x00000005
         [INF] mtval 0x00000000
         [INF] epc 0x800bfcba
         [INF] Terminating thread 'game_thread'
         [INF] Exiting thread 'game_thread' (0x81002d8c)
         [ERR] Process 'adventure_game' crashed due to unhandled exception 0
         [WRN] Process 'adventure_game' exited, restarting...
         [INF] Starting thread 'game_thread' (0x81002d8c)
         [INF] Adventure Game started!
         [INF] Game waiting for input...
         [INF] Game received command: help
         [INF] Game waiting for input...
         [INF] Game received command: exit
         [INF] Terminating thread 'game_thread'
         [INF] Exiting thread 'game_thread' (0x81002d8c)
         [INF] Process 'adventure_game' exited cleanly with code 0
         [INF] Supervisor shutting down system due to successful game exit.
         [INF] Shutting down with code 0

----------
Next steps
----------
Check out :ref:`module-pw_kernel-tour` for an in-depth explanation of the
software architecture of the adventure example.

.. _git_override: https://bazel.build/rules/lib/globals/module#git_override
.. _local_path_override: https://bazel.build/rules/lib/globals/module#local_path_override
