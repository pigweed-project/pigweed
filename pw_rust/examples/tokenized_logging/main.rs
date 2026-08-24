// Copyright 2023 The Pigweed Authors
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

//! An example demonstrating how to use the `pw_log` modules and set up a
//! tokenized backend.
//!
//! See <https://pigweed.dev/pw_rust/> for instructions on how to build this
//! example.  See the `pw_log_backend` crate in this directory for details on
//! how the tokenized backend is setup.
#![no_main]
#![no_std]

// Panic handler that halts the CPU on panic.
use cortex_m as _;
use cortex_m_rt::entry;
// Semihosting support which is well supported for QEMU targets.
use cortex_m_semihosting::{debug, hprintln};
use panic_halt as _;
use pw_log::{critical, infof, warnf};

#[entry]
fn main() -> ! {
    // Plain text printout without `pw_log`
    hprintln!("Hello, Pigweed!");

    // printf style `pw_log` messages
    infof!("Bare string");
    warnf!("Integer value %d", 42);

    // core::fmt style `pw_log` messages
    //
    // Note the support for this is in progress with the following notes:
    // * only `u32`, `i32`, and '&str' arguments are supported right now.
    // * arguments must be annotated with `as <type>` to work with `stable`
    //   toolchains.
    //
    // Both of these will be addressed in the future.
    critical!(
        "Generic rusty arguments: 0x{:08x} {}",
        0xaa as u32,
        -42 as i32
    );

    debug::exit(debug::EXIT_SUCCESS);
    loop {}
}
