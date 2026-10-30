.. _docs-module-guides:

========
Platform
========
.. toctree::
   :maxdepth: 1
   :hidden:

   Overview <self>

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

--------
Overview
--------
.. grid:: 3

   .. grid-item-card:: :octicon:`broadcast` Comms
      :link: comms/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Remote procedure calls (RPC), channels, and communication streams

   .. grid-item-card:: :octicon:`eye` Observability
      :link: observability/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Structured logging, tokenization, metrics, crash handling, and tracing

   .. grid-item-card:: :octicon:`sync` Async & concurrency
      :link: async/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Cooperative multitasking, futures, and asynchronous primitives

.. grid:: 3

   .. grid-item-card:: :octicon:`file-binary` Encoding & serialization
      :link: encoding/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Protocol Buffers, JSON, Varint, Base64, and wire format serialization

   .. grid-item-card:: :octicon:`cpu` OS abstraction layers
      :link: os/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Synchronization, threading, and portable RTOS abstractions

   .. grid-item-card:: :octicon:`server` Kernel
      :link: kernel/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Embedded kernels, preemptive scheduling, and userspace isolation

.. grid:: 3

   .. grid-item-card:: :octicon:`database` Memory & storage
      :link: memory/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Memory allocators, buffers, persistent flash storage, and key-value stores

   .. grid-item-card:: :octicon:`plug` HALs
      :link: hal/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Hardware abstraction layer interfaces for timers, I2C, SPI, UART, and GPIO

   .. grid-item-card:: :octicon:`package-dependencies` Hardware support
      :link: hw/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Board support, hardware drivers, and SDK integrations for target platforms

.. grid:: 3

   .. grid-item-card:: :octicon:`code` Data structures & utilities
      :link: core/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Embedded-safe collections, spans, strings, status, and error handling

   .. grid-item-card:: :octicon:`checklist` Testing
      :link: test/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Unit testing frameworks, mocking, compilation tests, and fuzzing

   .. grid-item-card:: :octicon:`workflow` Build systems
      :link: build/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Bazel, GN, and CMake integrations and rules

.. grid:: 3

   .. grid-item-card:: :octicon:`gear` Toolchains
      :link: toolchains/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Hermetic LLVM/Clang toolchains, C/C++ libraries, and build utilities

   .. grid-item-card:: :octicon:`command-palette` Developer tools
      :link: devtools/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Interactive REPL, CLI utilities, code intelligence, and emulators

   .. grid-item-card:: :octicon:`shield-lock` Cryptography & security
      :link: crypto/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Hashing, cryptographic primitives, and secure storage

-----
Comms
-----
Remote procedure calls (RPC), channels, and communication streams

Learn more: :doc:`comms/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_rpc2`", ":bdg-warning:`experimental`", "Next gen, zero-copy async RPC with end-to-end backpressure", "C++"
   ":ref:`module-pw_transport`", ":bdg-warning:`experimental`", "Asynchronous datagram-oriented transport interface", "C++"
   ":ref:`module-pw_channel`", ":bdg-danger:`deprecated`", "Async, zero-copy API for sending and receiving bytes or datagrams", "C++"
   ":ref:`module-pw_cobs`", ":bdg-warning:`experimental`", "Consistent Overhead Byte Stuffing (COBS) codec and zero-copy frame streamer", "C++"
   ":ref:`module-pw_grpc`", ":bdg-secondary:`unstable`", "pw_rpc over gRPC", "C++"
   ":ref:`module-pw_hdlc`", ":bdg-primary:`stable`", "Simple, robust, and efficient serial communication", "Python, C++, TypeScript"
   ":ref:`module-pw_router`", ":bdg-warning:`experimental`", "Transport-agnostic packet routing", "C++"
   ":ref:`module-pw_rpc`", ":bdg-primary:`stable`", "Efficient, low-code-size RPC system for embedded devices", "C++, Java, JavaScript, Python, TypeScript"
   ":ref:`module-pw_rpc_transport`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_stream`", ":bdg-primary:`stable`", "A foundational interface for streaming data", "C++, Rust"
   ":ref:`module-pw_transfer`", ":bdg-primary:`stable`", "", ""

Bluetooth
=========
Dual-mode Bluetooth Host stack, HCI controllers, and profiles

Learn more: :doc:`bluetooth/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_bluetooth`", ":bdg-secondary:`unstable`", "Host-layer Bluetooth Low Energy APIs and utilities", "C++"
   ":ref:`module-pw_bluetooth_hci`", ":bdg-primary:`stable`", "Bluetooth Host Controller Interface utilities", "C++"
   ":ref:`module-pw_bluetooth_profiles`", ":bdg-secondary:`unstable`", "Bluetooth profiles built on top of pw_bluetooth", "C++"
   ":ref:`module-pw_bluetooth_proxy`", ":bdg-secondary:`unstable`", "Lightweight proxy for augmenting Bluetooth functionality", "C++"
   ":ref:`module-pw_bluetooth_sapphire`", ":bdg-secondary:`unstable`", "Battle-tested Bluetooth with rock-solid reliability", "C++"

-------------
Observability
-------------
Structured logging, tokenization, metrics, crash handling, and tracing

Learn more: :doc:`observability/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_log`", ":bdg-primary:`stable`", "Robust and portable logging for embedded projects", "C++, Java, Python, Rust"
   ":ref:`module-pw_tokenizer`", ":bdg-primary:`stable`", "Compress strings to shrink logs by +75%", "C, C++, Python, Rust, TypeScript, Java"
   ":ref:`module-pw_assert`", ":bdg-primary:`stable`", "Flexible crash triggering and handling", "C, C++, Rust"
   ":ref:`module-pw_console`", ":bdg-primary:`stable`", "Multi-purpose pluggable interactive console for dev & manufacturing", "Python"
   ":ref:`module-pw_cli_analytics`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_cpu_exception`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_format`", ":bdg-warning:`experimental`", "String formatting", "Rust"
   ":ref:`module-pw_hex_dump`", ":bdg-primary:`stable`", "Handy hexdump utilities", "C++"
   ":ref:`module-pw_metric`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_multisink`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_persistent_ram`", ":bdg-primary:`stable`", "Reliable memory across reboots for crash reporting", "C++"
   ":ref:`module-pw_snapshot`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_symbolizer`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_trace`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_web`", ":bdg-primary:`stable`", "", ""

-------------------
Async & concurrency
-------------------
Cooperative multitasking, futures, and asynchronous primitives

Learn more: :doc:`async/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_async2`", ":bdg-primary:`stable`", "Cooperative async tasks for embedded", "C++"
   ":ref:`module-pw_atomic`", ":bdg-warning:`experimental`", "Software implementations of atomic operations", "C, C++"
   ":ref:`module-pw_work_queue`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_async`", ":bdg-danger:`deprecated`", "Portable APIs for asynchronous code", "C++"

------------------------
Encoding & serialization
------------------------
Protocol Buffers, JSON, Varint, Base64, and wire format serialization

Learn more: :doc:`encoding/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_protobuf`", ":bdg-primary:`stable`", "Expressive interface for encoding and decoding protocol buffers", "C++"
   ":ref:`module-pw_base64`", ":bdg-primary:`stable`", "Base64 encoding, decoding, and validating", "C, C++, Rust"
   ":ref:`module-pw_bytes`", ":bdg-primary:`stable`", "Utilities for manipulating binary data", "C++, Rust"
   ":ref:`module-pw_checksum`", ":bdg-primary:`stable`", "Checksum calculation API", "C, C++"
   ":ref:`module-pw_flatbuffers`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_json`", ":bdg-primary:`stable`", "Simple, efficient C++ JSON serialization", "C++"
   ":ref:`module-pw_protobuf_compiler`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_uuid`", ":bdg-warning:`experimental`", "128-bit universally unique identifier (UUID)", ""
   ":ref:`module-pw_varint`", ":bdg-primary:`stable`", "Functions for encoding and decoding variable length integers", "C, C++, Rust"

---------------------
OS abstraction layers
---------------------
Synchronization, threading, and portable RTOS abstractions

Learn more: :doc:`os/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sync`", ":bdg-primary:`stable`", "Portable abstraction for synchronization primitives", ""
   ":ref:`module-pw_thread`", ":bdg-primary:`stable`", "Thread creation and execution", "C, C++, Rust"
   ":ref:`module-pw_chrono`", ":bdg-primary:`stable`", "Portable std::chrono for constrained embedded devices", "C, C++"
   ":ref:`module-pw_interrupt`", ":bdg-secondary:`unstable`", "Interrupt handling utilities for embedded systems", ""
   ":ref:`module-pw_boot`", ":bdg-primary:`stable`", "Simplified booting for C++ targets", "C++"
   ":ref:`module-pw_system`", ":bdg-secondary:`unstable`", "", ""

Backends
========
Operating system, RTOS, and bare-metal backends for Pigweed's OS abstraction layers

Learn more: :doc:`os/backends`

Zephyr
------
Modules, backends, and helpers for the Zephyr RTOS

Learn more: :doc:`os/zephyr/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_allocator_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_assert_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_chrono_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_digital_io_zephyr`", ":bdg-warning:`experimental`", "", "C++"
   ":ref:`module-pw_env_setup_zephyr`", ":bdg-warning:`experimental`", "Zephyr utilities", ""
   ":ref:`module-pw_i2c_zephyr`", ":bdg-warning:`experimental`", "Zephyr implementation for pw_i2c", ""
   ":ref:`module-pw_interrupt_zephyr`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_log_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_spi_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_zephyr`", ":bdg-primary:`stable`", "", ""

Amazon FreeRTOS
---------------
Safe, non-overflowing OS abstractions & wrappers for FreeRTOS

Learn more: :doc:`os/freertos/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_interrupt_freertos`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_malloc_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_freertos`", ":bdg-primary:`stable`", "", ""

Eclipse ThreadX
---------------
Thread-safe OS abstraction layers & backends for Azure RTOS ThreadX

Learn more: :doc:`os/threadx/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_threadx`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_threadx`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_threadx`", ":bdg-primary:`stable`", "", ""

Segger embOS
------------
RTOS abstraction layers & thread-safety integrations for SEGGER embOS

Learn more: :doc:`os/embos/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_embos`", ":bdg-primary:`stable`", "", ""

STL
---
C++ Standard Template Library backends for host and POSIX targets

Learn more: :doc:`os/stl/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_stl`", ":bdg-primary:`stable`", "", ""

Bare metal
----------
Backends for running directly on target hardware without an OS or scheduler

Learn more: :doc:`os/baremetal/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sync_baremetal`", ":bdg-primary:`stable`", "", ""

Android
-------
Utilities and toolchains for using Pigweed on Android

Learn more: :doc:`targets/android/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_android_toolchain`", ":bdg-secondary:`unstable`", "GN toolchains for the Android NDK", "GN"
   ":ref:`module-pw_build_android`", ":bdg-primary:`stable`", "Utilities for using Pigweed in Android platform", "C++"
   ":ref:`module-pw_chre`", ":bdg-warning:`experimental`", "Android Context Hub Runtime Environment backend", "C++"
   ":ref:`module-pw_log_android`", ":bdg-primary:`stable`", "Android logging backend", "C++"

Fuchsia
-------
Backends and platform integrations for Fuchsia OS

Learn more: :doc:`targets/fuchsia/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_assert_fuchsia`", ":bdg-warning:`experimental`", "pw_assert backend for Fuchsia", "C, C++"
   ":ref:`module-pw_async_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia backend for pw_async", "C, C++"
   ":ref:`module-pw_log_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia logging backend", "C, C++"
   ":ref:`module-pw_random_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia implementation of pw_random", ""

------
Kernel
------
Embedded kernels, preemptive scheduling, and userspace isolation

Learn more: :doc:`kernel/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_kernel`", ":bdg-warning:`experimental`", "A Rust-based kernel for embedded systems", "Rust"

----------------
Memory & storage
----------------
Memory allocators, buffers, persistent flash storage, and key-value stores

Learn more: :doc:`memory/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_allocator`", ":bdg-primary:`stable`", "Flexible, safe, and measurable memory allocation", "C++"
   ":ref:`module-pw_buf`", ":bdg-secondary:`unstable`", "Contiguous buffer views with optional memory ownership", "C++"
   ":ref:`module-pw_kvs`", ":bdg-primary:`stable`", "Lightweight, persistent key-value store", "C++"
   ":ref:`module-pw_alignment`", ":bdg-primary:`stable`", "Natural object alignment, guaranteed", "C++"
   ":ref:`module-pw_blob_store`", ":bdg-primary:`stable`", "Flash-backed persistent storage for data blobs", "C++"
   ":ref:`module-pw_file`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_intrusive_ptr`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_malloc`", ":bdg-primary:`stable`", "Replacement interface for standard libc dynamic memory operations", "C, C++"
   ":ref:`module-pw_memory`", ":bdg-primary:`stable`", "Memory utilities", "C++"
   ":ref:`module-pw_multibuf`", ":bdg-secondary:`unstable`", "A buffer API optimized for zero-copy messaging", "C++"
   ":ref:`module-pw_ring_buffer`", ":bdg-primary:`stable`", "", ""

----
HALs
----
Hardware abstraction layer interfaces for timers, I2C, SPI, UART, and GPIO

Learn more: :doc:`hal/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_analog`", ":bdg-primary:`stable`", "Analog-to-digital converter libraries and utilities", "C++"
   ":ref:`module-pw_clock_tree`", ":bdg-secondary:`unstable`", "Clock tree management for embedded devices", "C++"
   ":ref:`module-pw_digital_io`", ":bdg-primary:`stable`", "Digital I/O interface", "C++"
   ":ref:`module-pw_display`", ":bdg-warning:`experimental`", "Graphic display support and framebuffer management", ""
   ":ref:`module-pw_hal`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_i2c`", ":bdg-primary:`stable`", "Cross-platform I2C API with interactive debugging", "C++"
   ":ref:`module-pw_sensor`", ":bdg-warning:`experimental`", "A modular way to see the world", "Python, C++"
   ":ref:`module-pw_spi`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_time`", ":bdg-warning:`experimental`", "Robust time, instants, and durations for multiple clock domains", "Rust"
   ":ref:`module-pw_uart`", ":bdg-secondary:`unstable`", "Core interfaces for UART communication", "C++"

----------------
Hardware support
----------------
Board support, hardware drivers, and SDK integrations for target platforms

Learn more: :doc:`hw/index`

Raspberry Pi RP2XXX
===================
Hardware drivers & platform support for Raspberry Pi RP2040 & Pico SDK

Learn more: :doc:`targets/rp2040/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_rp2040`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_digital_io_rp2040`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_i2c_rp2040`", ":bdg-primary:`stable`", "Raspberry Pi Pico SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_spi_rp2040`", ":bdg-secondary:`unstable`", "Raspberry Pi RP2040 backend for pw_spi", "C++"
   ":ref:`module-pw_sys_io_rp2040`", ":bdg-primary:`stable`", "", ""

NXP
===
Drivers, build utilities, & HAL implementations for NXP MCUXpresso SDK

Learn more: :doc:`targets/mimxrt595_evk/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_build_mcuxpresso`", ":bdg-primary:`stable`", "Utilities for building NXP MCUXpresso SDK targets", "Bazel, GN"
   ":ref:`module-pw_clock_tree_mcuxpresso`", ":bdg-secondary:`unstable`", "NXP MCUXpresso SDK implementation for pw_clock_tree", "C++"
   ":ref:`module-pw_digital_io_mcuxpresso`", ":bdg-primary:`stable`", "Digital I/O for NXP MCUXpresso", "C++"
   ":ref:`module-pw_dma_mcuxpresso`", ":bdg-secondary:`unstable`", "DMA helpers for MCUXpresso SDK", "C++"
   ":ref:`module-pw_i2c_mcuxpresso`", ":bdg-primary:`stable`", "NXP MCUXpresso SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_spi_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_stream_uart_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_uart_mcuxpresso`", ":bdg-secondary:`unstable`", "NXP MCUXpresso SDK implementation for pw_uart", "C++"

Ambiq
=====
Hardware drivers & platform support for Ambiq Apollo4 & AmbiqSuite SDK

Learn more: :doc:`targets/apollo4/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_ambiq_sdk`", ":bdg-primary:`stable`", "", ""

Arduino
=======
Build utilities, cores, & hardware support for Arduino

Learn more: :doc:`targets/arduino/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_arduino_build`", ":bdg-primary:`stable`", "Main function wrapper and build utilities for Arduino", "C++, GN"
   ":ref:`module-pw_sys_io_arduino`", ":bdg-primary:`stable`", "", ""

Emcraft
=======
Hardware support & HAL integrations for Emcraft SmartFusion2

Learn more: :doc:`targets/emcraft_sf2_som/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_emcraft_sf2`", ":bdg-primary:`stable`", "", ""

STMicroelectronics STM32FXXX
============================
Build support & hardware drivers for STMicroelectronics STM32Cube

Learn more: :doc:`targets/stm32f429i_disc1/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_stm32cube_build`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_baremetal_stm32f429`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_stm32cube`", ":bdg-primary:`stable`", "", ""

Texas Instruments
=================
Hardware & emulation support for Texas Instruments microcontrollers

Learn more: :doc:`targets/lm3s6965evb_qemu/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_baremetal_lm3s6965evb`", ":bdg-primary:`stable`", "", ""

---------------------------
Data structures & utilities
---------------------------
Embedded-safe collections, spans, strings, status, and error handling

Learn more: :doc:`core/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_status`", ":bdg-primary:`stable`", "Exception-free error propagation for embedded", "C++, C, Python, Java, TypeScript, Rust"
   ":ref:`module-pw_result`", ":bdg-primary:`stable`", "Error propagation primitives: value-or-error", "C++"
   ":ref:`module-pw_containers`", ":bdg-primary:`stable`", "Generic collections of objects for embedded devices", "C++"
   ":ref:`module-pw_function`", ":bdg-primary:`stable`", "Embedded-friendly std::function", "C++"
   ":ref:`module-pw_enum`", ":bdg-secondary:`unstable`", "Rich enum support", "C++"
   ":ref:`module-pw_numeric`", ":bdg-primary:`stable`", "Efficient mathematical utilities for embedded", "C++"
   ":ref:`module-pw_span`", ":bdg-primary:`stable`", "std::span for C++17", "C++"
   ":ref:`module-pw_string`", ":bdg-primary:`stable`", "Efficient, easy, and safe string manipulation", "C++"

-------
Testing
-------
Unit testing frameworks, mocking, compilation tests, and fuzzing

Learn more: :doc:`test/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_unit_test`", ":bdg-primary:`stable`", "GoogleTest for embedded", ""
   ":ref:`module-pw_compilation_testing`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_daq`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_fuzzer`", ":bdg-secondary:`unstable`", "Better C++ code through easier fuzzing", "C++"
   ":ref:`module-pw_hil`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_perf_test`", ":bdg-secondary:`unstable`", "Micro-benchmarks that are easy to write and run", "C++"
   ":ref:`module-pw_target_runner`", ":bdg-primary:`stable`", "gRPC server for running executables in parallel", "Go"

-------------
Build systems
-------------
Bazel, GN, and CMake integrations and rules

Learn more: :doc:`build/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_build`", ":bdg-primary:`stable`", "Integrations for Bazel, GN, and CMake", "Bazel, CMake, GN, Rust"
   ":ref:`module-pw_build_info`", ":bdg-primary:`stable`", "Generate, embed, and parse build-related information", "Bazel, CMake, GN"
   ":ref:`module-pw_codegen`", ":bdg-secondary:`unstable`", "Source code generation utilities", "Python"
   ":ref:`module-pw_rust`", ":bdg-warning:`experimental`", "", ""

----------
Toolchains
----------
Hermetic LLVM/Clang toolchains, C/C++ libraries, and build utilities

Learn more: :doc:`toolchains/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_bloat`", ":bdg-primary:`stable`", "Utilities for generating binary size reports", "C++, GN"
   ":ref:`module-pw_elf`", ":bdg-warning:`experimental`", "ELF file support", "C++"
   ":ref:`module-pw_libc`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_libcxx`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_polyfill`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_preprocessor`", ":bdg-primary:`stable`", "Helpful preprocessor macros", "C, C++"
   ":ref:`module-pw_toolchain`", ":bdg-primary:`stable`", "Embedded toolchains for GN-based Pigweed projects", "GN"

---------------
Developer tools
---------------
Interactive REPL, CLI utilities, code intelligence, and emulators

Learn more: :doc:`devtools/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_ide`", ":bdg-primary:`stable`", "Code editor and IDE support for Pigweed projects", "C++, CLI, Python, VS Code"
   ":ref:`module-pw_env_setup`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_module`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_presubmit`", ":bdg-primary:`stable`", "Python tools for running presubmit checks and linters", "Python"
   ":ref:`module-pw_watch`", ":bdg-primary:`stable`", "Embedded development file system watcher", ""
   ":ref:`module-docker`", ":bdg-warning:`experimental`", "Docker images for caching Pigweed environments", ""
   ":ref:`module-pw_change`", ":bdg-warning:`experimental`", "Commands that operate on Gerrit changes and Git commits", "Python"
   ":ref:`module-pw_cli`", ":bdg-primary:`stable`", "Enhance and accelerate custom command-line tooling", "Python"
   ":ref:`module-pw_config_loader`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_docgen`", ":bdg-primary:`stable`", "Documentation generator for Pigweed-based projects", "GN"
   ":ref:`module-pw_doctor`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_emu`", ":bdg-warning:`experimental`", "Flexible emulators frontend", "Python, CLI"
   ":ref:`module-pw_ghish`", ":bdg-warning:`experimental`", "GitHub CLI-like interface for Gerrit code reviews and CI checks", "Go"
   ":ref:`module-pw_package`", ":bdg-primary:`stable`", "", ""

-----------------------
Cryptography & security
-----------------------
Hashing, cryptographic primitives, and secure storage

Learn more: :doc:`crypto/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_crypto`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_fortifier`", ":bdg-warning:`experimental`", "Tools and frameworks for defensive development", "Python"
   ":ref:`module-pw_random`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_software_update`", ":bdg-warning:`experimental`", "Secure software delivery", "Python, C++"
   ":ref:`module-pw_tls_client`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-category pages to this
.. page's toctree, add them to the hand-written toctree above the
.. categories-start block.
.. toctree::
   :maxdepth: 1
   :hidden:

   comms/index
   observability/index
   async/index
   encoding/index
   os/index
   kernel/index
   memory/index
   hal/index
   hw/index
   core/index
   test/index
   build/index
   toolchains/index
   devtools/index
   crypto/index

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

.. toctree::
   :maxdepth: 1
   :hidden:

   Module index <modules>
   Module structure <module_structure>
   facades
