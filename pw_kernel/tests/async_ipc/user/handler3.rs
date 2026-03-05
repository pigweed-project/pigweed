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

use app_handler3::handle;
use userspace::entry;

mod common_handler;

#[entry]
fn entry() -> ! {
    if let Err(e) = common_handler::handle_increment_ipc(handle::IPC_3, 3) {
        pw_log::error!("IPC service 3 error: {}", e as u32);
        let _ = userspace::syscall::debug_shutdown(Err(e));
    }

    loop {}
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
