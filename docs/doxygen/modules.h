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
// in //docs/sphinx/module_metadata.json.

// TODO: b/441319784 - Move each module's content into that module's directory.
// E.g. the pw_allocator content should go into //pw_allocator/doxygen.h

// clang-format off

/// @defgroup pw_alignment pw_alignment
/// @brief Natural object alignment, guaranteed.
/// @maindocs
/// [Home](../../pw_alignment/docs.html)
/// @endmaindocs

/// @defgroup pw_allocator pw_allocator
/// @brief Flexible, safe, and measurable memory allocation.
/// @maindocs
/// [Home](../../pw_allocator/docs.html) |
/// [Guides](../../pw_allocator/guide.html) |
/// [Design & roadmap](../../pw_allocator/design.html) |
/// [Code size analysis](../../pw_allocator/code_size.html)
/// @endmaindocs

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

/// @defgroup pw_analog pw_analog
/// @brief Analog-to-digital converter libraries and utilities
/// @maindocs
/// [Home](../../pw_analog/docs.html)
/// @endmaindocs

/// @defgroup pw_async pw_async
/// @brief Portable APIs for asynchronous code.
/// @maindocs
/// [Home](../../pw_async/docs.html)
/// @endmaindocs

/// @defgroup pw_async2 pw_async2
/// @brief Cooperative async tasks for embedded.
/// @maindocs
/// [Home](../../pw_async2/docs.html) |
/// [Quickstart](../../pw_async2/quickstart.html) |
/// [Informed poll](../../pw_async2/informed_poll.html) |
/// [Codelab](../../pw_async2/codelab/docs.html) |
/// [Guides](../../pw_async2/guides.html) |
/// [Dispatcher](../../pw_async2/dispatcher.html) |
/// [Code size analysis](../../pw_async2/code_size.html) |
/// [Coroutines](../../pw_async2/coroutines.html) |
/// [Futures](../../pw_async2/futures.html) |
/// [Channels](../../pw_async2/channels.html) |
/// [Tasks](../../pw_async2/tasks.html)
/// @endmaindocs

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

/// @defgroup pw_async_basic pw_async_basic
/// @maindocs
/// [Home](../../pw_async_basic/docs.html)
/// @endmaindocs

/// @defgroup pw_base64 pw_base64
/// @brief Base64 encoding, decoding, and validating.
/// @maindocs
/// [Home](../../pw_base64/docs.html)
/// @endmaindocs

/// @defgroup pw_bloat pw_bloat
/// @brief Utilities for generating binary size reports.
/// @maindocs
/// [Home](../../pw_bloat/docs.html)
/// @endmaindocs

/// @defgroup pw_bluetooth pw_bluetooth
/// @brief Host-layer Bluetooth Low Energy APIs and utilities.
/// @maindocs
/// [Home](../../pw_bluetooth/docs.html)
/// @endmaindocs

/// @defgroup pw_bluetooth_proxy pw_bluetooth_proxy
/// @brief Lightweight proxy for augmenting Bluetooth functionality.
/// @maindocs
/// [Home](../../pw_bluetooth_proxy/docs.html)
/// @endmaindocs

/// @defgroup pw_bluetooth_sapphire pw_bluetooth_sapphire
/// @brief Battle-tested Bluetooth with rock-solid reliability.
/// @maindocs
/// [Home](../../pw_bluetooth_sapphire/docs.html)
/// @endmaindocs

/// @defgroup pw_build pw_build
/// @brief Integrations for Bazel, GN, and CMake.
/// @maindocs
/// [Home](../../pw_build/docs.html)
/// @endmaindocs

/// @defgroup pw_bytes pw_bytes
/// @brief Utilities for manipulating binary data.
/// @maindocs
/// [Home](../../pw_bytes/docs.html)
/// @endmaindocs

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

/// @defgroup pw_channel pw_channel
/// @brief Async, zero-copy API for sending and receiving bytes or datagrams.
/// @maindocs
/// [Home](../../pw_channel/docs.html)
/// @endmaindocs

/// @defgroup pw_chre pw_chre
/// @brief Android Context Hub Runtime Environment backend.
/// @maindocs
/// [Home](../../pw_chre/docs.html)
/// @endmaindocs

/// @defgroup pw_chrono pw_chrono
/// @brief Portable `std::chrono` for constrained embedded devices.
/// @maindocs
/// [Home](../../pw_chrono/docs.html)
/// @endmaindocs

/// @defgroup pw_clock_tree pw_clock_tree
/// @brief Clock tree management for embedded devices.
/// @maindocs
/// [Home](../../pw_clock_tree/docs.html)
/// @endmaindocs

/// @defgroup pw_clock_tree_mcuxpresso pw_clock_tree_mcuxpresso
/// @brief NXP MCUXpresso SDK implementation for pw_clock_tree.
/// @maindocs
/// [Home](../../pw_clock_tree_mcuxpresso/docs.html)
/// @endmaindocs

/// @defgroup pw_containers pw_containers
/// @brief Generic collections of objects for embedded devices.
/// @maindocs
/// [Home](../../pw_containers/docs.html) |
/// [Lists](../../pw_containers/lists.html) |
/// [Maps](../../pw_containers/maps.html) |
/// [Queues](../../pw_containers/queues.html) |
/// [Sets](../../pw_containers/sets.html) |
/// [Vectors](../../pw_containers/vectors.html) |
/// [Utilities](../../pw_containers/utilities.html) |
/// [Using items with multiple containers](../../pw_containers/multiple_containers.html)
/// @endmaindocs

/// @defgroup pw_crypto pw_crypto
/// @maindocs
/// [Home](../../pw_crypto/docs.html)
/// @endmaindocs

/// @defgroup pw_digital_io pw_digital_io
/// @brief Digital I/O interface.
/// @maindocs
/// [Home](../../pw_digital_io/docs.html)
/// @endmaindocs

/// @defgroup pw_digital_io_mcuxpresso pw_digital_io_mcuxpresso
/// @brief Digital I/O for NXP MCUXpresso.
/// @maindocs
/// [Home](../../pw_digital_io_mcuxpresso/docs.html)
/// @endmaindocs

/// @defgroup pw_display pw_display
/// @brief Graphic display support and framebuffer management.
/// @maindocs
/// [Home](../../pw_display/docs.html)
/// @endmaindocs

/// @defgroup pw_elf pw_elf
/// @brief ELF file support.
/// @maindocs
/// [Home](../../pw_elf/docs.html)
/// @endmaindocs

/// @defgroup pw_function pw_function
/// @brief Embedded-friendly `std::function`.
/// @maindocs
/// [Home](../../pw_function/docs.html)
/// @endmaindocs

/// @defgroup pw_hdlc pw_hdlc
/// @brief Simple, robust, and efficient serial communication.
/// @maindocs
/// [Home](../../pw_hdlc/docs.html)
/// @endmaindocs

/// @defgroup pw_hex_dump pw_hex_dump
/// @brief Handy hexdump utilities.
/// @maindocs
/// [Home](../../pw_hex_dump/docs.html)
/// @endmaindocs

/// @defgroup pw_i2c pw_i2c
/// @brief Cross-platform I2C API with interactive debugging.
/// @maindocs
/// [Home](../../pw_i2c/docs.html)
/// @endmaindocs

/// @defgroup pw_i2c_linux pw_i2c_linux
/// @brief Linux userspace implementation for pw_i2c.
/// @maindocs
/// [Home](../../pw_i2c_linux/docs.html)
/// @endmaindocs

/// @defgroup pw_i2c_zephyr pw_i2c_zephyr
/// @brief Zephyr implementation for pw_i2c.
/// @maindocs
/// [Home](../../pw_i2c_zephyr/docs.html)
/// @endmaindocs

/// @defgroup pw_interrupt pw_interrupt
/// @brief Interrupt handling utilities for embedded systems.
/// @maindocs
/// [Home](../../pw_interrupt/docs.html)
/// @endmaindocs

/// @defgroup pw_json pw_json
/// @brief Simple, efficient C++ JSON serialization.
/// @maindocs
/// [Home](../../pw_json/docs.html)
/// @endmaindocs

/// @defgroup pw_kvs pw_kvs
/// @brief Lightweight, persistent key-value store.
/// @maindocs
/// [Home](../../pw_kvs/docs.html)
/// @endmaindocs

/// @defgroup pw_log pw_log
/// @brief Robust and portable logging for embedded projects
/// @maindocs
/// [Home](../../pw_log/docs.html) |
/// [The pw_log protobuf](../../pw_log/protobuf.html) |
/// [Tokenized log arguments](../../pw_log/tokenized_args.html) |
/// [Backends](../../pw_log/backends.html)
/// @endmaindocs

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

/// @defgroup pw_log_string pw_log_string
/// @maindocs
/// [Home](../../pw_log_string/docs.html)
/// @endmaindocs

/// @defgroup pw_log_tokenized pw_log_tokenized
/// @maindocs
/// [Home](../../pw_log_tokenized/docs.html)
/// @endmaindocs

/// @defgroup pw_malloc pw_malloc
/// @brief Replacement interface for standard libc dynamic memory operations.
/// @maindocs
/// [Home](../../pw_malloc/docs.html)
/// @endmaindocs

/// @defgroup pw_multibuf pw_multibuf
/// @brief A buffer API optimized for zero-copy messaging.
/// @maindocs
/// [Home](../../pw_multibuf/docs.html) |
/// [Code size analysis](../../pw_multibuf/code_size.html)
/// @endmaindocs

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

/// @defgroup pw_numeric pw_numeric
/// @brief Efficient mathematical utilities for embedded.
/// @maindocs
/// [Home](../../pw_numeric/docs.html)
/// @endmaindocs

/// @defgroup pw_perf_test pw_perf_test
/// @brief Micro-benchmarks that are easy to write and run.
/// @maindocs
/// [Home](../../pw_perf_test/docs.html)
/// @endmaindocs

/// @defgroup pw_persistent_ram pw_persistent_ram
/// @maindocs
/// [Home](../../pw_persistent_ram/docs.html)
/// @endmaindocs

/// @defgroup pw_polyfill pw_polyfill
/// @maindocs
/// [Home](../../pw_polyfill/docs.html)
/// @endmaindocs

/// @defgroup pw_preprocessor pw_preprocessor
/// @brief Helpful preprocessor macros.
/// @maindocs
/// [Home](../../pw_preprocessor/docs.html)
/// @endmaindocs

/// @defgroup pw_protobuf pw_protobuf
/// @brief Expressive interface for encoding and decoding protocol buffers
/// @maindocs
/// [Home](../../pw_protobuf/docs.html) |
/// [Extended size report](../../pw_protobuf/size_report.html)
/// @endmaindocs

/// @defgroup pw_protobuf_find Find APIs
/// @ingroup pw_protobuf

/// @defgroup pw_random pw_random
/// @maindocs
/// [Home](../../pw_random/docs.html)
/// @endmaindocs

/// @defgroup pw_result pw_result
/// @brief Error propagation primitives: value-or-error
/// @maindocs
/// [Home](../../pw_random/docs.html)
/// @endmaindocs

/// @defgroup pw_rpc pw_rpc
/// @brief Efficient, low-code-size RPC system for embedded devices
/// @maindocs
/// [Home](../../pw_rpc/docs.html) |
/// [Quickstart & guides](../../pw_rpc/guides.html) |
/// [Client, server, and protobuf libraries](../../pw_rpc/libraries.html) |
/// [C++ server and client](../../pw_rpc/cpp.html) |
/// [Python client](../../pw_rpc/py/docs.html) |
/// [TypeScript client](../../pw_rpc/ts/docs.html) |
/// [Nanopb codegen](../../pw_rpc/nanopb/docs.html) |
/// [pw_protobuf codegen](../../pw_rpc/pwpb/docs.html) |
/// [Packet protocol](../../pw_rpc/protocol.html) |
/// [Design & roadmap](../../pw_rpc/design.html) |
/// [HDLC example](../../pw_hdlc/rpc_example/docs.html)
/// @endmaindocs

/// @defgroup pw_rpc_test Benchmarking & testing
/// @ingroup pw_rpc

/// @defgroup pw_rpc_channel Channels
/// @ingroup pw_rpc

/// @defgroup pw_rpc_sync Synchronous API
/// @ingroup pw_rpc
/// @maindocs
/// [Client synchronous call
/// wrappers](../../pw_rpc/cpp.html#client-synchronous-call-wrappers)
/// @endmaindocs

/// @defgroup pw_rpc_config Configuration
/// @ingroup pw_rpc

/// @defgroup pw_span pw_span
/// @brief std::span for C++17.
/// @maindocs
/// [Home](../../pw_span/docs.html)
/// @endmaindocs

/// @defgroup pw_status pw_status
/// @brief Exception-free error propagation for embedded
/// @maindocs
/// [Home](../../pw_status/docs.html) |
/// [Quickstart & guides](../../pw_status/guide.html) |
/// [Reference](../../pw_status/reference.html)
/// @endmaindocs

/// @defgroup pw_stream pw_stream
/// @brief A foundational interface for streaming data
/// @maindocs
/// [Home](../../pw_stream/docs.html) |
/// [Backends](../../pw_stream/backends.html) |
/// [Python](../../pw_stream/py/docs.html)
/// @endmaindocs

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

/// @defgroup pw_stream_uart_linux pw_stream_uart_linux
/// @brief UART stream implementation for Linux.
/// @maindocs
/// [Home](../../pw_stream_uart_linux/docs.html)
/// @endmaindocs

/// @defgroup pw_string pw_string
/// @brief Efficient, easy, and safe string manipulation
/// @maindocs
/// [Home](../../pw_string/docs.html) |
/// [Get started & guides](../../pw_string/guide.html) |
/// [Design & roadmap](../../pw_string/design.html) |
/// [Code size analysis](../../pw_string/code_size.html)
/// @endmaindocs

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

/// @defgroup pw_sync pw_sync
/// @maindocs
/// [Home](../../pw_sync/docs.html)
/// @endmaindocs

/// @defgroup pw_sys_io pw_sys_io
/// @maindocs
/// [Home](../../pw_sys_io/docs.html)
/// @endmaindocs

/// @defgroup pw_system pw_system
/// @maindocs
/// [Home](../../pw_system/docs.html)
/// @endmaindocs

/// @defgroup pw_thread pw_thread
/// @maindocs
/// [Home](../../pw_thread/docs.html)
/// @endmaindocs

/// @defgroup pw_tokenizer pw_tokenizer
/// @brief Compress strings to shrink logs by +75%
/// @maindocs
/// [Home](../../pw_tokenizer/docs.html) |
/// [Quickstart](../../pw_tokenizer/get_started.html) |
/// [Tokenization](../../pw_tokenizer/tokenization.html) |
/// [Token databases](../../pw_tokenizer/token_databases.html) |
/// [Detokenization](../../pw_tokenizer/detokenization.html)
/// @endmaindocs

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

/// @defgroup pw_toolchain pw_toolchain
/// @brief Embedded toolchains for GN-based Pigweed projects.
/// @maindocs
/// [Home](../../pw_toolchain/docs.html)
/// @endmaindocs

/// @defgroup pw_trace_tokenized pw_trace_tokenized
/// @maindocs
/// [Home](../../pw_trace_tokenized/docs.html)
/// @endmaindocs

/// @defgroup pw_transfer pw_transfer
/// @maindocs
/// [Home](../../pw_transfer/docs.html)
/// @endmaindocs

/// @defgroup pw_varint pw_varint
/// @brief Functions for encoding and decoding variable length integers.
/// @maindocs
/// [Home](../../pw_varint/docs.html)
/// @endmaindocs

/// @defgroup pw_uart pw_uart
/// @brief Core interfaces for UART communication.
/// @maindocs
/// [Home](../../pw_uart/docs.html)
/// @endmaindocs

/// @defgroup pw_unit_test pw_unit_test
/// @brief GoogleTest for embedded
/// @maindocs
/// [Home](../../pw_unit_test/docs.html)
/// @endmaindocs

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

/// @defgroup pw_uuid pw_uuid
/// @brief 128-bit universally unique identifier (UUID).
/// @maindocs
/// [Home](../../pw_uuid/docs.html)
/// @endmaindocs

/// @defgroup pw_work_queue pw_work_queue
/// @maindocs
/// [Home](../../pw_work_queue/docs.html)
/// @endmaindocs

/// @defgroup third_party third-party
/// @brief API integrations with third-party software e.g. FreeRTOS

/// @defgroup third_party_freertos FreeRTOS
/// @ingroup third_party
/// @brief FreeRTOS application functions

// clang-format on
