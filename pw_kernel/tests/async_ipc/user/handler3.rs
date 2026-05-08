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

use handler3_codegen::handle;
use pw_status::Result;
use userspace::process_entry;

mod common_handler;

#[process_entry("handler3")]
fn entry() -> Result<()> {
    common_handler::handle_increment_ipc(handle::IPC_3, 3).inspect_err(|e| {
        pw_log::error!("IPC service 3 error: {}", *e as u32);
        let _ = userspace::syscall::debug_shutdown(Err(*e));
    })
}
