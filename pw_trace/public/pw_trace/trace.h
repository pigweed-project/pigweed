// Copyright 2020 The Pigweed Authors
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
//==============================================================================
//
// This file describes Pigweed's public user-facing tracing API.
//
// THIS PUBLIC API IS NOT STABLE OR COMPLETE!
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pw_preprocessor/util.h"              // IWYU pragma: export
#include "pw_trace/internal/trace_internal.h"  // IWYU pragma: export

// Backend defines PW_TRACE in the form:
// #define PW_TRACE(event_type, flags, label, group_label, trace_id)

// trace_backend.h must ultimately resolve to a header that implements the
// macros required by the tracing facade, as described below.
//
// Inputs: Macros the downstream user provides to control the tracing system:
//
//   PW_TRACE_MODULE_NAME
//     - The module name the backend should use
//
// Outputs: Macros trace_backend.h is expected to provide:
//
//   PW_TRACE(event_type, flags, label, group_label, trace_id)
//   PW_TRACE_DATA(event_type, flags, label, group_label, trace_id,
//                 data_format_string, data, data_size)
//      - Implementing PW_TRACE_DATA is optional. If not defined, all data
//        traces will be removed.
//
//   event_type will pass the macro value which is defined by the backend when
//   enabling the trace type.
//
// Enabling traces: The backend can define these macros to enable the different
//                  trace types. If not defined those traces are removed.
//
//   PW_TRACE_TYPE_INSTANT: Instant trace, with only a label.
//   PW_TRACE_TYPE_INSTANT_GROUP: Instant trace, with a label and a group.
//   PW_TRACE_TYPE_DURATION_START: Start trace, with only a label.
//   PW_TRACE_TYPE_DURATION_END: End trace, with only a label.
//   PW_TRACE_TYPE_DURATION_GROUP_START: Start trace, with a label and a group.
//   PW_TRACE_TYPE_DURATION_GROUP_END: End trace, with a label and a group.
//   PW_TRACE_TYPE_ASYNC_START: Start trace, with label, group, and trace_id.
//   PW_TRACE_TYPE_ASYNC_INSTANT: Instant trace, with label, group, and trace_id
//   PW_TRACE_TYPE_ASYNC_END: End trace, with label, group, and trace_id.
//
// Defaults: The backend can use the macros to change what the default value is
//           if not provided.
//
//   PW_TRACE_FLAGS_DEFAULT: Default value if no flags are provided.
//   PW_TRACE_TRACE_ID_DEFAULT: Default value if not trace_id provided.
//   PW_TRACE_GROUP_LABEL_DEFAULT: Default value if not group_label provided.

#include "pw_trace_backend/trace_backend.h"  // IWYU pragma: export

/// @module{pw_trace}

/// The module name that the backend associates with the trace events in the
/// current file. Traces can be grouped into modules, similar to logging, by
/// defining this macro before including `pw_trace/trace.h`:
///
/// @code{.cpp}
/// #define PW_TRACE_MODULE_NAME "Input"
/// #include "pw_trace/trace.h"
/// @endcode
///
/// Defaults to an empty string.
#ifndef PW_TRACE_MODULE_NAME
#define PW_TRACE_MODULE_NAME ""
#endif  // PW_TRACE_MODULE_NAME

/// The flags value used by the trace macros that don't take an explicit
/// `flag` argument, such as `PW_TRACE_INSTANT`. Defaults to
/// `PW_TRACE_FLAGS_DEFAULT`.
#ifndef PW_TRACE_FLAGS
#define PW_TRACE_FLAGS PW_TRACE_FLAGS_DEFAULT
#endif  // PW_TRACE_FLAGS

/// Traces an instantaneous event, which has no duration.
///
/// Overloads:
/// - `PW_TRACE_INSTANT(label)`
/// - `PW_TRACE_INSTANT(label, group_label)`
/// - `PW_TRACE_INSTANT(label, group_label, trace_id)`
///
/// Arguments:
/// - `label` - A string literal which describes the trace.
/// - `group_label` (optional) - A string literal which groups this trace with
/// others in the same module and group.
/// - `trace_id` (optional) - A runtime `uint32_t` which groups this trace with
/// others with the same module, group, and trace ID. Every trace with a
/// `trace_id` must also have a `group_label`.
///
/// Example:
/// @code{.cpp}
/// PW_TRACE_INSTANT("HERE");
/// @endcode
#define PW_TRACE_INSTANT(...) PW_TRACE_INSTANT_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_INSTANT`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_INSTANT_FLAG(flag, label)`
/// - `PW_TRACE_INSTANT_FLAG(flag, label, group_label)`
/// - `PW_TRACE_INSTANT_FLAG(flag, label, group_label, trace_id)`
#define PW_TRACE_INSTANT_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_INSTANT_ARGS, __VA_ARGS__)

/// Same as `PW_TRACE_INSTANT`, but appends a user-supplied data buffer to the
/// trace event.
///
/// Overloads:
/// - `PW_TRACE_INSTANT_DATA(label, data_format_string, data, size)`
/// - `PW_TRACE_INSTANT_DATA(label, group_label, data_format_string, data,
/// size)`
/// - `PW_TRACE_INSTANT_DATA(label, group_label, trace_id, data_format_string,
/// data, size)`
///
/// Arguments:
/// - `data_format_string` - A string which is used by the decoder to identify
/// the data. This could for example be printf-style tokens, a Python struct
/// packed format string, or a custom label recognized by the decoder.
/// - `data` - A pointer to a buffer of arbitrary caller-provided data
/// (`const void*`).
/// - `size` - The size of the data (`size_t`).
#define PW_TRACE_INSTANT_DATA(...) \
  PW_TRACE_INSTANT_DATA_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_INSTANT_DATA`, but uses the provided `flag` value instead
/// of `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_INSTANT_DATA_FLAG(flag, label, data_format_string, data, size)`
/// - `PW_TRACE_INSTANT_DATA_FLAG(flag, label, group_label, data_format_string,
/// data, size)`
/// - `PW_TRACE_INSTANT_DATA_FLAG(flag, label, group_label, trace_id,
/// data_format_string, data, size)`
#define PW_TRACE_INSTANT_DATA_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_INSTANT_DATA_ARGS, __VA_ARGS__)

/// Starts tracing a duration event. Must be paired with a `PW_TRACE_END` (or
/// `PW_TRACE_END_DATA`) with the same module, `label`, `group_label`, and
/// `trace_id`.
///
/// Overloads:
/// - `PW_TRACE_START(label)`
/// - `PW_TRACE_START(label, group_label)`
/// - `PW_TRACE_START(label, group_label, trace_id)`
///
/// Arguments:
/// - `label` - A string literal which describes the trace.
/// - `group_label` (optional) - A string literal which groups this trace with
/// others in the same module and group.
/// - `trace_id` (optional) - A runtime `uint32_t` which groups this trace with
/// others with the same module, group, and trace ID. Every trace with a
/// `trace_id` must also have a `group_label`.
///
/// Example:
/// @code{.cpp}
/// PW_TRACE_START("label");
/// // Do something.
/// PW_TRACE_END("label");
/// @endcode
#define PW_TRACE_START(...) PW_TRACE_START_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_START`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_START_FLAG(flag, label)`
/// - `PW_TRACE_START_FLAG(flag, label, group_label)`
/// - `PW_TRACE_START_FLAG(flag, label, group_label, trace_id)`
#define PW_TRACE_START_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_START_ARGS, __VA_ARGS__)

/// Same as `PW_TRACE_START`, but appends a user-supplied data buffer to the
/// trace event.
///
/// Overloads:
/// - `PW_TRACE_START_DATA(label, data_format_string, data, size)`
/// - `PW_TRACE_START_DATA(label, group_label, data_format_string, data, size)`
/// - `PW_TRACE_START_DATA(label, group_label, trace_id, data_format_string,
/// data, size)`
///
/// Arguments:
/// - `data_format_string` - A string which is used by the decoder to identify
/// the data. This could for example be printf-style tokens, a Python struct
/// packed format string, or a custom label recognized by the decoder.
/// - `data` - A pointer to a buffer of arbitrary caller-provided data
/// (`const void*`).
/// - `size` - The size of the data (`size_t`).
///
/// @note A trace duration start/end can be combined with a duration data
/// start/end, to include data at only one of the trace points and not the
/// other.
#define PW_TRACE_START_DATA(...) \
  PW_TRACE_START_DATA_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_START_DATA`, but uses the provided `flag` value instead
/// of `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_START_DATA_FLAG(flag, label, data_format_string, data, size)`
/// - `PW_TRACE_START_DATA_FLAG(flag, label, group_label, data_format_string,
/// data, size)`
/// - `PW_TRACE_START_DATA_FLAG(flag, label, group_label, trace_id,
/// data_format_string, data, size)`
#define PW_TRACE_START_DATA_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_START_DATA_ARGS, __VA_ARGS__)

/// Ends tracing a duration event. Must be paired with a `PW_TRACE_START` (or
/// `PW_TRACE_START_DATA`) with the same module, `label`, `group_label`, and
/// `trace_id`.
///
/// Overloads:
/// - `PW_TRACE_END(label)`
/// - `PW_TRACE_END(label, group_label)`
/// - `PW_TRACE_END(label, group_label, trace_id)`
///
/// Arguments:
/// - `label` - A string literal which describes the trace.
/// - `group_label` (optional) - A string literal which groups this trace with
/// others in the same module and group.
/// - `trace_id` (optional) - A runtime `uint32_t` which groups this trace with
/// others with the same module, group, and trace ID. Every trace with a
/// `trace_id` must also have a `group_label`.
///
/// Example:
/// @code{.cpp}
/// PW_TRACE_START("label");
/// // Do something.
/// PW_TRACE_END("label");
/// @endcode
#define PW_TRACE_END(...) PW_TRACE_END_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_END`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_END_FLAG(flag, label)`
/// - `PW_TRACE_END_FLAG(flag, label, group_label)`
/// - `PW_TRACE_END_FLAG(flag, label, group_label, trace_id)`
#define PW_TRACE_END_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_END_ARGS, __VA_ARGS__)

/// Same as `PW_TRACE_END`, but appends a user-supplied data buffer to the
/// trace event.
///
/// Overloads:
/// - `PW_TRACE_END_DATA(label, data_format_string, data, size)`
/// - `PW_TRACE_END_DATA(label, group_label, data_format_string, data, size)`
/// - `PW_TRACE_END_DATA(label, group_label, trace_id, data_format_string, data,
/// size)`
///
/// Arguments:
/// - `data_format_string` - A string which is used by the decoder to identify
/// the data. This could for example be printf-style tokens, a Python struct
/// packed format string, or a custom label recognized by the decoder.
/// - `data` - A pointer to a buffer of arbitrary caller-provided data
/// (`const void*`).
/// - `size` - The size of the data (`size_t`).
///
/// @note A trace duration start/end can be combined with a duration data
/// start/end, to include data at only one of the trace points and not the
/// other.
#define PW_TRACE_END_DATA(...) \
  PW_TRACE_END_DATA_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)

/// Same as `PW_TRACE_END_DATA`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional.
///
/// Overloads:
/// - `PW_TRACE_END_DATA_FLAG(flag, label, data_format_string, data, size)`
/// - `PW_TRACE_END_DATA_FLAG(flag, label, group_label, data_format_string,
/// data, size)`
/// - `PW_TRACE_END_DATA_FLAG(flag, label, group_label, trace_id,
/// data_format_string, data, size)`
#define PW_TRACE_END_DATA_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_END_DATA_ARGS, __VA_ARGS__)

#ifdef __cplusplus

/// Traces the duration of the current scope. Emits a start event where the
/// macro appears in code and an end event when the enclosing scope exits. This
/// provides a convenient method of tracing functions or loops. C++ only.
///
/// Overloads:
/// - `PW_TRACE_SCOPE(label)`
/// - `PW_TRACE_SCOPE(label, group_label)`
/// - `PW_TRACE_SCOPE(label, group_label, trace_id)`
///
/// Arguments:
/// - `label` - A string literal which describes the trace.
/// - `group_label` (optional) - A string literal which groups this trace with
/// others in the same module and group.
/// - `trace_id` (optional) - A runtime `uint32_t` which groups this trace with
/// others with the same module, group, and trace ID. Every trace with a
/// `trace_id` must also have a `group_label`.
///
/// Example:
/// @code{.cpp}
/// {
///   PW_TRACE_SCOPE("Bar");
///   // Do some stuff.
/// }
///
/// {
///   PW_TRACE_SCOPE("Foo", "Group");
///   {
///     PW_TRACE_SCOPE("SubFoo", "Group");
///     // Do some stuff.
///   }
///   // Do some stuff.
/// }
/// @endcode
///
/// Which can be visualized as:
/// @code{.none}
/// Bar:   [----------------Bar----------------]
/// Group: [----------------Foo----------------]
///                   [------SubFoo-------]
/// @endcode
#ifndef PW_TRACE_SCOPE
#define PW_TRACE_SCOPE(...) PW_TRACE_SCOPE_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)
#endif  // PW_TRACE_SCOPE

/// Same as `PW_TRACE_SCOPE`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional. C++ only.
///
/// Overloads:
/// - `PW_TRACE_SCOPE_FLAG(flag, label)`
/// - `PW_TRACE_SCOPE_FLAG(flag, label, group_label)`
/// - `PW_TRACE_SCOPE_FLAG(flag, label, group_label, trace_id)`
#ifndef PW_TRACE_SCOPE_FLAG
#define PW_TRACE_SCOPE_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_SCOPE_ARGS, __VA_ARGS__)
#endif  // PW_TRACE_SCOPE_FLAG

/// Traces the duration of the current function. This is the same as
/// `PW_TRACE_SCOPE`, but uses the function name (`PW_TRACE_FUNCTION_LABEL`) as
/// the label. This is the only trace macro which does not require a `label`.
/// C++ only.
///
/// Overloads:
/// - `PW_TRACE_FUNCTION()`
/// - `PW_TRACE_FUNCTION(group_label)`
/// - `PW_TRACE_FUNCTION(group_label, trace_id)`
///
/// Arguments:
/// - `group_label` (optional) - A string literal which groups this trace with
/// others in the same module and group.
/// - `trace_id` (optional) - A runtime `uint32_t` which groups this trace with
/// others with the same module, group, and trace ID. Every trace with a
/// `trace_id` must also have a `group_label`.
///
/// Example:
/// @code{.cpp}
/// void Bar() {
///   PW_TRACE_FUNCTION();
///   // Do some stuff.
/// }
///
/// void Child() {
///   PW_TRACE_FUNCTION("Group");
///   // Do some stuff.
/// }
///
/// void Parent() {
///   PW_TRACE_FUNCTION("Group");
///   // Do some stuff.
///   Child();
/// }
/// @endcode
///
/// Which can be visualized as:
/// @code{.none}
/// Bar:   [----------------Bar----------------]
/// Group: [----------------Parent----------------]
///                   [------Child-------]
/// @endcode
#ifndef PW_TRACE_FUNCTION
#define PW_TRACE_FUNCTION(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_FUNCTION_ARGS, __VA_ARGS__)
//  PW_TRACE_FUNCTION_FLAG(PW_TRACE_FLAGS, __VA_ARGS__)
#endif  // PW_TRACE_FUNCTION

/// Same as `PW_TRACE_FUNCTION`, but uses the provided `flag` value instead of
/// `PW_TRACE_FLAGS`. The flag goes at the start; the `group_label` and
/// `trace_id` arguments are still optional. C++ only.
///
/// Overloads:
/// - `PW_TRACE_FUNCTION_FLAG(flag)`
/// - `PW_TRACE_FUNCTION_FLAG(flag, group_label)`
/// - `PW_TRACE_FUNCTION_FLAG(flag, group_label, trace_id)`
#ifndef PW_TRACE_FUNCTION_FLAG
#define PW_TRACE_FUNCTION_FLAG(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TRACE_FUNCTION_FLAGS_ARGS, __VA_ARGS__)
#endif  // PW_TRACE_FUNCTION_FLAG

#endif  // __cplusplus

/// @endmodule
