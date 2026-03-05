// Copyright 2025 The Pigweed Authors
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

use pw_status::{Error, Result};
use userspace::syscall::{self, Signals};
use userspace::time::Instant;

pub fn handle_increment_ipc(ipc_handle: u32, increment: u8) -> Result<()> {
    loop {
        // Wait for an IPC to come in.
        let wait_return = syscall::object_wait(ipc_handle, Signals::READABLE, Instant::MAX)
            .map_err(|_| Error::Internal)?;

        if !wait_return.pending_signals.contains(Signals::READABLE) || wait_return.user_data != 0 {
            return Err(Error::Internal);
        }

        let mut buffer = [0u8; 1];
        match syscall::channel_read(ipc_handle, 0, &mut buffer) {
            Ok(len) => {
                if len != 1 {
                    return Err(Error::OutOfRange);
                }
                buffer[0] = buffer[0].wrapping_add(increment);
                syscall::channel_respond(ipc_handle, &buffer)?;
            }
            // Transaction was cancelled by initiator while we were waking up.
            // Just go back to waiting for the next one.
            Err(Error::Unavailable) => continue,
            Err(e) => return Err(e),
        }
    }
}
