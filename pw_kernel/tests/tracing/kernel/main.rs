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
#![no_std]

use kernel::scheduler::State;
use kernel::trace::{trace_context_switch, trace_span_end, trace_span_start};
use kernel::{Kernel, trace_span_end, trace_span_end_if, trace_span_start, trace_span_start_if};
use pw_status::Result;

pub fn main<K: Kernel>(kernel: K) -> Result<()> {
    test_logger::start("Kernel Tracing Test");

    // Generate some synthetic context switch traces between nonexistent threads.
    trace_context_switch(kernel, 1, 2, State::Running);
    trace_context_switch(kernel, 2, 3, State::Ready);
    trace_context_switch(kernel, 0, usize::MAX, State::New);
    trace_context_switch(kernel, 3, 1, State::Terminated);

    // Exercise span start and end functions.
    let message = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    trace_span_start(kernel, message);
    trace_span_end(kernel, message);

    // Test span macros with nested spans.
    trace_span_start!(kernel, "test span");
    trace_span_start!(kernel, "formatted span: {}", 42);
    trace_span_start!(kernel, "multi arg: {} and {}", 1, 2);
    trace_span_end!(kernel);
    trace_span_end!(kernel);
    trace_span_end!(kernel);

    // Test conditional span macros.
    trace_span_start_if!(kernel, true, "conditional start true");
    trace_span_start_if!(kernel, false, "conditional start false");
    trace_span_start_if!(kernel, true, "conditional formatted: {}", 100);
    trace_span_start_if!(kernel, false, "conditional formatted: {}", 200);

    trace_span_end_if!(kernel, true);
    trace_span_end_if!(kernel, false);

    // And one more for balance
    trace_span_end!(kernel);

    test_logger::passed("Kernel Tracing Test");
    Ok(())
}
