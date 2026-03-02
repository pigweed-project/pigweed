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
use crate::scheduler::thread::State;

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
        use time::Clock;
        kernel.get_state().trace_buffer.add_record(
            // TODO: https://pigweed.dev/issues/477681143 - Add target specific
            // timestamp scaling.
            (K::Clock::now().ticks() >> 8) as u32,
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
