# Copyright 2024 The Pigweed Authors
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
include_guard(GLOBAL)

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

function(emboss_cc_library NAME)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      1
    ONE_VALUE_ARGS
      SOURCES
    MULTI_VALUE_ARGS
      IMPORT_DIRS
      DEPS
    REQUIRED_ARGS
      SOURCES)

  # Include default import dirs with the user specified values.
  list(APPEND arg_IMPORT_DIRS $ENV{PW_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})

  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/${NAME}")

  # embossc will output and emb to: <output_path>/<input_path>.emb.h
  pw_rebase_paths(outputs ${out_dir} "${CMAKE_CURRENT_SOURCE_DIR}"
    "${arg_SOURCES}" ".emb.h")

  # Set the include path to export to the output file's directory.
  get_filename_component(output_include_path "${outputs}" DIRECTORY)

  # Make the import dir paths absolute
  pw_rebase_paths(abs_import_dirs "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${arg_IMPORT_DIRS}" "")

  # Expose a list of our sources so that other generate steps can depend on
  # them.
  pw_rebase_paths(abs_sources "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${arg_SOURCES}" "")
  pw_add_library_generic("${NAME}._sources" INTERFACE
    # Disable the sandbox since generated headers are inherently sandboxed.
    SANDBOX
      OFF
    HEADERS
      ${abs_sources}
    PUBLIC_INCLUDES
      ${abs_import_dirs}
  )

  # Build up a list of other `emb` sources the generate step depends on. We
  # use this rather than the full emboss_cc_library so that the generate steps
  # can run in parallel.
  set(source_deps "${arg_DEPS}")
  list(TRANSFORM source_deps APPEND ._sources)

  # We need to extract the actual files from the targets for them to work
  # as proper dependencies for `add_custom_command`.
  set(dependent_sources "")
  foreach(dep IN LISTS source_deps)
    get_target_property(sources ${dep} SOURCES)
    list(APPEND dependent_sources ${sources})
    get_target_property(
      imports ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    list(APPEND abs_import_dirs ${imports})
  endforeach()

  # Setup the emboss command:
  # python3 $runner $embossc --generate cc --output-path $out_dir \
  # --import-dir ... --import-dir ... $source
  set(embossc "${dir_pw_third_party_emboss}/embossc")
  set(runner "$ENV{PW_ROOT}/third_party/emboss/embossc_runner.py")

  list(APPEND emboss_cmd python3 "-OO"
    "${runner}" "${embossc}" "--generate" "cc" "--no-cc-enum-traits"
    "--output-path" "${out_dir}")

  foreach(impt IN LISTS abs_import_dirs)
    list(APPEND emboss_cmd "--import-dir" "${impt}")
  endforeach()

  list(APPEND emboss_cmd "${arg_SOURCES}")

  # Define the command to generate $outputs
  add_custom_command(
    COMMAND
      ${emboss_cmd}
    DEPENDS
      ${runner}
      ${arg_SOURCES}
      ${dependent_sources}
    OUTPUT
      ${outputs})
  # Tie a target to $outputs that will trigger the command
  add_custom_target("${NAME}._generate" DEPENDS ${outputs})

  # Export a library that exposes the generated outputs
  pw_add_library_generic("${NAME}" INTERFACE
    GENERATED_HEADERS
      ${outputs}
    PUBLIC_INCLUDES
      "${out_dir}/public"
      "${output_include_path}"
    PUBLIC_DEPS
      pw_third_party.emboss.cpp_utils
      ${arg_DEPS}
  )
  # Tie in the generated outputs as a dep of the library
  add_dependencies("${NAME}" "${NAME}._generate")
endfunction(emboss_cc_library)
