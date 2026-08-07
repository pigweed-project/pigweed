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

use kernel::Kernel;
use kernel::scheduler::State;
use kernel::trace::trace_context_switch;
use pw_status::Result;

pub fn main<K: Kernel>(kernel: K) -> Result<()> {
    test_logger::start("Kernel Tracing Test");

    // Generate some synthetic context switch traces between nonexistent threads.
    trace_context_switch(kernel, 1, 2, State::Running);
    trace_context_switch(kernel, 2, 3, State::Ready);
    trace_context_switch(kernel, 0, usize::MAX, State::New);
    trace_context_switch(kernel, 3, 1, State::Terminated);

    test_logger::passed("Kernel Tracing Test");
    Ok(())
}
