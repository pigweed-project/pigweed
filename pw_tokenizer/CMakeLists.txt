# Copyright 2020 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)
include($ENV{PW_ROOT}/pw_build/cc_blob_library.cmake)

pw_add_module_config(pw_tokenizer_CONFIG)

pw_add_library(pw_tokenizer.config INTERFACE
  HEADERS
    public/pw_tokenizer/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_tokenizer_CONFIG}
)

pw_add_library(pw_tokenizer STATIC
  HEADERS
    public/pw_tokenizer/encode_args.h
    public/pw_tokenizer/enum.h
    public/pw_tokenizer/hash.h
    public/pw_tokenizer/nested_tokenization.h
    public/pw_tokenizer/tokenize.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_base64
    pw_bytes
    pw_containers.to_array
    pw_result
    pw_span
    pw_preprocessor
    pw_tokenizer.config
    pw_varint
  SOURCES
    encode_args.cc
    hash.cc
    public/pw_tokenizer/internal/argument_types.h
    public/pw_tokenizer/internal/argument_types_macro_4_byte.h
    public/pw_tokenizer/internal/argument_types_macro_8_byte.h
    public/pw_tokenizer/internal/enum.h
    public/pw_tokenizer/internal/pw_tokenizer_65599_fixed_length_128_hash_macro.h
    public/pw_tokenizer/internal/pw_tokenizer_65599_fixed_length_256_hash_macro.h
    public/pw_tokenizer/internal/pw_tokenizer_65599_fixed_length_80_hash_macro.h
    public/pw_tokenizer/internal/pw_tokenizer_65599_fixed_length_96_hash_macro.h
    public/pw_tokenizer/internal/tokenize_string.h
    tokenize.cc
)

if(Zephyr_FOUND AND CONFIG_PIGWEED_TOKENIZER)
  zephyr_linker_sources(SECTIONS "${CMAKE_CURRENT_SOURCE_DIR}/pw_tokenizer_zephyr.ld")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "")
  target_link_options(pw_tokenizer
    PUBLIC
      "-T${CMAKE_CURRENT_SOURCE_DIR}/pw_tokenizer_linker_sections.ld"
      "-L${CMAKE_CURRENT_SOURCE_DIR}"
  )
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
  target_link_options(pw_tokenizer
    PUBLIC
      "-T${CMAKE_CURRENT_SOURCE_DIR}/pw_add_tokenizer_sections_to_default_script.ld"
      "-L${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()

pw_add_library(pw_tokenizer.base64 STATIC
  HEADERS
    public/pw_tokenizer/base64.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_base64
    pw_span
    pw_string.string
    pw_tokenizer
    pw_tokenizer.config
  SOURCES
    base64.cc
)

pw_add_library(pw_tokenizer.decoder STATIC
  HEADERS
    public/pw_tokenizer/detokenize.h
    public/pw_tokenizer/token_database.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_span
    pw_stream
    pw_tokenizer
    pw_tokenizer.base64
    pw_tokenizer._csv
  SOURCES
    decode.cc
    detokenize.cc
    public/pw_tokenizer/internal/decode.h
    token_database.cc
  PRIVATE_DEPS
    pw_bytes
    pw_bytes.bit
    pw_elf.reader
    pw_tokenizer._csv
    pw_varint
)

pw_add_library(pw_tokenizer.detokenize_from_this_program STATIC
  HEADERS
    public/pw_tokenizer/detokenize_from_this_program.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_result
    pw_tokenizer.decoder
  SOURCES
    detokenize_from_this_program.cc
)

pw_add_library(pw_tokenizer._csv STATIC
  HEADERS
    private/pw_tokenizer_private/csv.h
  PUBLIC_INCLUDES
    private
  SOURCES
    csv.cc
  PRIVATE_DEPS
    pw_log
)

pw_add_test(pw_tokenizer.csv_test
  SOURCES
    csv_test.cc
    private/pw_tokenizer_private/csv.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_tokenizer._csv
  GROUPS
    modules
    pw_tokenizer
)

pw_proto_library(pw_tokenizer.proto
  SOURCES
    pw_tokenizer_proto/options.proto
)

# Executable for generating test data for the C++ and Python detokenizers. This
# target should only be built for the host.
add_executable(pw_tokenizer.generate_decoding_test_data EXCLUDE_FROM_ALL
    generate_decoding_test_data.cc)
target_link_libraries(pw_tokenizer.generate_decoding_test_data PRIVATE
    pw_varint pw_tokenizer)
target_compile_options(pw_tokenizer.generate_decoding_test_data PRIVATE
    -Wall -Werror)

# Executable for generating a test ELF file for elf_reader_test.py. A host
# version of this binary is checked in for use in elf_reader_test.py.
add_executable(pw_tokenizer.elf_reader_test_binary EXCLUDE_FROM_ALL
    py/elf_reader_test_binary.c)
target_link_libraries(pw_tokenizer.elf_reader_test_binary PRIVATE
    -Wl,--unresolved-symbols=ignore-all)  # main is not defined
set_target_properties(pw_tokenizer.elf_reader_test_binary PROPERTIES
    OUTPUT_NAME elf_reader_test_binary.elf)

pw_add_test(pw_tokenizer.argument_types_test
  SOURCES
    argument_types_test_c.c
    argument_types_test.cc
    private/pw_tokenizer_private/argument_types_test.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_tokenizer
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.base64_test
  SOURCES
    base64_test.cc
  PRIVATE_DEPS
    pw_tokenizer.base64
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.decode_test
  SOURCES
    decode_test.cc
    private/pw_tokenizer_private/tokenized_string_decoding_test_data.h
    private/pw_tokenizer_private/varint_decoding_test_data.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_varint
    pw_tokenizer.decoder
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.detokenize_test
  SOURCES
    detokenize_test.cc
    private/pw_tokenizer_private/printf_support.h
    private/pw_tokenizer_private/printf_support.cc
    private/pw_tokenizer_private/tokenize_test.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_base64
    pw_tokenizer.decoder
    pw_tokenizer.detokenizer_elf_test_blob
    pw_stream
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.encode_args_test
  SOURCES
    encode_args_test.cc
  PRIVATE_DEPS
    pw_tokenizer
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.hash_test
  SOURCES
    hash_test.cc
    private/pw_tokenizer_private/generated_hash_test_cases.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_tokenizer
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.token_database_test
  SOURCES
    token_database_test.cc
  PRIVATE_DEPS
    pw_tokenizer.decoder
  GROUPS
    modules
    pw_tokenizer
)

pw_add_test(pw_tokenizer.tokenize_test
  SOURCES
    tokenize_test_c.c
    tokenize_test.cc
    private/pw_tokenizer_private/tokenize_test.h
  PRIVATE_INCLUDES
    private
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_string.string
    pw_tokenizer
    pw_varint
  GROUPS
    modules
    pw_tokenizer
)

pw_cc_blob_library(pw_tokenizer.detokenizer_elf_test_blob
  HEADER
    pw_tokenizer/example_binary_with_tokenized_strings.h
  NAMESPACE
    test::ns
  BLOB
    SYMBOL_NAME kElfSection
    PATH py/example_binary_with_tokenized_strings.elf
)

if(Zephyr_FOUND)
  zephyr_link_libraries_ifdef(CONFIG_PIGWEED_TOKENIZER pw_tokenizer)
  zephyr_link_libraries_ifdef(CONFIG_PIGWEED_TOKENIZER_BASE64 pw_tokenizer.base64)
  zephyr_link_libraries_ifdef(CONFIG_PIGWEED_DETOKENIZER pw_tokenizer.decoder)
endif()
