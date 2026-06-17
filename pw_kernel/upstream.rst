.. _module-pw_kernel-upstream:

============================
Upstream contributor's guide
============================
.. pigweed-module-subpage::
   :name: pw_kernel

This guide is for :ref:`docs-glossary-upstream` contributors working on
``pw_kernel``. If you're interested in using ``pw_kernel`` in a downstream
project, see :ref:`module-pw_kernel-quickstart`.

-------------
Prerequisites
-------------
This guide assumes you're already set up for upstream development. If not,
see :ref:`docs-contributing`.

.. _module-pw_kernel-upstream-config:

--------------
Configurations
--------------
``pw_kernel`` provides several Bazel configurations for different targets and
build types:

- ``k_host``: For building and running on your host machine (Linux, macOS).
- ``k_qemu_mps2_an505``: For QEMU emulating an Arm Cortex-M33 based system (MPS2-AN505).
- ``k_qemu_virt_riscv32``: For QEMU emulating a RISC-V 32-bit based system.
- ``k_rp2350``: For the Raspberry Pi RP2350 microcontroller.

In subsequent sections the placeholder ``<config>`` should be replaced with one
of these values.

-------------
VS Code setup
-------------
.. _rust-analyzer: https://rust-analyzer.github.io/

For the best Rust development experience, especially with VS Code, we recommend
`rust-analyzer`_.

``rust-analyzer`` needs a ``rust-project.json`` file at the root of your workspace
to understand the project structure, dependencies, and build configurations.
You can generate this file using Bazel.

For a given configuration:

.. code-block:: console

   bazelisk run @rules_rust//tools/rust_analyzer:gen_rust_project \
   -- --config <config> //pw_kernel/...

This command creates or updates the ``rust-project.json`` file in your Pigweed
project root.

To enable ``rust-analyzer`` to provide real-time feedback (errors and warnings)
in VS Code based on your Bazel build configuration, add the following to your
Pigweed project's ``.vscode/settings.json`` file.

.. code-block:: json

   "rust-analyzer.check.overrideCommand": [
     "bazelisk",
     "build",
     "--config=k_lint",
     "--config=$CONFIG",
     "--@rules_rust//:error_format=json",
     "--experimental_ui_max_stdouterr_bytes=10485760",
     "//pw_kernel/..."
   ]

-----
Build
-----
To build ``pw_kernel`` for a given configuration:

.. code-block:: console

   bazelisk build --config <config> //pw_kernel/...

----
Test
----
To run all ``pw_kernel`` tests for a given configuration:

.. code-block:: console

   bazelisk test --config <config> //pw_kernel/...

To run unit tests tests for the RISC-V QEMU target and see all test output:

.. code-block:: console

   bazelisk test --test_output=all --cache_test_results=no \
   --config k_qemu_virt_riscv32 \
   //pw_kernel/target/qemu_virt_riscv32/unittest_runner

The test runner executes all discovered tests and reports their status.
Bare-metal tests are run first, followed by kernel-aware tests if the kernel
is initialized.

Raspberry Pi RP2350
===================
.. _Installation: https://probe.rs/docs/getting-started/installation/

You can run the tests on a physical RP2350-based board.

1. Build a test:

   .. code-block:: console

      bazelisk build --config k_rp2350 \
      //pw_kernel/target/pw_rp2350/ipc/user:ipc

2. Confirm that the ELF file was output to
   ``//bazel-bin/pw_kernel/target/pw_rp2350/ipc/user/``.

3. Flash a test:

   .. code-block:: console

      probe-rs download bazel-bin/pw_kernel/target/pw_rp2350/ipc/user/ipc.elf \
      && probe-rs reset --chip rp2350

   See `Installation`_ if you don't have ``probe-rs`` installed.

4. Run a test:

   .. code-block:: console

      bazelisk run --config k_rp2350 //pw_kernel/target/pw_rp2350/ipc/user:ipc \
      -- -d <PORT>

   The placeholder ``<PORT>`` should be replaced with the actual serial port,
   e.g. ``/dev/ttyACM0`` on Linux or ``/dev/cu.usbmodemXXXXXX`` on macOS.

   This command will first build the firmware if necessary and then connect to
   the device using :ref:`pw_tokenizer.serial_detokenizer
   <module-pw_tokenizer-cli-detokenizing>` to display human-readable logs.

.. _module-pw_kernel-upstream-conventions:

-----------
Conventions
-----------
This section outlines the conventions used in ``pw_kernel`` to ensure
consistency and maintainability.

Logging and error handling
==========================
Consistent logging is crucial for debugging and maintaining ``pw_kernel``.

General logging style
---------------------
- **Capitalization**: Log messages should be capitalized as sentences, unless
  they begin with a specific identifier or symbol that is lowercase by
  definition (e.g., ``thread_name``).
- **Punctuation**: Do not end log messages with a trailing period. *Exception*:
  If a log message contains multiple full sentences, use periods for the
  intermediate sentences, but omit it for the final one.
- **Clarity**: Prefer clear, descriptive messages over cryptic abbreviations.

Register and value dumping
--------------------------
- **Format**: Use ``Key=Value`` format for technical dumps, separated by commas
  if on the same line. *Example*: ``Exception: cause={:#x}, epc={:#x}``
- **Hexadecimal**: Use standard Rust hex formatting ``{:#x}`` for values. For
  full 32-bit addresses, use ``{:#010x}`` to include the ``0x`` prefix and
  leading zeros.
- **Threads**: When logging thread information, use the format ``'thread_name'
  ({:#010x})``, where the name is in single quotes and the ID is in parentheses
  and hex format. *Example*: ``Context switch to thread 'idle' (0x20001234)``

Optional debug logging
----------------------
High-frequency or verbose logs that are useful for specific debugging sessions
but too noisy for general use should be gated by a ``const bool`` flag using
the ``log_if`` crate.

- **Flags**: Define a ``const bool`` flag at the top of the file (e.g., ``const
  LOG_SCHEDULER_EVENTS: bool = false;``).
- **Default State**: These flags must be committed as ``false`` by default.
- **Usage**: Use ``log_if::info_if!`` or ``log_if::debug_if!`` with the flag.
  *Example*: ``log_if::info_if!(LOG_CONTEXT_SWITCH, "Context switch to thread
  '{}' ({:#x})", ...)``

Panics
------
- **Macro**: Always use ``pw_assert::panic!`` instead of the standard library
  ``panic!``.
- **Style**: Follow the same capitalization and punctuation rules as general
  logs (capitalized, no trailing period).
- **Context**: Provide as much context as reasonable in the panic message.

  - *Bad*: ``pw_assert::panic!("Run queue empty")``
  - *Good*: ``pw_assert::panic!("Run queue empty: no runnable threads (idle
    thread missing?)")``

- **Unimplemented**: For unimplemented features, use
  ``pw_assert::panic!("Unimplemented: <feature_name>")`` instead of ``todo!()``
  or generic "Unimplemented" messages.
