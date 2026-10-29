// Copyright 2021 The Pigweed Authors
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

#include <stddef.h>
#include <stdint.h>

#include "pw_preprocessor/compiler.h"
#include "pw_preprocessor/util.h"

/// @module{pw_trace}

/// Enables instant trace events with only a label. The backend must define
/// this macro to enable this trace type; if left undefined, traces of this
/// type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_INSTANT 1
/// Enables instant trace events with a label and a group. The backend must
/// define this macro to enable this trace type; if left undefined, traces of
/// this type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_INSTANT_GROUP 1
/// Enables start trace events with only a label. The backend must define this
/// macro to enable this trace type; if left undefined, traces of this type are
/// removed. The macro's value is passed to `PW_TRACE` and `PW_TRACE_DATA` as
/// the `event_type` argument.
#define PW_TRACE_TYPE_DURATION_START 1
/// Enables end trace events with only a label. The backend must define this
/// macro to enable this trace type; if left undefined, traces of this type are
/// removed. The macro's value is passed to `PW_TRACE` and `PW_TRACE_DATA` as
/// the `event_type` argument.
#define PW_TRACE_TYPE_DURATION_END 1
/// Enables start trace events with a label and a group. The backend must
/// define this macro to enable this trace type; if left undefined, traces of
/// this type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_DURATION_GROUP_START 1
/// Enables end trace events with a label and a group. The backend must define
/// this macro to enable this trace type; if left undefined, traces of this
/// type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_DURATION_GROUP_END 1
/// Enables start trace events with a label, group, and trace ID. The backend
/// must define this macro to enable this trace type; if left undefined, traces
/// of this type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_ASYNC_START 1
/// Enables instant trace events with a label, group, and trace ID. The backend
/// must define this macro to enable this trace type; if left undefined, traces
/// of this type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_ASYNC_INSTANT 1
/// Enables end trace events with a label, group, and trace ID. The backend
/// must define this macro to enable this trace type; if left undefined, traces
/// of this type are removed. The macro's value is passed to `PW_TRACE` and
/// `PW_TRACE_DATA` as the `event_type` argument.
#define PW_TRACE_TYPE_ASYNC_END 1

/// @endmodule

PW_EXTERN_C_START

// Empty function for compiling out trace statements. Since the function is
// empty and inline, it should be completely compiled out. This function
// accomplishes following:
//
//   - Uses the arguments to PW_TRACE, which avoids "unused variable" warnings.
//   - Executes expressions passed to PW_TRACE, so that the behavior is
//     consistent between this null backend and an actual backend.
//
// These two functions are used in PW_TRACE and PW_TRACE_DATA.

static inline void _pw_trace_Ignored(int event_type,
                                     uint8_t flags,
                                     const char* label,
                                     const char* group,
                                     uint32_t trace_id) {
  (void)event_type;
  (void)flags;
  (void)label;
  (void)group;
  (void)trace_id;
}

static inline void _pw_trace_IgnoredData(int event_type,
                                         uint8_t flags,
                                         const char* label,
                                         const char* group,
                                         uint32_t trace_id,
                                         const char* type,
                                         const void* data,
                                         size_t size) {
  (void)event_type;
  (void)flags;
  (void)label;
  (void)group;
  (void)trace_id;
  (void)type;
  (void)data;
  (void)size;
}

PW_EXTERN_C_END

/// @module{pw_trace}

/// Records a trace event. Every `pw_trace` backend must implement this macro.
/// The facade invokes it from each of the trace event macros, such as
/// `PW_TRACE_INSTANT`, `PW_TRACE_START`, and `PW_TRACE_END`.
///
/// @param[in] event_type The value of the `PW_TRACE_TYPE_*` macro that the
/// backend defined for this trace type, such as `PW_TRACE_TYPE_INSTANT`.
/// @param[in] flags The flags value for the event.
/// @param[in] label A string literal which describes the trace.
/// @param[in] group The group label: a string literal which groups this trace
/// with others in the same module and group, or `PW_TRACE_GROUP_LABEL_DEFAULT`.
/// @param[in] trace_id A runtime `uint32_t` which groups this trace with others
/// with the same module, group, and trace ID, or `PW_TRACE_TRACE_ID_DEFAULT`.
#define PW_TRACE(event_type, flags, label, group, trace_id) \
  _pw_trace_Ignored(event_type, flags, label, group, trace_id)

/// Records a trace event with an attached user-supplied data buffer. The
/// facade invokes it from each of the `*_DATA` trace event macros, such as
/// `PW_TRACE_INSTANT_DATA`. Implementing this macro is optional; if the
/// backend does not define it, all data trace events are removed.
///
/// @param[in] event_type The value of the `PW_TRACE_TYPE_*` macro that the
/// backend defined for this trace type, such as `PW_TRACE_TYPE_INSTANT`.
/// @param[in] flags The flags value for the event.
/// @param[in] label A string literal which describes the trace.
/// @param[in] group The group label: a string literal which groups this trace
/// with others in the same module and group, or `PW_TRACE_GROUP_LABEL_DEFAULT`.
/// @param[in] trace_id A runtime `uint32_t` which groups this trace with others
/// with the same module, group, and trace ID, or `PW_TRACE_TRACE_ID_DEFAULT`.
/// @param[in] type The data format string: a string which is used by the
/// decoder to identify the data.
/// @param[in] data A pointer to a buffer of arbitrary caller-provided data
/// (`const void*`).
/// @param[in] size The size of the data (`size_t`).
#define PW_TRACE_DATA(                                           \
    event_type, flags, label, group, trace_id, type, data, size) \
  _pw_trace_IgnoredData(                                         \
      event_type, flags, label, group, trace_id, type, data, size)

/// @endmodule
