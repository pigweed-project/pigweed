.. _module-pw_kernel-build:

======================
Building system images
======================
.. pigweed-module-subpage::
   :name: pw_kernel

``pw_kernel`` firmware is built with Bazel into a single bootable
``system_image`` (an ELF and a flat binary). An image is composed from:

* **The kernel (including optional kernelspace code/apps)**: the kernel
  ``rust_binary``, which compiles the core kernel, architecture port, and any
  kernelspace apps linked directly as ordinary ``rust_library`` targets.
* **Userspace apps (optional)**: ``rust_app`` and ``multi_process_app`` targets
  that supply application metadata to ``system_image``, which compiles and
  merges them into the final image against the system configuration.

Every image must contain a kernel. The image can additionally contain
kernelspace apps, userspace apps, or both. This guide describes the build rules
involved, how userspace apps are compiled, and how ``system_image`` assembles
the final image.

--------------
Build overview
--------------
A ``pw_kernel`` build centers around the following components:

* **System configuration** (``system.json5``): A configuration file defining
  the target architecture, kernel Flash and RAM layout, and optional kernel
  interrupt table. When the image includes userspace apps, it also declares
  applications, processes, threads, IPC objects, wait groups, and userspace
  interrupts.
* **Kernel binary** (``rust_binary``): The executable target for the kernel. It
  links the core kernel, architecture port, optional kernelspace apps in
  ``deps``, the generated kernel linker script (``target_linker_script``), and
  the generated target setup crate (``target_codegen``, whose ``start()``
  launches configured userspace processes).
* **Userspace apps** (``rust_app`` and ``multi_process_app``, optional):
  Targets declaring userspace process sources and dependencies. Rather than
  compiling directly, they supply metadata to ``system_image``, which generates
  the compilation and link actions against the target platform and system
  configuration.
* **System image** (``system_image``): The top-level Bazel macro that ties
  these components together. It transitions the kernel binary to the target
  platform, compiles any userspace ``apps`` against ``system.json5``, and
  packages everything into ``<name>.elf`` and ``<name>.bin``.

.. mermaid::

   flowchart TD
       Config["system.json5 (System Configuration)"] --> KernelCodegen["target_linker_script & target_codegen"]
       KernelLib["Kernel & Kernelspace Apps"] --> KernelElf["Kernel Binary (kernel.elf)"]
       KernelCodegen --> KernelElf

       subgraph Userspace["Userspace Apps (when apps is set)"]
           AppInfo["rust_app / multi_process_app"] --> AppBuild["system_image.apps (App Compiler)"]
           SharedDeps["Shared Libraries (pw_status, libcore, ...)"] --> AppBuild
           AppCodegen["Per-App Codegen & Linker Script"] --> AppBuild
           AppBuild --> AppElfs["App Binaries (app_1.elf, app_2.elf)"]
       end

       Config -.-> AppCodegen
       KernelElf --> Assembler["SystemImageAssembler"]
       AppElfs -.-> Assembler
       Assembler --> FinalElf["System Image ELF (system.elf)"]
       FinalElf --> Objcopy["objcopy"]
       Objcopy --> FinalBin["Bootable Flat Binary (system.bin)"]

--------------------
System configuration
--------------------
Every system image is backed by a ``system.json5`` configuration file:

* **Architecture and kernel layout** (always): Specifies the architecture type
  (for example, ``armv8m`` or ``riscv``), vector table address, kernel Flash and
  RAM regions, and any kernel-handled interrupt table.
* **Applications, processes, and objects** (when userspace apps are included):
  Adds an ``apps`` list declaring each application's Flash allocation, its
  constituent processes and per-process RAM allocations, threads (with kernel
  stack sizes), IPC channel endpoints, wait groups, and userspace interrupt
  objects.

-------------------------------
How userspace apps are compiled
-------------------------------
Userspace application targets declare process sources and dependencies without
compiling directly or binding to specific memory addresses (physical or
virtual) at declaration time. Instead, they supply metadata to
``system_image``, which generates the compilation and link actions against the
target platform and system configuration.

Why apps compile inside system_image
====================================
Userspace applications require two artifacts derived directly from
``system.json5``:

1. **A generated configuration crate** (for example, ``<process>_codegen``):
   Exposes strongly-typed Rust handles and constants for IPC channels, wait
   groups, processes, and interrupts declared in ``system.json5``.
2. **An application linker script** (``<app>.ld``): Places the application's
   code and data at the exact memory addresses (Flash and RAM) assigned by the
   system generator for memory protection.

Because these artifacts depend on the specific system configuration
(``system.json5``), userspace applications cannot be compiled or linked as
standalone binaries in their own packages. Instead, ``system_image`` generates
the compilation and link actions for each included application against that
configuration.

Under the hood, rather than instantiating separate ``rust_binary`` or
``rust_library`` targets for every app in the Bazel package graph, the internal
``<name>.apps`` rule directly invokes ``rules_rust``'s compile action
(``rustc_compile_action``). In terms of the Bazel dependency graph:

* **App targets supply metadata**: Targets defined with ``rust_app`` or
  ``multi_process_app`` declare source files and dependencies via providers
  (``PwAppInfo``) without creating compile actions of their own.
* **Shared dependencies are normal Bazel targets**: Standard library
  dependencies (such as ``libcore``, ``pw_status``, and third-party crates)
  remain ordinary ``rust_library`` targets. Bazel analyzes and builds them
  in the target platform's configuration once per platform, sharing and caching
  the resulting ``.rlib`` files across all apps and system images.
* **App compilation actions belong to** ``<name>.apps``: The internal
  ``<name>.apps`` target depends on the shared libraries, renders the generated
  configuration crates and linker scripts, and registers the ``rustc``
  compilation and linking actions that produce each app ELF.

Compilation pipeline
====================
When a ``system_image`` includes ``apps``, the internal ``<name>.apps`` helper
target executes the following pipeline:

1. **Dependency resolution**: Bazel builds the shared library dependencies
   specified in each app's ``deps`` as standard ``rust_library`` targets in the
   target platform's build configuration.
2. **Per-app code generation**: The system generator reads ``system.json5`` and
   emits each process's configuration crate source and each application's linker
   script (``<app>.ld``).
3. **Codegen compilation**: Each generated configuration crate is compiled into
   a Rust ``.rlib`` using ``rules_rust``'s compile action and passed to the
   process crate via ``--extern``.
4. **App compilation and linking**: Each application crate (or generated
   multi-process entry crate) is compiled and linked with ``rustc`` against its
   pre-compiled shared library dependencies, generated configuration ``.rlib``,
   and generated linker script to produce an app ELF.
5. **System image assembly**: ``SystemImageAssembler`` merges the kernel ELF and
   any application ELFs into a single ``<name>.elf`` image. (When ``apps`` is
   omitted, ``SystemImageAssembler`` packages the kernel ELF directly.)
6. **Flat binary generation**: ``objcopy`` extracts a flat binary
   (``<name>.bin``) ready to be flashed to hardware or loaded into an emulator.

-------------------------------
Defining userspace applications
-------------------------------
Userspace applications are defined in ``BUILD.bazel`` using ``rust_app`` for
single-process applications or ``multi_process_app`` when multiple processes
share a code section.

Single-process applications
===========================
Use ``rust_app`` when an application consists of a single process whose entry
point is marked with ``#[userspace::entry]``:

.. TODO: https://pwbug.dev/584392948 - Update snippet to point to minimal
   working example when available.

.. code-block:: bazel

   load("@pigweed//pw_kernel/tooling:rust_app.bzl", "rust_app")

   rust_app(
       name = "sensor_monitor",
       srcs = ["main.rs"],
       codegen_crate_name = "sensor_monitor_codegen",
       edition = "2024",
       deps = [
           "@pigweed//pw_kernel/syscall:syscall_user",
           "@pigweed//pw_kernel/userspace",
           "@pigweed//pw_log/rust:pw_log",
           "@pigweed//pw_status/rust:pw_status",
       ],
   )

* ``name``: Must match the app and process name declared in ``system.json5`` and
  be a valid non-keyword Rust identifier.
* ``codegen_crate_name``: The crate name under which ``srcs`` import generated
  handles directly (for example, ``use sensor_monitor_codegen::handle;``—no
  ``extern crate`` declaration is required).
* ``deps``: Standard Rust library and Pigweed dependencies, shared across all
  apps on the same platform.

Multi-process applications
==========================
To reduce Flash usage when multiple processes share common library dependencies
(such as ``libcore``, ``userspace``, or logging crates), define each process
with ``rust_process`` and bundle them into a single binary with
``multi_process_app``:

.. code-block:: bazel

   load("@pigweed//pw_kernel/tooling:multi_process_app.bzl", "multi_process_app")
   load("@pigweed//pw_kernel/tooling:rust_app.bzl", "rust_process")

   rust_process(
       name = "uart_server",
       srcs = ["server.rs"],
       codegen_crate_name = "uart_server_codegen",
       edition = "2024",
       deps = [
           "@pigweed//pw_kernel/syscall:syscall_user",
           "@pigweed//pw_kernel/userspace",
       ],
   )

   rust_process(
       name = "uart_client",
       srcs = ["client.rs"],
       codegen_crate_name = "uart_client_codegen",
       edition = "2024",
       deps = [
           "@pigweed//pw_kernel/syscall:syscall_user",
           "@pigweed//pw_kernel/userspace",
       ],
   )

   multi_process_app(
       name = "uart_subsystem",
       processes = [
           ":uart_server",
           ":uart_client",
       ],
   )

``multi_process_app`` links all constituent processes into one shared Flash
image with a generated entry table that dispatches to each process's
``#[userspace::process_entry("<process_name>")]`` function.

Because that one image is shared by several processes, the generated app
linker script rejects global mutable statics (``.data`` and ``.bss``) at link
time; keep per-process mutable state on the process's stack.

-------------------------
Defining the system image
-------------------------
``system_image`` assembles the final firmware image for a target platform.

With userspace apps
===================
When the image includes userspace apps, the kernel binary links
``target_codegen`` and ``target_linker_script``, and ``system_image`` compiles
and bundles ``apps``. Any kernelspace apps can also be added directly to
``kernel_target``'s ``deps`` to run alongside the userspace processes:

.. code-block:: bazel

   load("@rules_rust//rust:defs.bzl", "rust_binary")
   load("@pigweed//pw_kernel/tooling:system_image.bzl", "system_image")
   load("@pigweed//pw_kernel/tooling:target_codegen.bzl", "target_codegen")
   load("@pigweed//pw_kernel/tooling:target_linker_script.bzl", "target_linker_script")

   filegroup(
       name = "system_config",
       srcs = ["system.json5"],
   )

   target_codegen(
       name = "codegen",
       arch = "@pigweed//pw_kernel/arch/arm_cortex_m:arch_arm_cortex_m",
       system_config = ":system_config",
   )

   target_linker_script(
       name = "linker_script",
       system_config = ":system_config",
       template = "//my_project/target:linker_script_template",
   )

   rust_binary(
       name = "kernel_target",
       srcs = ["target.rs"],
       edition = "2024",
       deps = [
           ":codegen",
           ":linker_script",
           "//my_project/kernel_app",  # Optional kernelspace app.
           "@pigweed//pw_kernel/arch/arm_cortex_m:arch_arm_cortex_m",
           "@pigweed//pw_kernel/kernel",
           "@pigweed//pw_kernel/target:target_common",
           "@pigweed//pw_kernel/userspace",
       ],
   )

   system_image(
       name = "my_system",
       apps = [
           "//my_project/apps:sensor_monitor",
           "//my_project/apps:uart_subsystem",
       ],
       kernel = ":kernel_target",
       platform = "//my_project/target:my_platform",
       system_config = ":system_config",
   )

Kernel-only system image
========================
When an image runs only kernelspace apps and needs no userspace processes, omit
``apps`` and set ``userspace = False`` to compile out userspace support
(``target_codegen`` is only needed if ``system.json5`` configures a kernel
interrupt table):

.. code-block:: bazel

   load("@rules_rust//rust:defs.bzl", "rust_binary")
   load("@pigweed//pw_kernel/tooling:system_image.bzl", "system_image")
   load("@pigweed//pw_kernel/tooling:target_linker_script.bzl", "target_linker_script")

   filegroup(
       name = "system_config",
       srcs = ["system.json5"],
   )

   target_linker_script(
       name = "linker_script",
       system_config = ":system_config",
       template = "//my_project/target:linker_script_template",
   )

   rust_binary(
       name = "kernel_target",
       srcs = ["target.rs"],
       edition = "2024",
       deps = [
           ":linker_script",
           "//my_project/kernel_app",
           "@pigweed//pw_kernel/arch/arm_cortex_m:arch_arm_cortex_m",
           "@pigweed//pw_kernel/kernel",
           "@pigweed//pw_kernel/target:target_common",
       ],
   )

   system_image(
       name = "my_kernel_only_system",
       kernel = ":kernel_target",
       platform = "//my_project/target:my_platform",
       system_config = ":system_config",
       userspace = False,
   )

Building either ``system_image`` target produces two artifacts:

* ``bazel-bin/path/to/<name>.elf``: Bootable ELF image.
* ``bazel-bin/path/to/<name>.bin``: Flat binary image ready for flashing.

-------------------
Testing and running
-------------------
To run automated tests against a ``system_image`` target, use
``system_image_test``:

.. code-block:: bazel

   load("@pigweed//pw_kernel/tooling:system_image.bzl", "system_image_test")

   system_image_test(
       name = "my_system_test",
       image = ":my_system",
   )

This exposes the image as a Bazel test target that can be executed under QEMU or
dispatched to an on-device test runner via ``bazelisk test``.
