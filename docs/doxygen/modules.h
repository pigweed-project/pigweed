// Copyright 2025 The Pigweed Authors
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
#pragma once

// TODO: b/426012010 - Auto-generate this file from the metadata provided
// in //docs/common/metadata/categories.toml and //<module>/module.toml.

// TODO: b/441319784 - Move each module's content into that module's directory.
// E.g. the pw_allocator content should go into //pw_allocator/doxygen.h

// clang-format off

// Top-level category (`category_<id>`) and module (`pw_<name>`) groups are
// auto-generated between `categories-start` and `categories-end` from
// //docs/common/metadata/categories.toml and //<module>/module.toml.
// Lower-level submodule groups (such as `pw_allocator_block`) after
// `categories-end` are maintained manually and are OK to edit directly.

// categories-start
// +----------------------------------------------------------+
// | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
// |                                                          |
// | To regenerate this section, run:                         |
// |   bazelisk run //docs/common/metadata:sync               |
// |                                                          |
// | Docs: https://pigweed.dev/contributing/docs/website.html |
// +----------------------------------------------------------+
/// @defgroup category_async Async & concurrency
/// @brief Cooperative multitasking, futures, and asynchronous primitives
/// @defgroup category_build Build systems
/// @brief Bazel, GN, and CMake integrations and rules
/// @defgroup category_comms Comms
/// @brief Remote procedure calls (RPC), channels, and communication streams
/// @defgroup category_core Data structures & utilities
/// @brief Embedded-safe collections, spans, strings, status, and error handling
/// @defgroup category_crypto Cryptography & security
/// @brief Hashing, cryptographic primitives, and secure storage
/// @defgroup category_encoding Encoding & serialization
/// @brief Protocol Buffers, JSON, Varint, Base64, and wire format serialization
/// @defgroup category_hal HALs
/// @brief Hardware abstraction layer interfaces for timers, I2C, SPI, UART, and GPIO
/// @defgroup category_hw Hardware support
/// @brief Board support, hardware drivers, and SDK integrations for target platforms
/// @defgroup category_memory Memory & storage
/// @brief Memory allocators, buffers, persistent flash storage, and key-value stores
/// @defgroup category_observability Observability
/// @brief Structured logging, tokenization, metrics, crash handling, and tracing
/// @defgroup category_os OS abstraction layers
/// @brief Synchronization, threading, and portable RTOS abstractions
/// @defgroup category_test Testing
/// @brief Unit testing frameworks, mocking, compilation tests, and fuzzing
/// @defgroup category_toolchain Toolchains
/// @brief Hermetic LLVM/Clang toolchains, C/C++ libraries, and build utilities
/// @defgroup category_bluetooth Bluetooth
/// @ingroup category_comms
/// @brief Dual-mode Bluetooth Host stack, HCI controllers, and profiles
/// @defgroup category_nxp NXP
/// @ingroup category_hw
/// @brief Drivers, build utilities, & HAL implementations for NXP MCUXpresso SDK
/// @defgroup category_os_backends Backends
/// @ingroup category_os
/// @brief Operating system, RTOS, and bare-metal backends for Pigweed's OS abstraction layers
/// @defgroup category_android Android
/// @ingroup category_os_backends
/// @brief Utilities and toolchains for using Pigweed on Android
/// @defgroup category_zephyr Zephyr
/// @ingroup category_os_backends
/// @brief Modules, backends, and helpers for the Zephyr RTOS
/// @defgroup pw_alignment pw_alignment
/// @ingroup category_memory
/// @brief Natural object alignment, guaranteed
/// @defgroup pw_allocator pw_allocator
/// @ingroup category_memory
/// @brief Flexible, safe, and measurable memory allocation
/// @defgroup pw_analog pw_analog
/// @ingroup category_hal
/// @brief Analog-to-digital converter libraries and utilities
/// @defgroup pw_assert pw_assert
/// @ingroup category_observability
/// @brief Flexible crash triggering and handling
/// @defgroup pw_async pw_async
/// @ingroup category_async
/// @brief Portable APIs for asynchronous code
/// @defgroup pw_async2 pw_async2
/// @ingroup category_async
/// @brief Cooperative async tasks for embedded
/// @defgroup pw_async_basic pw_async_basic
/// @ingroup pw_async
/// @defgroup pw_base64 pw_base64
/// @ingroup category_encoding
/// @brief Base64 encoding, decoding, and validating
/// @defgroup pw_bloat pw_bloat
/// @ingroup category_toolchain
/// @brief Utilities for generating binary size reports
/// @defgroup pw_bluetooth pw_bluetooth
/// @ingroup category_bluetooth
/// @brief Host-layer Bluetooth Low Energy APIs and utilities
/// @defgroup pw_bluetooth_proxy pw_bluetooth_proxy
/// @ingroup category_bluetooth
/// @brief Lightweight proxy for augmenting Bluetooth functionality
/// @defgroup pw_bluetooth_sapphire pw_bluetooth_sapphire
/// @ingroup category_bluetooth
/// @brief Battle-tested Bluetooth with rock-solid reliability
/// @defgroup pw_buf pw_buf
/// @ingroup category_memory
/// @brief Contiguous buffer views with optional memory ownership
/// @defgroup pw_build pw_build
/// @ingroup category_build
/// @brief Integrations for Bazel, GN, and CMake
/// @defgroup pw_bytes pw_bytes
/// @ingroup category_encoding
/// @brief Utilities for manipulating binary data
/// @defgroup pw_channel pw_channel
/// @ingroup category_comms
/// @brief Async, zero-copy API for sending and receiving bytes or datagrams
/// @defgroup pw_chre pw_chre
/// @ingroup category_android
/// @brief Android Context Hub Runtime Environment backend
/// @defgroup pw_chrono pw_chrono
/// @ingroup category_os
/// @brief Portable std::chrono for constrained embedded devices
/// @defgroup pw_clock_tree pw_clock_tree
/// @ingroup category_hal
/// @brief Clock tree management for embedded devices
/// @defgroup pw_clock_tree_mcuxpresso pw_clock_tree_mcuxpresso
/// @ingroup pw_clock_tree category_nxp
/// @brief NXP MCUXpresso SDK implementation for pw_clock_tree
/// @defgroup pw_cobs pw_cobs
/// @ingroup category_comms
/// @brief Consistent Overhead Byte Stuffing (COBS) codec and zero-copy frame streamer
/// @defgroup pw_containers pw_containers
/// @ingroup category_core
/// @brief Generic collections of objects for embedded devices
/// @defgroup pw_crypto pw_crypto
/// @ingroup category_crypto
/// @defgroup pw_digital_io pw_digital_io
/// @ingroup category_hal
/// @brief Digital I/O interface
/// @defgroup pw_digital_io_mcuxpresso pw_digital_io_mcuxpresso
/// @ingroup pw_digital_io category_nxp
/// @brief Digital I/O for NXP MCUXpresso
/// @defgroup pw_display pw_display
/// @ingroup category_hal
/// @brief Graphic display support and framebuffer management
/// @defgroup pw_elf pw_elf
/// @ingroup category_toolchain
/// @brief ELF file support
/// @defgroup pw_enum pw_enum
/// @ingroup category_core
/// @brief Rich enum support
/// @defgroup pw_function pw_function
/// @ingroup category_core
/// @brief Embedded-friendly std::function
/// @defgroup pw_hdlc pw_hdlc
/// @ingroup category_comms
/// @brief Simple, robust, and efficient serial communication
/// @defgroup pw_hex_dump pw_hex_dump
/// @ingroup category_observability
/// @brief Handy hexdump utilities
/// @defgroup pw_i2c pw_i2c
/// @ingroup category_hal
/// @brief Cross-platform I2C API with interactive debugging
/// @defgroup pw_i2c_linux pw_i2c_linux
/// @ingroup pw_i2c
/// @brief Linux userspace implementation for pw_i2c
/// @defgroup pw_i2c_zephyr pw_i2c_zephyr
/// @ingroup pw_i2c category_zephyr
/// @brief Zephyr implementation for pw_i2c
/// @defgroup pw_interrupt pw_interrupt
/// @ingroup category_os
/// @brief Interrupt handling utilities for embedded systems
/// @defgroup pw_json pw_json
/// @ingroup category_encoding
/// @brief Simple, efficient C++ JSON serialization
/// @defgroup pw_kvs pw_kvs
/// @ingroup category_memory
/// @brief Lightweight, persistent key-value store
/// @defgroup pw_log pw_log
/// @ingroup category_observability
/// @brief Robust and portable logging for embedded projects
/// @defgroup pw_log_string pw_log_string
/// @ingroup pw_log
/// @defgroup pw_log_tokenized pw_log_tokenized
/// @ingroup pw_log
/// @defgroup pw_malloc pw_malloc
/// @ingroup category_memory
/// @brief Replacement interface for standard libc dynamic memory operations
/// @defgroup pw_metric pw_metric
/// @ingroup category_observability
/// @defgroup pw_multibuf pw_multibuf
/// @ingroup category_memory
/// @brief A buffer API optimized for zero-copy messaging
/// @defgroup pw_numeric pw_numeric
/// @ingroup category_core
/// @brief Efficient mathematical utilities for embedded
/// @defgroup pw_perf_test pw_perf_test
/// @ingroup category_test
/// @brief Micro-benchmarks that are easy to write and run
/// @defgroup pw_persistent_ram pw_persistent_ram
/// @ingroup category_observability
/// @brief Reliable memory across reboots for crash reporting
/// @defgroup pw_polyfill pw_polyfill
/// @ingroup category_toolchain
/// @defgroup pw_preprocessor pw_preprocessor
/// @ingroup category_toolchain
/// @brief Helpful preprocessor macros
/// @defgroup pw_protobuf pw_protobuf
/// @ingroup category_encoding
/// @brief Expressive interface for encoding and decoding protocol buffers
/// @defgroup pw_random pw_random
/// @ingroup category_crypto
/// @defgroup pw_result pw_result
/// @ingroup category_core
/// @brief Error propagation primitives: value-or-error
/// @defgroup pw_rpc pw_rpc
/// @ingroup category_comms
/// @brief Efficient, low-code-size RPC system for embedded devices
/// @defgroup pw_rpc2 pw_rpc2
/// @ingroup category_comms
/// @brief Next gen, zero-copy async RPC with end-to-end backpressure
/// @defgroup pw_span pw_span
/// @ingroup category_core
/// @brief std::span for C++17
/// @defgroup pw_spi pw_spi
/// @ingroup category_hal
/// @defgroup pw_status pw_status
/// @ingroup category_core
/// @brief Exception-free error propagation for embedded
/// @defgroup pw_stream pw_stream
/// @ingroup category_comms
/// @brief A foundational interface for streaming data
/// @defgroup pw_stream_uart_linux pw_stream_uart_linux
/// @ingroup pw_stream
/// @brief UART stream implementation for Linux
/// @defgroup pw_string pw_string
/// @ingroup category_core
/// @brief Efficient, easy, and safe string manipulation
/// @defgroup pw_sync pw_sync
/// @ingroup category_os
/// @brief Portable abstraction for synchronization primitives
/// @defgroup pw_sys_io pw_sys_io
/// @ingroup category_hal
/// @defgroup pw_system pw_system
/// @ingroup category_os
/// @defgroup pw_thread pw_thread
/// @ingroup category_os
/// @brief Thread creation and execution
/// @defgroup pw_tokenizer pw_tokenizer
/// @ingroup category_observability
/// @brief Compress strings to shrink logs by +75%
/// @defgroup pw_toolchain pw_toolchain
/// @ingroup category_toolchain
/// @brief Embedded toolchains for GN-based Pigweed projects
/// @defgroup pw_trace pw_trace
/// @ingroup category_observability
/// @defgroup pw_trace_tokenized pw_trace_tokenized
/// @ingroup pw_trace
/// @defgroup pw_transfer pw_transfer
/// @ingroup category_comms
/// @defgroup pw_transport pw_transport
/// @ingroup category_comms
/// @brief Asynchronous datagram-oriented transport interface
/// @defgroup pw_uart pw_uart
/// @ingroup category_hal
/// @brief Core interfaces for UART communication
/// @defgroup pw_unit_test pw_unit_test
/// @ingroup category_test
/// @brief GoogleTest for embedded
/// @defgroup pw_uuid pw_uuid
/// @ingroup category_encoding
/// @brief 128-bit universally unique identifier (UUID)
/// @defgroup pw_varint pw_varint
/// @ingroup category_encoding
/// @brief Functions for encoding and decoding variable length integers
/// @defgroup pw_work_queue pw_work_queue
/// @ingroup category_async
// +----------------------------------------------------------+
// | END OF AUTO-GENERATED SECTION                            |
// +----------------------------------------------------------+
// categories-end

/// @defgroup pw_allocator_block Blocks
/// @ingroup pw_allocator
/// @brief An allocatable region of memory.
/// @details Blocks are the fundamental type managed by several of the
/// @ref pw_allocator_concrete_block

/// @defgroup pw_allocator_block_impl Block implementations
/// @ingroup pw_allocator_block
/// @brief The following combine block mix-ins and provide both the methods
/// they require as well as a concrete representation of the data those methods
/// need

/// @defgroup pw_allocator_block_mixins Block mix-ins
/// @ingroup pw_allocator_block
/// @brief Blocks are defined using several stateless “mix-in” interface types
/// @details These provide specific functionality, while deferring the detailed
/// representation of a block to a derived type.

/// @defgroup pw_allocator_bucket Buckets
/// @ingroup pw_allocator
/// @brief Data structures that track free blocks

/// @defgroup pw_allocator_config Configuration
/// @ingroup pw_allocator
/// @brief Options for controlling block poisoning intervals, validation
/// checks, and deprecation warnings

/// @defgroup pw_allocator_concrete Concrete allocators
/// @ingroup pw_allocator
/// @brief Concrete allocator implementations that provide memory dynamically

/// @defgroup pw_allocator_concrete_block Block allocators
/// @ingroup pw_allocator_concrete

/// @defgroup pw_allocator_core Core interfaces
/// @ingroup pw_allocator
/// @brief Generic allocator interfaces that can be injected into routines that
/// need dynamic memory
/// @details These include `Allocator`, as well as the `Layout` type that is
/// passed to it and the managed smart pointer types, such as `UniquePtr`, that
/// can be returned from it.

/// @defgroup pw_allocator_forwarding Forwarding allocators
/// @ingroup pw_allocator
/// @brief Allocator implementations that don’t allocate memory directly
/// and instead rely on other allocators while providing additional behaviors
/// @details Learn more: [Forwarding allocator
/// concept](../../pw_allocator/design.html#module-pw-allocator-design-forwarding)

/// @defgroup pw_allocator_impl_test_fuzz FuzzTest support
/// @ingroup pw_allocator_impl_test

/// @defgroup pw_allocator_impl Implementation interfaces
/// @ingroup pw_allocator
/// @brief Interfaces for allocator implementers
/// @details These interfaces are intended for allocator implementers, not for
/// `pw_allocator` consumers.

/// @defgroup pw_allocator_impl_size Size reports
/// @ingroup pw_allocator_impl
/// @brief Generate code size reports for allocator implementations

/// @defgroup pw_allocator_impl_test Testing and debugging
/// @ingroup pw_allocator_impl
/// @brief Test utilities for allocator implementers
/// @details These facilitate writing unit tests and fuzz tests for both
/// concrete and forwarding allocator implementations. They are not intended to
/// be used by module consumers.

/// @defgroup pw_allocator_util Utilities
/// @ingroup pw_allocator
/// @brief Helpers for metrics, fragmentation, and buffer management

/// @defgroup pw_async2_alloc Dynamic allocation
/// @ingroup pw_async2
/// @brief Heap allocate space for tasks

/// @defgroup pw_async2_context Context
/// @ingroup pw_async2
/// @brief Resources for scheduling asynchronous work

/// @defgroup pw_async2_dispatchers Dispatchers
/// @ingroup pw_async2
/// @brief Interface for the cooperative scheduler and concrete implementations
/// @details Learn more: [Dispatchers](../../pw_async2/dispatcher.html)

/// @defgroup pw_async2_combinators Combinators
/// @ingroup pw_async2
/// @brief Helpers for interacting with multiples futures
/// @details Learn more: [Combinators](../../pw_async2/futures.html#combinators)

/// @defgroup pw_async2_coroutines Coroutines
/// @ingroup pw_async2
/// @brief C++20 coroutine support.
/// @details Learn more: [Coroutines](../../pw_async2/coroutines.html)

/// @defgroup pw_async2_channels Channels
/// @ingroup pw_async2
/// @brief Inter-task communication
/// @details Learn more: [Channels](../../pw_async2/channels.html)

/// @defgroup pw_async2_deprecated Deprecated
/// @ingroup pw_async2
/// @brief Deprecated APIs

/// @defgroup pw_async2_futures Futures
/// @ingroup pw_async2
/// @brief Values that may not be ready yet
/// @details Learn more: [Futures](../../pw_async2/futures.html)

/// @defgroup pw_async2_poll Poll
/// @ingroup pw_async2
/// @brief Statuses for asynchronous operations

/// @defgroup pw_async2_tasks Tasks
/// @ingroup pw_async2
/// @brief Logical collections of asynchronous work
/// @details Learn more: [Tasks](../../pw_async2/tasks.html)

/// @defgroup pw_async2_time Time
/// @ingroup pw_async2
/// @brief Interacting with time, timers, and timeouts
/// @details Learn more: [Interacting with time, timers, and timeouts](../../pw_async2/guides.html#interacting-with-timers-delays-and-timeouts)

/// @defgroup pw_async2_wakers Wakers
/// @ingroup pw_async2
/// @brief Queue work in response to async events
/// @details Learn more: [Wakers](../../pw_async2/informed_poll.html#wakers-progress-updates-for-the-dispatcher)

/// @defgroup pw_async2_config Configuration
/// @ingroup pw_async2
/// @brief Compile-time configuration
/// @details Learn more: [Configuration](../../pw_async2/guides.html#configuring-pw-async2)

/// @defgroup pw_bytes_ptr Packed pointers
/// @ingroup pw_bytes
/// @brief Store data in unused pointer bits

/// @defgroup pw_bytes_align Alignment
/// @ingroup pw_bytes
/// @brief Functions for memory alignment

/// @defgroup pw_bytes_array Byte arrays
/// @ingroup pw_bytes
/// @brief Compile-time construction of byte arrays

/// @defgroup pw_bytes_build Byte builders
/// @ingroup pw_bytes
/// @brief Dynamically build byte sequences in fixed-size buffers

/// @defgroup pw_bytes_bit Bit manipulation
/// @ingroup pw_bytes
/// @brief C++20 `<bit>` features and extensions

/// @defgroup pw_bytes_endian Endian conversion
/// @ingroup pw_bytes
/// @brief Utilities for handling byte order

/// @defgroup pw_bytes_suffix Byte suffixes
/// @ingroup pw_bytes
/// @brief Convenient literal for `std::byte`

/// @defgroup pw_bytes_unit Byte units
/// @ingroup pw_bytes
/// @brief Constants and literals for byte sizes e.g. KiB

/// @defgroup pw_bytes_span Byte spans
/// @ingroup pw_bytes
/// @brief Aliases for `pw::span` of bytes

/// @defgroup pw_json_builder_api Builder API
/// @ingroup pw_json
/// @brief Serialize JSON into fixed-size buffers.

/// @defgroup pw_log_global_config Global config
/// @ingroup pw_log
/// @details These configuration options differ from the options in
/// `pw_log/options.h` in that these should be set at a global level in the
/// build system rather than at a module or compile unit level.

/// @defgroup pw_log_module_config Module config
/// @ingroup pw_log
/// @details These configuration options differ from the options in
/// `pw_log/config.h` in that these should be set at a module/compile unit
/// level rather than a global level.

/// @defgroup pw_log_proto Protobuf helpers
/// @ingroup pw_log
/// @details Learn more: [The pw_log protobuf](../../pw_log/protobuf.html)

/// @defgroup pw_log_short Short macros
/// @ingroup pw_log
/// @brief Optional shortened versions of the `PW_LOG` macros
/// @details These shortened versions are not prefixed with `PW_*`, so may
/// collide with other macros.

/// @defgroup pw_log_glog Google Logging (glog) macros
/// @ingroup pw_log
/// @details @warning Pigweed strongly recommends sticking to `printf`-style
/// logging instead of C++ stream-style glog logging unless absolutely
/// necessary. These macros are only provided for compatibility with
/// non-embedded code.

/// @defgroup pw_log_tokenized_args Tokenized args
/// @ingroup pw_log
/// @details Learn more: [Tokenized log arguments](../../pw_log/tokenized_args.html)

/// @defgroup pw_multibuf_v1 Legacy v1 API
/// @ingroup pw_multibuf
/// @brief Interfaces that will eventually be deprecated
/// @details Most users of `pw_multibuf` will start by allocating a `MultiBuf`
/// using a `MultiBufAllocator` class, such as the `SimpleAllocator`.
///
/// A `MultiBuf` consists of a number of `Chunk` instances representing
/// contiguous memory regions. A `Chunk` can be grown or shrunk which allows
/// `MultiBuf` to be grown or shrunk. This allows, for example, lower layers to
/// reserve part of a `MultiBuf` for a header or footer. See `Chunk` for more
/// details.
///
/// `MultiBuf` exposes an `std::byte` iterator interface as well as a `Chunk`
/// iterator available through the `Chunks()` method. It allows extracting a
/// `Chunk` as an RAII-style `OwnedChunk` which manages its own lifetime.

/// @defgroup pw_multibuf_v1_impl Allocator implementation API
/// @ingroup pw_multibuf_v1
/// @details Some users will need to directly implement the `MultiBufAllocator`
/// interface in order to provide allocation out of a particular region,
/// provide particular allocation policy, fix chunks to some size (such as MTU
/// size - header for socket implementations), or specify other custom behavior.
/// These users will also need to understand and implement the
/// `ChunkRegionTracker` API.
///
/// A simple implementation of a `ChunkRegionTracker` is provided, called
/// `HeaderChunkRegionTracker`. It stores its `Chunk` and region metadata in a
/// `Allocator` allocation alongside the data. The allocation process is
/// synchronous, making this class suitable for testing. The allocated region or
/// `Chunk` must not outlive the provided allocator.
///
/// Another `ChunkRegionTracker` specialization is the lightweight
/// `SingleChunkRegionTracker`, which does not rely on `Allocator` and uses the
/// provided memory view to create a single chunk. This is useful when a single
/// `Chunk` is sufficient at no extra overhead. However, the user needs to own
/// the provided memory and know when a new `Chunk` can be requested.

/// @defgroup pw_multibuf_v1_test Test-only features
/// @ingroup pw_multibuf_v1

/// @defgroup pw_multibuf_v2 Experimental v2 API
/// @ingroup pw_multibuf
/// @brief Experimental API that separates out the concern of memory allocation

/// @defgroup pw_preprocessor_compiler Compiler
/// @ingroup pw_preprocessor_internal
/// @brief Public compiler attributes and macros.

/// @defgroup pw_preprocessor_internal Internal
/// @ingroup pw_preprocessor
/// @brief Internal implementation details.

/// @defgroup pw_protobuf_find Find APIs
/// @ingroup pw_protobuf

/// @defgroup pw_rpc_test Benchmarking & testing
/// @ingroup pw_rpc

/// @defgroup pw_rpc_channel Channels
/// @ingroup pw_rpc

/// @defgroup pw_rpc_sync Synchronous API
/// @ingroup pw_rpc

/// @defgroup pw_rpc_config Configuration
/// @ingroup pw_rpc

/// @defgroup pw_rpc2_client Client
/// @ingroup pw_rpc2
/// @brief Connect to an RPC server and invoke RPC methods
/// @details Learn more: [Calling RPCs](../../pw_rpc2/examples.html#module-pw-rpc2-examples-call)

/// @defgroup pw_rpc2_server Server
/// @ingroup pw_rpc2
/// @brief Register RPC services and accept incoming client connections
/// @details Learn more: [Implementing a service](../../pw_rpc2/examples.html#module-pw-rpc2-examples-implement)

/// @defgroup pw_rpc2_readers_writers Readers and writers
/// @ingroup pw_rpc2
/// @brief Read and write RPC messages and responses

/// @defgroup pw_rpc2_status Status
/// @ingroup pw_rpc2
/// @brief %Status codes and result types for RPC operations
/// @details Learn more: [Error handling](../../pw_rpc2/cpp.html#module-pw-rpc2-cpp-errors)

/// @defgroup pw_rpc2_testing Testing
/// @ingroup pw_rpc2
/// @brief Unit test RPC services and clients end to end
/// @details Learn more: [Unit testing](../../pw_rpc2/testing.html)

/// @defgroup pw_stream_interface Interfaces
/// @ingroup pw_stream
/// @brief Generic stream interfaces that support a combination of reading,
/// writing, and seeking

/// @defgroup pw_stream_interface_reader Readers
/// @ingroup pw_stream_interface
/// @brief Streams that support reading but not writing

/// @defgroup pw_stream_interface_writer Writers
/// @ingroup pw_stream_interface
/// @brief Streams that support writing but not reading

/// @defgroup pw_stream_interface_readerwriter ReaderWriters
/// @ingroup pw_stream_interface
/// @brief Streams that support both reading and writing

/// @defgroup pw_stream_concrete Implementations
/// @ingroup pw_stream
/// @brief Concrete implementations of stream interfaces for general use

/// @defgroup pw_string_inline InlineString and InlineBasicString
/// @ingroup pw_string
/// @brief Safer alternatives to `std::string` and `std::basic_string`

/// @defgroup pw_string_builder StringBuilder
/// @ingroup pw_string
/// @brief The flexibility of `std::ostringstream` but with a small footprint

/// @defgroup pw_string_util Utilities
/// @ingroup pw_string
/// @brief Safer alternatives to C++ standard library string functions

/// @defgroup pw_string_utf8 UTF-8 helpers
/// @ingroup pw_string
/// @brief Basic helpers for reading and writing UTF-8-encoded strings

/// @defgroup pw_tokenizer_tokenize Tokenization
/// @ingroup pw_tokenizer
/// @brief Convert string literals to tokens

/// @defgroup pw_tokenizer_detokenize Detokenization
/// @ingroup pw_tokenizer
/// @brief Expand a token to the string it represents and decode its arguments

/// @defgroup pw_tokenizer_database Token databases
/// @ingroup pw_tokenizer
/// @brief Store a mapping of tokens to the strings they represent

/// @defgroup pw_tokenizer_config Configuration
/// @ingroup pw_tokenizer
/// @brief Tokenization customization options

/// @defgroup pw_unit_test_declaration Test declaration
/// @ingroup pw_unit_test

/// @defgroup pw_unit_test_control Test control
/// @ingroup pw_unit_test

/// @defgroup pw_unit_test_expectations Expectations
/// @ingroup pw_unit_test
/// @details When a test fails an expectation, the framework marks the test as
/// a failure and then continues executing the test. They’re useful when you
/// want to verify multiple dimensions of the same feature and see all the
/// errors at the same time.

/// @defgroup pw_unit_test_assertions Assertions
/// @ingroup pw_unit_test
/// @details Assertions work the same as expectations except they stop the
/// execution of the test as soon as a failed condition is met.

/// @defgroup pw_unit_test_events Event handlers
/// @ingroup pw_unit_test

/// @defgroup pw_unit_test_configuration Configuration
/// @ingroup pw_unit_test

/// @defgroup pw_unit_test_helpers Helpers
/// @ingroup pw_unit_test

/// @defgroup pw_unit_test_constexpr Constexpr tests
/// @ingroup pw_unit_test

/// @defgroup third_party third-party
/// @brief API integrations with third-party software e.g. FreeRTOS

/// @defgroup third_party_freertos FreeRTOS
/// @ingroup third_party
/// @brief FreeRTOS application functions

// clang-format on
