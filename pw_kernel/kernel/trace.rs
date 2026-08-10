// Copyright 2026 The Pigweed Authors
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

use crate::Kernel;
use crate::scheduler::State;

#[cfg(feature = "tracing")]
#[expect(clippy::cast_possible_truncation)]
fn get_tracepoint_timestamp<K: Kernel>() -> u32 {
    use pw_time_core::Clock;
    // TODO: https://pigweed.dev/issues/477681143 - Add target specific
    // timestamp scaling.
    (K::Clock::now().ticks() >> 8) as u32
}

/// Traces a context switch event.
///
/// This function records a context switch event to the kernel's trace buffer.
pub fn trace_context_switch<K: Kernel>(
    kernel: K,
    current_thread_id: usize,
    new_thread_id: usize,
    current_thread_state: State,
) {
    #[cfg(not(feature = "tracing"))]
    {
        // Silence unused variable warnings when tracing is not enabled.
        let _ = (
            kernel,
            current_thread_id,
            new_thread_id,
            current_thread_state,
        );
    }

    #[cfg(feature = "tracing")]
    #[allow(clippy::cast_possible_truncation)]
    {
        use pw_kernel_tracing::{ContextSwitchEvent, EventType};
        kernel.get_state().trace_buffer.add_record(
            get_tracepoint_timestamp::<K>(),
            EventType::ContextSwitch,
            ContextSwitchEvent {
                // TODO: https://pigweed.dev/issues/477681354 - Support 64bit
                // architectures and their 64 bit thread ids.
                old_thread_id: current_thread_id as u32,
                new_thread_id: new_thread_id as u32,
                old_thread_state: current_thread_state as u8,
            }
            .encode(),
        );
    }
}

/// Starts a trace span with an optional message.
///
/// This function records a span start event to the kernel's trace buffer.
pub fn trace_span_start<K: Kernel>(kernel: K, message: [u8; 12]) {
    #[cfg(not(feature = "tracing"))]
    {
        // Silence unused variable warnings when tracing is not enabled.
        let _ = (kernel, message);
    }

    #[cfg(feature = "tracing")]
    {
        use pw_kernel_tracing::{EventType, TraceSpanEvent};
        kernel.get_state().trace_buffer.add_record(
            get_tracepoint_timestamp::<K>(),
            EventType::SpanStart,
            TraceSpanEvent { message }.encode(),
        );
    }
}

/// Ends a trace span with an optional message.
///
/// This function records a span end event to the kernel's trace buffer.
pub fn trace_span_end<K: Kernel>(kernel: K, message: [u8; 12]) {
    #[cfg(not(feature = "tracing"))]
    {
        // Silence unused variable warnings when tracing is not enabled.
        let _ = (kernel, message);
    }

    #[cfg(feature = "tracing")]
    {
        use pw_kernel_tracing::{EventType, TraceSpanEvent};
        kernel.get_state().trace_buffer.add_record(
            get_tracepoint_timestamp::<K>(),
            EventType::SpanEnd,
            TraceSpanEvent { message }.encode(),
        );
    }
}

/// Starts a trace span with an optional format string and format arguments.
#[macro_export]
macro_rules! trace_span_start {
    ($kernel:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
        #[cfg(feature = "tracing")]
        {
            let mut message = [0u8; 12];
            let _ = $crate::__private::pw_tokenizer::tokenize_core_fmt_to_buffer!(
                &mut message,
                $($format_string)PW_FMT_CONCAT+,
                $($args),*
            );
            $crate::trace::trace_span_start($kernel, message);
        }
        #[cfg(not(feature = "tracing"))]
        {
            #[allow(clippy::unnecessary_cast)]
            let _ = (&$kernel, $($args),*);
        }
    }};
}

/// Ends a trace span with an optional format string and format arguments.
#[macro_export]
macro_rules! trace_span_end {
    ($kernel:expr) => {{
        #[cfg(feature = "tracing")]
        {
            $crate::trace::trace_span_end($kernel, [0u8; 12]);
        }
        #[cfg(not(feature = "tracing"))]
        {
            let _ = &$kernel;
        }
    }};
}

/// Starts a trace span with an optional format string and format arguments if condition is true.
#[macro_export]
macro_rules! trace_span_start_if {
    ($kernel:expr, $condition:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
        if $condition {
            $crate::trace_span_start!($kernel, $($format_string)PW_FMT_CONCAT+, $($args),*);
        } else {
            #[allow(clippy::unnecessary_cast)]
            let _ = (&$kernel, $($args),*);
        }
    }};
}

/// Ends a trace span with an optional format string and format arguments if condition is true.
#[macro_export]
macro_rules! trace_span_end_if {
    ($kernel:expr, $condition:expr) => {{
        if $condition {
            $crate::trace_span_end!($kernel);
        } else {
            let _ = &$kernel;
        }
    }};
}
