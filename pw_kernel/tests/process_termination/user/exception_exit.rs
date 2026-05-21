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
#![no_main]

use pw_status::{Error, Result};
use userspace::process_entry;

#[process_entry("exception_exit")]
fn main() -> Result<()> {
    test_logger::info!("I am the exception exit process. Triggering exception...");
    unsafe {
        core::ptr::null_mut::<u32>().write_volatile(42);
    }
    test_logger::error!("Exception trigger FAILED!");
    Err(Error::Internal)
}
