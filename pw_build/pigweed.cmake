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
include_guard(GLOBAL)

cmake_minimum_required(VERSION 3.20)

# The PW_ROOT environment variable should be set in bootstrap. If it is not set,
# set it to the root of the Pigweed repository.
if("$ENV{PW_ROOT}" STREQUAL "")
  get_filename_component(pw_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  message("The PW_ROOT environment variable is not set; "
          "using ${pw_root} within CMake")
  set(ENV{PW_ROOT} "${pw_root}")
endif()

# TOOD(ewout, hepler): Remove this legacy include once all users pull in
# pw_unit_test/test.cmake for test functions and variables instead of relying
# on them to be provided by pw_build/pigweed.cmake.
include("$ENV{PW_ROOT}/pw_unit_test/test.cmake")

# Wrapper around cmake_parse_arguments that fails with an error if any arguments
# remained unparsed or a required argument was not provided.
#
# All parsed arguments are prefixed with "arg_". This helper can only be used
# by functions, not macros.
#
# Required Arguments:
#
#   NUM_POSITIONAL_ARGS - PARSE_ARGV <N> arguments for
#                              cmake_parse_arguments
#
# Optional Args:
#
#   OPTION_ARGS - <option> arguments for cmake_parse_arguments
#   ONE_VALUE_ARGS - <one_value_keywords> arguments for cmake_parse_arguments
#   MULTI_VALUE_ARGS - <multi_value_keywords> arguments for
#                           cmake_parse_arguments
#   REQUIRED_ARGS - required arguments which must be set, these may any
#                        argument type (<option>, <one_value_keywords>, and/or
#                        <multi_value_keywords>)
#
macro(pw_parse_arguments)
  if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)  # Remove when CMake 3.31 or newer is required.
  endif()
  cmake_parse_arguments(
    pw_parse_arg "" "NUM_POSITIONAL_ARGS"
    "OPTION_ARGS;ONE_VALUE_ARGS;MULTI_VALUE_ARGS;REQUIRED_ARGS"
    ${ARGN}
  )
  pw_require_args("pw_parse_arguments" "pw_parse_arg_" NUM_POSITIONAL_ARGS)
  if(NOT "${pw_parse_arg_UNPARSED_ARGUMENTS}" STREQUAL "")
    message(FATAL_ERROR "Unexpected arguments to pw_parse_arguments: "
            "${pw_parse_arg_UNPARSED_ARGUMENTS}")
  endif()

  # Now that we have the macro's arguments, process the caller's arguments.
  cmake_parse_arguments(PARSE_ARGV
    "${pw_parse_arg_NUM_POSITIONAL_ARGS}"
    arg
    "${pw_parse_arg_OPTION_ARGS}"
    "${pw_parse_arg_ONE_VALUE_ARGS}"
    "${pw_parse_arg_MULTI_VALUE_ARGS}"
  )
  if(NOT "${arg_UNPARSED_ARGUMENTS}" STREQUAL "")
    set(_all_args
        ${pw_parse_arg_OPTION_ARGS}
        ${pw_parse_arg_ONE_VALUE_ARGS}
        ${pw_parse_arg_MULTI_VALUE_ARGS})
    message(FATAL_ERROR
        "Unexpected arguments to ${CMAKE_CURRENT_FUNCTION}: "
        "${arg_UNPARSED_ARGUMENTS}\n"
        "Valid arguments: ${_all_args}"
    )
  endif()
  pw_require_args("${CMAKE_CURRENT_FUNCTION}" "arg_"
                  ${pw_parse_arg_REQUIRED_ARGS})
endmacro()

# Checks that one or more variables are set. This is used to check that
# arguments were provided to a function. Fails with FATAL_ERROR if
# ${ARG_PREFIX}${name} is empty. The FUNCTION_NAME is used in the error message.
# If FUNCTION_NAME is "", it is set to CMAKE_CURRENT_FUNCTION.
#
# Usage:
#
#   pw_require_args(FUNCTION_NAME ARG_PREFIX ARG_NAME [ARG_NAME ...])
#
# Examples:
#
#   # Checks that arg_FOO is non-empty, using the current function name.
#   pw_require_args("" arg_ FOO)
#
#   # Checks that FOO and BAR are non-empty, using function name "do_the_thing".
#   pw_require_args(do_the_thing "" FOO BAR)
#
macro(pw_require_args FUNCTION_NAME ARG_PREFIX)
  if("${FUNCTION_NAME}" STREQUAL "")
    set(_pw_require_args_FUNCTION_NAME "${CMAKE_CURRENT_FUNCTION}")
  else()
    set(_pw_require_args_FUNCTION_NAME "${FUNCTION_NAME}")
  endif()

  foreach(name IN ITEMS ${ARGN})
    if("${${ARG_PREFIX}${name}}" STREQUAL "")
      message(FATAL_ERROR "A value must be provided for ${name} in "
          "${_pw_require_args_FUNCTION_NAME}.")
    endif()
  endforeach()
endmacro()

# pw_target_link_targets: CMake target only form of target_link_libraries.
#
# Helper wrapper around target_link_libraries which only supports CMake targets
# and detects when the target does not exist.
#
# NOTE: Generator expressions are not supported.
#
# Due to the processing order of list files, the list of targets has to be
# checked at the end of the root CMake list file. Instead of requiring all
# list files to be modified, a DEFER CALL is used.
#
# Required Args:
#
#   <name> - The library target to add the TARGET link dependencies to.
#
# Optional Args:
#
#   INTERFACE - interface target_link_libraries arguments which are all TARGETs.
#   PUBLIC - public target_link_libraries arguments which are all TARGETs.
#   PRIVATE - private target_link_libraries arguments which are all TARGETs.
function(pw_target_link_targets NAME)
  set(types INTERFACE PUBLIC PRIVATE )
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      1
    MULTI_VALUE_ARGS
      ${types}
  )

  if(NOT TARGET "${NAME}")
    message(FATAL_ERROR "\"${NAME}\" must be a TARGET library")
  endif()

  foreach(type IN LISTS types)
    foreach(library IN LISTS arg_${type})
      target_link_libraries(${NAME} ${type} ${library})
      if(NOT TARGET ${library})
        # It's possible the target has not yet been defined due to the ordering
        # of add_subdirectory. Ergo defer the call until the end of the
        # configuration phase.

        # cmake_language(DEFER ...) evaluates arguments at the time the deferred
        # call is executed, ergo wrap it in a cmake_language(EVAL CODE ...) to
        # evaluate the arguments now. The arguments are wrapped in brackets to
        # avoid re-evaluation at the deferred call.
        cmake_language(EVAL CODE
          "cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL
                          _pw_target_link_targets_deferred_check
                          [[${NAME}]] [[${type}]] ${library})"
        )
      endif()
    endforeach()
  endforeach()
endfunction()

# Runs any deferred library checks for pw_target_link_targets.
#
# Makes one exception for the zephyr_interface which is only brought in when
# integrating with the Zephyr build system.
#
# Required Args:
#
#   <name> - The name of the library target to add the link dependencies to.
#   <type> - The type of the library (INTERFACE, PUBLIC, PRIVATE).
#   <library> - The library to check to assert it's a TARGET.
function(_pw_target_link_targets_deferred_check NAME TYPE LIBRARY)
  if(NOT TARGET ${LIBRARY})
      message(FATAL_ERROR
        "${NAME}'s ${TYPE} dep \"${LIBRARY}\" is not a target.")
  endif()
endfunction()

# Sets the provided variable to the multi_value_keywords from pw_add_library.
macro(_pw_add_library_multi_value_args variable)
  set("${variable}" SOURCES HEADERS GENERATED_HEADERS
                    PUBLIC_DEPS PRIVATE_DEPS
                    PUBLIC_INCLUDES PRIVATE_INCLUDES
                    PUBLIC_DEFINES PRIVATE_DEFINES
                    PUBLIC_COMPILE_OPTIONS PRIVATE_COMPILE_OPTIONS
                    PUBLIC_LINK_OPTIONS PRIVATE_LINK_OPTIONS "${ARGN}")
endmacro()

# Internal helper to sandbox a file preserving its relative path.
# Enforces that it is under the current directory.
function(_pw_sandbox_path_relative sandbox_dir path result_var)
  set(parent_dir "..")
  cmake_path(RELATIVE_PATH path OUTPUT_VARIABLE relative_file)

  cmake_path(IS_PREFIX parent_dir "${relative_file}" in_parent_directory)
  if(in_parent_directory)
    message(FATAL_ERROR
      "File paths must be nested under the current directory; "
      "'${path}' is not nested under ${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  if(IS_DIRECTORY "${path}")
    message(DEBUG "Sandboxing include '${path}' as '${sandbox_dir}/${relative_file}'")
    set("${result_var}" "${sandbox_dir}/${relative_file}" PARENT_SCOPE)
  else()
    set(destination "${sandbox_dir}/${relative_file}")
    message(DEBUG "Sandboxing '${path}' as '${destination}'")
    cmake_path(GET destination PARENT_PATH directory)
    file(MAKE_DIRECTORY "${directory}")
    file(CREATE_LINK "${path}" "${destination}" SYMBOLIC)
    set("${result_var}" "${destination}" PARENT_SCOPE)
  endif()
endfunction()

function(_pw_sandbox_relative_paths sandbox_dir paths_var)
  set(result "")
  foreach(path IN LISTS ${paths_var})
    cmake_path(ABSOLUTE_PATH path NORMALIZE)
    _pw_sandbox_path_relative("${sandbox_dir}" "${path}" destination)
    list(APPEND result "${destination}")
  endforeach()
  set("${paths_var}" ${result} PARENT_SCOPE)
endfunction()

function(_pw_sandbox_sources sandbox_dir paths_var)
  set(result "")
  set(source_counter 0)
  set(prefix_maps "")

  foreach(path IN LISTS ${paths_var})
    cmake_path(ABSOLUTE_PATH path NORMALIZE)
    cmake_path(GET path EXTENSION ext)

    if("${ext}" MATCHES "^\\.(c|cc|cpp|cxx|S|s)$")
      math(EXPR source_counter "${source_counter} + 1")
      cmake_path(GET path FILENAME filename)
      set(destination "${sandbox_dir}/sources/src_${source_counter}/${filename}")
      message(DEBUG "Sandboxing source '${path}' as '${destination}'")
      cmake_path(GET destination PARENT_PATH directory)
      file(MAKE_DIRECTORY "${directory}")
      file(CREATE_LINK "${path}" "${destination}" SYMBOLIC)
      list(APPEND result "${destination}")

      cmake_path(GET path PARENT_PATH original_dir)
      list(APPEND prefix_maps "-ffile-prefix-map=${sandbox_dir}/sources/src_${source_counter}/=${original_dir}/")
    elseif("${ext}" MATCHES "^\\.(h|hh|hpp|hxx|inc|inl)$")
      # Fallback for headers listed in SOURCES
      _pw_sandbox_path_relative("${sandbox_dir}" "${path}" destination)
      list(APPEND result "${destination}")
    else()
      message(FATAL_ERROR
        "Unrecognized file extension '${ext}' for source file '${path}'")
    endif()
  endforeach()

  set("${paths_var}" ${result} PARENT_SCOPE)
  set(pw_sandbox_prefix_maps ${prefix_maps} PARENT_SCOPE)
endfunction()

# pw_add_library_generic: Creates a CMake library target.
#
# This is the base helper used to instantiate CMake libraries.
# It should only be used when adding a dependency on a custom library
# from your own project. When adding a dependency on an upstream
# Pigweed module you should use pw_add_library instead.
#
# Required Args:
#
#   <name> - The name of the library target to be created.
#   <type> - The library type which must be INTERFACE, OBJECT, STATIC, or
#            SHARED.
#
# Optional Args:
#
#   SOURCES - source files for this library
#   HEADERS - header files for this library
#   GENERATED_HEADERS - headers that will be generated when the target is built
#   PUBLIC_DEPS - public pw_target_link_targets arguments
#   PRIVATE_DEPS - private pw_target_link_targets arguments
#   PUBLIC_INCLUDES - public target_include_directories argument
#   PRIVATE_INCLUDES - public target_include_directories argument
#   PUBLIC_DEFINES - public target_compile_definitions arguments
#   PRIVATE_DEFINES - private target_compile_definitions arguments
#   PUBLIC_COMPILE_OPTIONS - public target_compile_options arguments
#   PRIVATE_COMPILE_OPTIONS - private target_compile_options arguments
#   PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE - private target_compile_options BEFORE
#     arguments from the specified deps's INTERFACE_COMPILE_OPTIONS. Note that
#     these deps are not pulled in as target_link_libraries. This should not be
#     exposed by the non-generic API.
#   PUBLIC_LINK_OPTIONS - public target_link_options arguments
#   PRIVATE_LINK_OPTIONS - private target_link_options arguments
#   SANDBOX - whether to sandbox this library (ON/OFF); overrides
#     pw_ENABLE_CC_SANDBOX
function(pw_add_library_generic NAME TYPE)
  set(supported_library_types INTERFACE OBJECT STATIC SHARED)
  if(NOT "${TYPE}" IN_LIST supported_library_types)
    message(FATAL_ERROR "\"${TYPE}\" is not a valid library type for ${NAME}. "
          "Must be INTERFACE, OBJECT, STATIC, or SHARED.")
  endif()

  _pw_add_library_multi_value_args(multi_value_args)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      2
    ONE_VALUE_ARGS
      SANDBOX
    MULTI_VALUE_ARGS
      ${multi_value_args}
      PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE
  )

  # CMake 3.22 does not have a notion of target_headers yet, so in the mean
  # time we ask for headers to be specified for consistency with GN & Bazel and
  # to improve the IDE experience. However, we do want to ensure all the headers
  # which are otherwise ignored by CMake are present.
  #
  # See https://gitlab.kitware.com/cmake/cmake/-/issues/22468 for adding support
  # to CMake to associate headers with targets properly for CMake 3.23.
  foreach(header IN ITEMS ${arg_HEADERS})
    get_filename_component(header "${header}" ABSOLUTE)
    if(NOT EXISTS ${header})
      message(FATAL_ERROR "Header not found: \"${header}\"")
    endif()
  endforeach()

  # Applies public or private attributes to a target. This fills a role similar
  # to GN's public_configs and configs, but adds the attributes directly to the
  # target to avoid creating extra internal targets.
  macro(apply_library_config NAME TYPE VISIBILITY)
    target_include_directories(${NAME} ${TYPE} ${arg_${VISIBILITY}_INCLUDES})
    target_compile_definitions(${NAME} ${TYPE} ${arg_${VISIBILITY}_DEFINES})
    target_compile_options(${NAME} ${TYPE} ${arg_${VISIBILITY}_COMPILE_OPTIONS})
    target_link_options(${NAME} ${TYPE} ${arg_${VISIBILITY}_LINK_OPTIONS})
    pw_target_link_targets(${NAME} ${TYPE} ${arg_${VISIBILITY}_DEPS})
  endmacro()

  set(sandbox_dir "${CMAKE_BINARY_DIR}/PIGWEED_SANDBOX/${NAME}")

  # Default sandboxing to the global pw_ENABLE_CC_SANDBOX option.
  if("${arg_SANDBOX}" STREQUAL "")
    set(sandbox_enabled "${pw_ENABLE_CC_SANDBOX}")
  else()
    set(sandbox_enabled "${arg_SANDBOX}")
  endif()

  if(sandbox_enabled)
    foreach(path IN LISTS arg_GENERATED_HEADERS arg_HEADERS arg_SOURCES
        arg_PUBLIC_INCLUDES arg_PRIVATE_INCLUDES)
      # Don't sandbox files that don't exist yet or are generator expressions.
      cmake_path(ABSOLUTE_PATH path OUTPUT_VARIABLE absolute)
      if(NOT EXISTS "${absolute}" OR "${path}" MATCHES ".*\\$<.+>.*")
        if(NOT "${arg_SANDBOX}" STREQUAL "" AND arg_SANDBOX)
          message(FATAL_ERROR "Sandboxing is enabled for ${NAME}, but the path "
            "'${path}' does not exist or is a generator expression. This is "
            "not currently supported.")
        endif()

        message(DEBUG
          "Skipping sandboxing ${NAME} due to generated file '${path}'")
        set(sandbox_enabled OFF)
        break()
      endif()
    endforeach()

    if(sandbox_enabled)
      file(REMOVE_RECURSE "${sandbox_dir}")
      _pw_sandbox_relative_paths("${sandbox_dir}" arg_HEADERS)
      _pw_sandbox_sources("${sandbox_dir}" arg_SOURCES)
      set(sources_prefix_maps ${pw_sandbox_prefix_maps})
      _pw_sandbox_relative_paths("${sandbox_dir}" arg_PRIVATE_INCLUDES)
      _pw_sandbox_relative_paths("${sandbox_dir}" arg_PUBLIC_INCLUDES)
    endif()
  endif()
  list(APPEND arg_HEADERS ${arg_GENERATED_HEADERS})

  if("${TYPE}" STREQUAL "INTERFACE")
    if(NOT "${arg_SOURCES}" STREQUAL "")
      message(
        SEND_ERROR "${NAME} cannot have sources as it's an INTERFACE library"
        "(${arg_SOURCES})")
    endif(NOT "${arg_SOURCES}" STREQUAL "")

    add_library("${NAME}" INTERFACE EXCLUDE_FROM_ALL)
    target_sources("${NAME}" PRIVATE ${arg_HEADERS})
    apply_library_config("${NAME}" INTERFACE PUBLIC)
  elseif(("${TYPE}" STREQUAL "STATIC") OR ("${TYPE}" STREQUAL "SHARED"))
    if("${arg_SOURCES}" STREQUAL "")
      message(
        SEND_ERROR "${NAME} must have SOURCES as it's not an INTERFACE library")
    endif("${arg_SOURCES}" STREQUAL "")

    add_library("${NAME}" "${TYPE}" EXCLUDE_FROM_ALL)
    target_sources("${NAME}" PRIVATE ${arg_HEADERS} ${arg_SOURCES})
    apply_library_config("${NAME}" PUBLIC PUBLIC)
    apply_library_config("${NAME}" PRIVATE PRIVATE)
    if(sandbox_enabled)
      target_compile_options("${NAME}" PRIVATE ${sources_prefix_maps})
    endif()
    foreach(compile_option_dep IN LISTS arg_PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE)
      # This will fail at build time if the target does not exist.
      target_compile_options("${NAME}" BEFORE PRIVATE
          $<TARGET_PROPERTY:${compile_option_dep},INTERFACE_COMPILE_OPTIONS>
      )
    endforeach()
  elseif("${TYPE}" STREQUAL "OBJECT")
    if("${arg_SOURCES}" STREQUAL "")
      message(
        SEND_ERROR "${NAME} must have SOURCES as it's not an INTERFACE library")
    endif("${arg_SOURCES}" STREQUAL "")

    # In order to support OBJECT libraries while maintaining transitive
    # linking dependencies, the library has to be split up into two where the
    # outer interface library forwards not only the internal object library
    # but also its TARGET_OBJECTS.
    add_library("${NAME}._pw_object" OBJECT EXCLUDE_FROM_ALL)
    target_sources("${NAME}._pw_object" PRIVATE ${arg_SOURCES})
    apply_library_config("${NAME}._pw_object" PRIVATE PUBLIC)
    apply_library_config("${NAME}._pw_object" PRIVATE PRIVATE)
    if(sandbox_enabled)
      target_compile_options("${NAME}._pw_object" PRIVATE ${sources_prefix_maps})
    endif()
    foreach(compile_option_dep IN LISTS arg_PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE)
      # This will fail at build time if the target does not exist.
      target_compile_options("${NAME}._pw_object" BEFORE PRIVATE
          $<TARGET_PROPERTY:${compile_option_dep},INTERFACE_COMPILE_OPTIONS>
      )
    endforeach()

    add_library("${NAME}" INTERFACE EXCLUDE_FROM_ALL)
    target_sources("${NAME}" PRIVATE ${arg_HEADERS})
    pw_target_link_targets("${NAME}"
      INTERFACE
        "${NAME}._pw_object"
    )
    apply_library_config("${NAME}" INTERFACE PUBLIC)
    target_link_libraries("${NAME}"
      INTERFACE
        $<TARGET_OBJECTS:${NAME}._pw_object>
    )
  else()
    message(FATAL_ERROR "Unsupported libary type: ${TYPE}")
  endif()
endfunction(pw_add_library_generic)

# Checks that the library's name is prefixed by the relative path with dot
# separators instead of forward slashes. Ignores paths not under the root
# directory.
#
# Optional Args:
#
#   REMAP_PREFIXES - support remapping a prefix for checks
#
function(_pw_check_name_is_relative_to_root NAME ROOT)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      2
    MULTI_VALUE_ARGS
      REMAP_PREFIXES
  )

  file(RELATIVE_PATH rel_path "${ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}")
  if("${rel_path}" MATCHES "^\\.\\.")
    return()  # Ignore paths not under ROOT
  endif()

  list(LENGTH arg_REMAP_PREFIXES remap_arg_count)
  if("${remap_arg_count}" EQUAL 2)
    list(GET arg_REMAP_PREFIXES 0 from_prefix)
    list(GET arg_REMAP_PREFIXES 1 to_prefix)
    string(REGEX REPLACE "^${from_prefix}" "${to_prefix}" rel_path "${rel_path}")
  elseif(NOT "${remap_arg_count}" EQUAL 0)
    message(FATAL_ERROR
        "If REMAP_PREFIXES is specified, exactly two arguments must be given.")
  endif()

  if(NOT "${rel_path}" MATCHES "^\\.\\..*")
    string(REPLACE "/" "." dot_rel_path "${rel_path}")
    if(NOT "${NAME}" MATCHES "^${dot_rel_path}(\\.[^\\.]+)?(\\.facade)?$")
      message(FATAL_ERROR
          "Module libraries under ${ROOT} must match the module name or be in "
          "the form 'PATH_TO.THE_TARGET.NAME'. The library '${NAME}' does not "
          "match. Expected ${dot_rel_path}.LIBRARY_NAME"
      )
    endif()
  endif()
endfunction(_pw_check_name_is_relative_to_root)

# Creates a pw module library.
#
# Required Args:
#
#   <name> - The name of the library target to be created.
#   <type> - The library type which must be INTERFACE, OBJECT, STATIC or SHARED.
#
# Optional Args:
#
#   SOURCES - source files for this library
#   HEADERS - header files for this library
#   PUBLIC_DEPS - public pw_target_link_targets arguments
#   PRIVATE_DEPS - private pw_target_link_targets arguments
#   PUBLIC_INCLUDES - public target_include_directories argument
#   PRIVATE_INCLUDES - public target_include_directories argument
#   PUBLIC_DEFINES - public target_compile_definitions arguments
#   PRIVATE_DEFINES - private target_compile_definitions arguments
#   PUBLIC_COMPILE_OPTIONS - public target_compile_options arguments
#   PRIVATE_COMPILE_OPTIONS - private target_compile_options arguments
#   PUBLIC_LINK_OPTIONS - public target_link_options arguments
#   PRIVATE_LINK_OPTIONS - private target_link_options arguments
#   SANDBOX - whether to sandbox this library (ON/OFF); overrides
#     pw_ENABLE_CC_SANDBOX
#
function(pw_add_library NAME TYPE)
  _pw_add_library_multi_value_args(pw_add_library_generic_multi_value_args)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      2
    ONE_VALUE_ARGS
      SANDBOX
    MULTI_VALUE_ARGS
      ${pw_add_library_generic_multi_value_args}
  )

  _pw_check_name_is_relative_to_root("${NAME}" "$ENV{PW_ROOT}"
    REMAP_PREFIXES
      third_party pw_third_party
  )

  pw_add_library_generic(${NAME} ${TYPE}
    SOURCES
      ${arg_SOURCES}
    HEADERS
      ${arg_HEADERS}
    GENERATED_HEADERS
      ${arg_GENERATED_HEADERS}
    PUBLIC_DEPS
      # TODO: b/232141950 - Apply compilation options that affect ABI
      # globally in the CMake build instead of injecting them into libraries.
      pw_build
      ${arg_PUBLIC_DEPS}
    PRIVATE_DEPS
      ${arg_PRIVATE_DEPS}
    PUBLIC_INCLUDES
      ${arg_PUBLIC_INCLUDES}
    PRIVATE_INCLUDES
      ${arg_PRIVATE_INCLUDES}
    PUBLIC_DEFINES
      ${arg_PUBLIC_DEFINES}
    PRIVATE_DEFINES
      ${arg_PRIVATE_DEFINES}
    PUBLIC_COMPILE_OPTIONS
      ${arg_PUBLIC_COMPILE_OPTIONS}
    PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE
      pw_build.warnings
    PRIVATE_COMPILE_OPTIONS
      ${arg_PRIVATE_COMPILE_OPTIONS}
    PUBLIC_LINK_OPTIONS
      ${arg_PUBLIC_LINK_OPTIONS}
    PRIVATE_LINK_OPTIONS
      ${arg_PRIVATE_LINK_OPTIONS}
    SANDBOX
      ${arg_SANDBOX}
  )
endfunction(pw_add_library)

# Declares a module as a facade.
#
# Facades are declared as two libraries to avoid circular dependencies.
# Libraries that use the facade depend on a library named for the module. The
# module that implements the facade depends on a library named
# MODULE_NAME.facade.
#
# pw_add_facade accepts the same arguments as pw_add_library.
# It also accepts the following argument:
#
#  BACKEND - The name of the facade's backend variable.
function(pw_add_facade NAME TYPE)
  _pw_add_library_multi_value_args(multi_value_args)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      2
    ONE_VALUE_ARGS
      BACKEND
    MULTI_VALUE_ARGS
      ${multi_value_args}
  )

  _pw_check_name_is_relative_to_root("${NAME}" "$ENV{PW_ROOT}"
    REMAP_PREFIXES
      third_party pw_third_party
  )

  pw_add_facade_generic("${NAME}" "${TYPE}"
    BACKEND
      ${arg_BACKEND}
    SOURCES
      ${arg_SOURCES}
    HEADERS
      ${arg_HEADERS}
    PUBLIC_DEPS
      # TODO: b/232141950 - Apply compilation options that affect ABI
      # globally in the CMake build instead of injecting them into libraries.
      pw_build
      ${arg_PUBLIC_DEPS}
    PRIVATE_DEPS
      ${arg_PRIVATE_DEPS}
    PUBLIC_INCLUDES
      ${arg_PUBLIC_INCLUDES}
    PRIVATE_INCLUDES
      ${arg_PRIVATE_INCLUDES}
    PUBLIC_DEFINES
      ${arg_PUBLIC_DEFINES}
    PRIVATE_DEFINES
      ${arg_PRIVATE_DEFINES}
    PUBLIC_COMPILE_OPTIONS
      ${arg_PUBLIC_COMPILE_OPTIONS}
    PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE
      pw_build.warnings
    PRIVATE_COMPILE_OPTIONS
      ${arg_PRIVATE_COMPILE_OPTIONS}
    PUBLIC_LINK_OPTIONS
      ${arg_PUBLIC_LINK_OPTIONS}
    PRIVATE_LINK_OPTIONS
      ${arg_PRIVATE_LINK_OPTIONS}
  )
endfunction(pw_add_facade)

# pw_add_facade_generic: Creates a CMake facade library target.
#
# This is the base helper used to instantiate a facade library.
# It should only be used when adding a dependency on a custom
# facade library from your own project. When adding a dependency
# on an upstream Pigweed facade you should use pw_add_facade instead.
#
# Facades are declared as two libraries to avoid circular dependencies.
# Libraries that use the facade depend on the <name> of this target. The
# libraries that implement this facade have to depend on an internal library
# named <name>.facade.
#
# Required Args:
#
#   <name> - The name for the public facade target (<name>) for all users and
#            the suffixed facade target for backend implementers (<name.facade).
#   <type> - The library type which must be INTERFACE, OBJECT, STATIC, or
#            SHARED.
#   BACKEND - The name of the facade's backend variable.
#
# Optional Args:
#
#   SOURCES - source files for this library
#   HEADERS - header files for this library
#   PUBLIC_DEPS - public pw_target_link_targets arguments
#   PRIVATE_DEPS - private pw_target_link_targets arguments
#   PUBLIC_INCLUDES - public target_include_directories argument
#   PRIVATE_INCLUDES - public target_include_directories argument
#   PUBLIC_DEFINES - public target_compile_definitions arguments
#   PRIVATE_DEFINES - private target_compile_definitions arguments
#   PUBLIC_COMPILE_OPTIONS - public target_compile_options arguments
#   PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE - private target_compile_options BEFORE
#     arguments from the specified deps's INTERFACE_COMPILE_OPTIONS. Note that
#     these deps are not pulled in as target_link_libraries. This should not be
#     exposed by the non-generic API.
#   PRIVATE_COMPILE_OPTIONS - private target_compile_options arguments
#   PUBLIC_LINK_OPTIONS - public target_link_options arguments
#   PRIVATE_LINK_OPTIONS - private target_link_options arguments
function(pw_add_facade_generic NAME TYPE)
  set(supported_library_types INTERFACE OBJECT STATIC SHARED)
  if(NOT "${TYPE}" IN_LIST supported_library_types)
    message(FATAL_ERROR "\"${TYPE}\" is not a valid library type for ${NAME}. "
          "Must be INTERFACE, OBJECT, STATIC, or SHARED.")
  endif()

  _pw_add_library_multi_value_args(multi_value_args)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      2
    ONE_VALUE_ARGS
      BACKEND
    MULTI_VALUE_ARGS
      ${multi_value_args}
      PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE
    REQUIRED_ARGS
      BACKEND
  )

  if(NOT DEFINED "${arg_BACKEND}")
    message(FATAL_ERROR "${NAME}'s backend variable ${arg_BACKEND} has not "
            "been defined, you may be missing a pw_add_backend_variable or "
            "the *.cmake import to that file.")
  endif()
  string(REGEX MATCH ".+_BACKEND" backend_ends_in_backend "${arg_BACKEND}")
  if(NOT backend_ends_in_backend)
    message(FATAL_ERROR "The ${NAME} pw_add_generic_facade's BACKEND argument "
            "(${arg_BACKEND}) must end in _BACKEND (${name_ends_in_backend})")
  endif()

  set(backend_target "${${arg_BACKEND}}")
  if ("${backend_target}" STREQUAL "")
    # If no backend is set, a script that displays an error message is used
    # instead. If the facade is used in the build, it fails with this error.
    pw_add_error_target("${NAME}.NO_BACKEND_SET"
      MESSAGE
        "Attempted to build the ${NAME} facade with no backend set. "
        "Configure the ${NAME} backend using pw_set_backend or remove all "
        "dependencies on it. See https://pigweed.dev/pw_build."
    )

    set(backend_target "${NAME}.NO_BACKEND_SET")
  endif()

  # Define the facade library, which is used by the backend to avoid circular
  # dependencies.
  pw_add_library_generic("${NAME}.facade" INTERFACE
    HEADERS
      ${arg_HEADERS}
    PUBLIC_INCLUDES
      ${arg_PUBLIC_INCLUDES}
    PUBLIC_DEPS
      ${arg_PUBLIC_DEPS}
    PUBLIC_DEFINES
      ${arg_PUBLIC_DEFINES}
    PUBLIC_COMPILE_OPTIONS
      ${arg_PUBLIC_COMPILE_OPTIONS}
    PUBLIC_LINK_OPTIONS
      ${arg_PUBLIC_LINK_OPTIONS}
  )

  # Define the public-facing library for this facade, which depends on the
  # header files and public interface aspects from the .facade target and
  # exposes the dependency on the backend along with the private library
  # target components.
  pw_add_library_generic("${NAME}" "${TYPE}"
    PUBLIC_DEPS
      "${NAME}.facade"
      "${backend_target}"
    SOURCES
      ${arg_SOURCES}
    PRIVATE_INCLUDES
      ${arg_PRIVATE_INCLUDES}
    PRIVATE_DEPS
      ${arg_PRIVATE_DEPS}
    PRIVATE_DEFINES
      ${arg_PRIVATE_DEFINES}
    PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE
      ${arg_PRIVATE_COMPILE_OPTIONS_DEPS_BEFORE}
    PRIVATE_COMPILE_OPTIONS
      ${arg_PRIVATE_COMPILE_OPTIONS}
    PRIVATE_LINK_OPTIONS
      ${arg_PRIVATE_LINK_OPTIONS}
  )
endfunction(pw_add_facade_generic)

# Declare a facade's backend variables which can be overriden later by using
# pw_set_backend.
#
# Required Arguments:
#   NAME - Name of the facade's backend variable.
#
# Optional Arguments:
#   DEFAULT_BACKEND - Optional default backend selection for the facade.
#
function(pw_add_backend_variable NAME)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      1
    ONE_VALUE_ARGS
      DEFAULT_BACKEND
  )

  string(REGEX MATCH ".+_BACKEND" name_ends_in_backend "${NAME}")
  if(NOT name_ends_in_backend)
    message(FATAL_ERROR "The ${NAME} pw_add_backend_variable's NAME argument "
            "must end in _BACKEND")
  endif()

  set("${NAME}" "${arg_DEFAULT_BACKEND}" CACHE STRING
      "${NAME} backend variable for a facade")
endfunction()

# Sets which backend to use for the given facade's backend variable.
function(pw_set_backend NAME BACKEND)
  # TODO(ewout, hepler): Deprecate this temporarily support which permits the
  # direct facade name directly, instead of the facade's backend variable name.
  # Also update this to later assert the variable is DEFINED to catch typos.
  string(REGEX MATCH ".+_BACKEND" name_ends_in_backend "${NAME}")
  if(NOT name_ends_in_backend)
    set(NAME "${NAME}_BACKEND")
  endif()
  if(NOT DEFINED "${NAME}")
    message(WARNING "${NAME} was not defined when pw_set_backend was invoked, "
            "you may be missing a pw_add_backend_variable or the *.cmake "
            "import to that file.")
  endif()

  set("${NAME}" "${BACKEND}" CACHE STRING "backend variable for a facade" FORCE)
endfunction(pw_set_backend)

# Zephyr specific wrapper for pw_set_backend.
function(pw_set_zephyr_backend_ifdef COND FACADE BACKEND BACKEND_DECL)
  if(${${COND}})
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${BACKEND_DECL}")
      message(FATAL_ERROR
          "Can't find backend declaration file '${CMAKE_CURRENT_LIST_DIR}/${BACKEND_DECL}'")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/${BACKEND_DECL}")
    pw_set_backend("${FACADE}" "${BACKEND}")
  endif()
endfunction()

# Zephyr specific wrapper to convert a pw library to a Zephyr library
function(pw_zephyrize_libraries_ifdef COND)
  if(DEFINED Zephyr_FOUND)
    if(${${COND}})
      zephyr_link_libraries(${ARGN})
      foreach(lib ${ARGN})
        target_link_libraries(${lib} INTERFACE zephyr_interface)
      endforeach()
    endif()
  endif()
endfunction()

# Zephyr function allowing conversion of Kconfig values to Pigweed configs
function(pw_set_config_from_zephyr ZEPHYR_CONFIG PW_CONFIG)
  if(${ZEPHYR_CONFIG})
    add_compile_definitions(${PW_CONFIG}=${${ZEPHYR_CONFIG}})
  endif()
endfunction()

# Set up the default pw_build_DEFAULT_MODULE_CONFIG.
set("pw_build_DEFAULT_MODULE_CONFIG" pw_build.empty CACHE STRING
    "Default implementation for all Pigweed module configurations.")

# Declares a module configuration variable for module libraries to depend on.
# Configs should be set to libraries which can be used to provide defines
# directly or though included header files.
#
# The configs can be selected either through the pw_set_module_config function
# to set the pw_build_DEFAULT_MODULE_CONFIG used by default for all Pigweed
# modules or by selecting a specific one for the given NAME'd configuration.
#
# Args:
#
#   NAME: name to use for the target which can be depended on for the config.
function(pw_add_module_config NAME)
  pw_parse_arguments(NUM_POSITIONAL_ARGS 1)

  # Declare the module configuration variable for this module.
  set("${NAME}" "${pw_build_DEFAULT_MODULE_CONFIG}"
      CACHE STRING "Module configuration for ${NAME}")
endfunction(pw_add_module_config)

# Sets which config library to use for the given module.
#
# This can be used to set a specific module configuration or the default
# module configuration used for all Pigweed modules:
#
#   pw_set_module_config(pw_build_DEFAULT_MODULE_CONFIG my_config)
#   pw_set_module_config(pw_foo_CONFIG my_foo_config)
function(pw_set_module_config NAME LIBRARY)
  pw_parse_arguments(NUM_POSITIONAL_ARGS 2)

  # Update the module configuration variable.
  set("${NAME}" "${LIBRARY}" CACHE STRING "Config for ${NAME}" FORCE)
endfunction(pw_set_module_config)

# Adds compiler options to all targets built by CMake. Flags may be added any
# time after this function is defined. The effect is global; all targets added
# before or after a pw_add_global_compile_options call will be built with the
# flags, regardless of where the files are located.
#
# pw_add_global_compile_options takes one optional named argument:
#
#   LANGUAGES: Which languages (ASM, C, CXX) to apply the options to. Flags
#       apply to all languages by default.
#
# All other arguments are interpreted as compiler options.
function(pw_add_global_compile_options)
  cmake_parse_arguments(PARSE_ARGV 0 args "" "" "LANGUAGES")

  set(supported_build_languages ASM C CXX)

  if(NOT args_LANGUAGES)
    set(args_LANGUAGES ${supported_build_languages})
  endif()

  # Check the selected language.
  foreach(lang IN LISTS args_LANGUAGES)
    if(NOT "${lang}" IN_LIST supported_build_languages)
      message(FATAL_ERROR "'${lang}' is not a supported language. "
              "Supported languages: ${supported_build_languages}")
    endif()
  endforeach()

  # Enumerate which flags variables to set.
  foreach(lang IN LISTS args_LANGUAGES)
    list(APPEND cmake_flags_variables "CMAKE_${lang}_FLAGS")
  endforeach()

  # Set each flag for each specified flags variable.
  foreach(variable IN LISTS cmake_flags_variables)
    foreach(flag IN LISTS args_UNPARSED_ARGUMENTS)
      set(${variable} "${${variable}} ${flag}" CACHE INTERNAL "" FORCE)
    endforeach()
  endforeach()
endfunction(pw_add_global_compile_options)

# pw_add_error_target: Creates a CMake target which fails to build and prints a
#                      message
#
# This function prints a message and causes a build failure only if you attempt
# to build the target. This is useful when FATAL_ERROR messages cannot be used
# to catch problems during the CMake configuration phase.
#
# Args:
#
#   NAME: name to use for the target
#   MESSAGE: The message to print, prefixed with "ERROR: ". The message may be
#            composed of multiple pieces by passing multiple strings.
#
function(pw_add_error_target NAME)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS
      1
    MULTI_VALUE_ARGS
      MESSAGE
  )

  # In case the message is comprised of multiple strings, stitch them together.
  set(message "ERROR: ")
  foreach(line IN LISTS arg_MESSAGE)
    string(APPEND message "${line}")
  endforeach()

  add_custom_target("${NAME}._error_message"
    COMMAND
      "${CMAKE_COMMAND}" -E echo "${message}"
    COMMAND
      "${CMAKE_COMMAND}" -E false
  )

  # A static library is provided, in case this rule nominally provides a
  # compiled output, e.g. to enable $<TARGET_FILE:"${NAME}">.
  pw_add_library_generic("${NAME}" STATIC
    SOURCES
      $<TARGET_PROPERTY:pw_build.empty,SOURCES>
  )
  add_dependencies("${NAME}" "${NAME}._error_message")
endfunction(pw_add_error_target)

# Rebases a set of files to a new root path and optionally appends extensions
# to them. This is particularly useful for file generators.
#
# Required Arguments:
#
#   <var>        - Variable to store the rebased file list in.
#   <new_root>   - The new root to rebase file paths onto.
#   <root>       - The current root to rebase off of.
#   <files>      - The list of files to rebase.
#   <extensions> - List of extensions to replace the existing file extensions
#                  with.
#
# Examples:
#
# list(APPEND files "public/proj/foo.def" "public/proj/bar.def")
#
# pw_rebase_paths(out_files "/tmp" "${CMAKE_CURRENT_SOURCE_DIR}/public"
#   ${files} "")
# out_files => [ "/tmp/proj/foo.def", "/tmp/proj/bar.def" ]
#
# pw_rebase_paths(out_files "/tmp" "${CMAKE_CURRENT_SOURCE_DIR}/public"
#   ${files} ".h")
# out_files => [ "/tmp/proj/foo.h", "/tmp/proj/bar.h" ]
#
# list (APPEND exts ".h" ".cc")
# pw_rebase_paths(out_files "/tmp" "${CMAKE_CURRENT_SOURCE_DIR}/public"
#   ${files} ${exts})
# out_files => [ "/tmp/proj/foo.h", "/tmp/proj/bar.h",
#                "/tmp/proj/foo.cc", "/tmp/proj/bar.cc" ]
function(pw_rebase_paths VAR NEW_ROOT ROOT FILES EXTENSIONS)
  foreach(file IN LISTS FILES)
    get_filename_component(file "${file}" ABSOLUTE)
    file(RELATIVE_PATH file "${ROOT}" "${file}")

    if("${EXTENSIONS}" STREQUAL "")
      list(APPEND mirrored_files "${NEW_ROOT}/${file}")
    else()
      foreach(ext IN LISTS EXTENSIONS)
        get_filename_component(dir "${file}" DIRECTORY)
        get_filename_component(name "${file}" NAME_WE)
        list(APPEND mirrored_files "${NEW_ROOT}/${dir}/${name}${ext}")
      endforeach()
    endif()
  endforeach()

  set("${VAR}"
    "${mirrored_files}"
    PARENT_SCOPE)
endfunction(pw_rebase_paths)
