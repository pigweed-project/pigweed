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

use foreign_box::{ForeignRc, ForeignRcState};
use pw_status::{Error, Result};
use time::Instant;

use crate::Kernel;
use crate::object::{KernelObject, ObjectBase, Signals, SyscallBuffer, WaitReturn};
use crate::sync::mutex::Mutex;

struct Transaction<K: Kernel> {
    send_buffer: SyscallBuffer,
    recv_buffer: SyscallBuffer,
    initiator: ForeignRc<K::AtomicUsize, ChannelInitiatorObject<K>>,
}

pub struct ChannelHandlerObject<K: Kernel> {
    base: ObjectBase<K>,
    active_transaction: Mutex<K, Option<Transaction<K>>>,
}

impl<K: Kernel> ChannelHandlerObject<K> {
    pub fn new(kernel: K) -> Self {
        Self {
            base: ObjectBase::new(),
            active_transaction: Mutex::new(kernel, None),
        }
    }
}

impl<K: Kernel> KernelObject<K> for ChannelHandlerObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.base)
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<<K>::Clock>,
    ) -> Result<WaitReturn> {
        self.base.wait_until(kernel, signal_mask, deadline)
    }

    fn channel_read(
        &self,
        _kernel: K,
        offset: usize,
        mut read_buffer: SyscallBuffer,
    ) -> Result<usize> {
        let active_transaction = self.active_transaction.lock();
        let Some(ref transaction) = *active_transaction else {
            return Err(Error::Unavailable);
        };

        transaction.send_buffer.copy_into(offset, &mut read_buffer)
    }

    fn channel_respond(&self, kernel: K, response_buffer: SyscallBuffer) -> Result<()> {
        let mut active_transaction = self.active_transaction.lock();
        let Some(ref mut transaction) = *active_transaction else {
            return Err(Error::Unavailable);
        };
        if response_buffer.size() > transaction.recv_buffer.size() {
            return Err(Error::OutOfRange);
        }
        response_buffer.copy_into(0, &mut transaction.recv_buffer)?;

        transaction.recv_buffer.truncate(response_buffer.size());
        self.base.signal(kernel, |signals| {
            signals - (Signals::READABLE | Signals::WRITEABLE)
        });
        transaction
            .initiator
            .base
            .signal(kernel, |_| Signals::READABLE);
        Ok(())
    }
}

pub struct ChannelInitiatorObject<K: Kernel> {
    base: ObjectBase<K>,
    handler: ForeignRc<K::AtomicUsize, ChannelHandlerObject<K>>,
}

impl<K: Kernel> ChannelInitiatorObject<K> {
    #[must_use]
    pub fn new(handler: ForeignRc<K::AtomicUsize, ChannelHandlerObject<K>>) -> Self {
        Self {
            base: ObjectBase::new(),
            handler,
        }
    }
}

impl<K: Kernel> KernelObject<K> for ChannelInitiatorObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.base)
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<<K>::Clock>,
    ) -> Result<WaitReturn> {
        self.base.wait_until(kernel, signal_mask, deadline)
    }

    fn channel_transact(
        &self,
        kernel: K,
        send_buffer: SyscallBuffer,
        recv_buffer: SyscallBuffer,
        deadline: Instant<K::Clock>,
    ) -> Result<usize> {
        self.start_transaction(kernel, send_buffer, recv_buffer)?;

        // Result processing is deferred until the object is in a coherent state.
        let wait_result = self.object_wait(kernel, Signals::READABLE, deadline);

        // Always clean up the transaction state regardless of wait_result.
        let transaction_result = self.finish_transaction(kernel);

        wait_result?;

        transaction_result
    }

    fn channel_async_transact(
        &self,
        kernel: K,
        send_buffer: SyscallBuffer,
        recv_buffer: SyscallBuffer,
    ) -> Result<()> {
        self.start_transaction(kernel, send_buffer, recv_buffer)
    }

    fn channel_async_transact_complete(&self, kernel: K) -> Result<usize> {
        let active_signals = self.base.state.lock(kernel).active_signals;
        if active_signals.contains(Signals::READABLE) {
            // Transaction completed successfully.
            self.finish_transaction(kernel)
        } else {
            // Transaction is still pending (or doesn't exist).
            Err(Error::Unavailable)
        }
    }

    fn channel_async_cancel(&self, kernel: K) -> Result<()> {
        self.finish_transaction(kernel).map(|_| ())
    }
}

impl<K: Kernel> ChannelInitiatorObject<K> {
    fn start_transaction(
        &self,
        kernel: K,
        send_buffer: SyscallBuffer,
        recv_buffer: SyscallBuffer,
    ) -> Result<()> {
        // TODO: konkers - When the kernel has dynamic memory mapping APIs either:
        // * these checks will have to be differed til the time of memcpy.
        // * a region locking mechanism will need to be built
        // * IPC will be disallowed too/from dynamically mappable memory.

        let self_rc = unsafe { ForeignRcState::create_ref_from_inner(self) };

        let mut active_transaction = self.handler.active_transaction.lock();

        // Check to see if a transaction is already active on the channel.
        if active_transaction.is_some() {
            return Err(Error::Unavailable);
        }

        *active_transaction = Some(Transaction {
            send_buffer,
            recv_buffer,
            initiator: self_rc,
        });

        drop(active_transaction);

        // Clear Readable and Writable signals on our side before
        // signaling the handler.
        self.base.signal(kernel, |signals| {
            signals - (Signals::READABLE | Signals::WRITEABLE)
        });

        self.handler.base.signal(kernel, |_| Signals::READABLE);

        Ok(())
    }

    fn finish_transaction(&self, kernel: K) -> Result<usize> {
        // TODO: konkers - Rationalize signal behavior with syscall_defs.rs.
        // Go back to the writable state now that the transaction is finished.
        self.base.signal(kernel, |signals| {
            (signals | Signals::WRITEABLE) - Signals::READABLE
        });

        // Also reset the handler signals.
        self.handler.base.signal(kernel, |signals| {
            signals - (Signals::READABLE | Signals::WRITEABLE)
        });

        let mut active_transaction = self.handler.active_transaction.lock();

        // All success and error paths reset `active_transaction` to `None`.
        let transaction = active_transaction.take();

        match transaction {
            // The handler has stored the number of response bytes by updating.
            // the recv_buffer length.
            Some(transaction) => Ok(transaction.recv_buffer.size()),

            // Transaction was dropped.
            None => Err(Error::Unavailable),
        }
    }
}
