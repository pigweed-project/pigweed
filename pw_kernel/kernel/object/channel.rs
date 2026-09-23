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

use foreign_box::ForeignRc;
use pw_status::{Error, Result};
use pw_time_core::Instant;

use crate::object::{KernelObject, ObjectBase, SignalUpdate, Signals, SyscallBuffer, WaitReturn};
use crate::sync::mutex::Mutex;
use crate::{Arch, Kernel};

struct Transaction {
    send_buffer: SyscallBuffer,
    recv_buffer: SyscallBuffer,
}

/// The shared state of a channel.
///
/// Both endpoint objects hold a `ForeignRc` to one of these.  The endpoints do
/// not reference each other.
///
/// `initiator_base` and `handler_base` each carry an independent [`ObjectBase`]
/// lock and can be reached from either endpoint.  Never hold both locks at the
/// same time, as initiator and handler paths would acquire them in opposite
/// orders.
pub struct Channel<K: Kernel> {
    initiator_base: ObjectBase<K>,
    handler_base: ObjectBase<K>,
    active_transaction: Mutex<K, Option<Transaction>>,
}

impl<K: Kernel> Channel<K> {
    #[must_use]
    pub fn new(kernel: K) -> Self {
        Self {
            initiator_base: ObjectBase::new(Signals::WRITEABLE),
            handler_base: ObjectBase::new(Signals::no_active()),
            active_transaction: Mutex::new(kernel, None),
        }
    }
}

/// The handler endpoint of a [`Channel`].
pub struct ChannelHandlerObject<K: Kernel> {
    channel: ForeignRc<<K as Arch>::AtomicUsize, Channel<K>>,
}

impl<K: Kernel> ChannelHandlerObject<K> {
    #[must_use]
    pub fn new(channel: ForeignRc<<K as Arch>::AtomicUsize, Channel<K>>) -> Self {
        Self { channel }
    }
}

impl<K: Kernel> KernelObject<K> for ChannelHandlerObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.channel.handler_base)
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<<K>::Clock>,
    ) -> Result<WaitReturn> {
        self.channel
            .handler_base
            .wait_until(kernel, signal_mask, deadline)
    }

    fn channel_read(
        &self,
        _kernel: K,
        offset: usize,
        mut read_buffer: SyscallBuffer,
    ) -> Result<usize> {
        let active_transaction = self.channel.active_transaction.lock();
        let Some(ref transaction) = *active_transaction else {
            return Err(Error::Unavailable);
        };

        transaction.send_buffer.copy_into(offset, &mut read_buffer)
    }

    fn channel_respond(&self, kernel: K, response_buffer: SyscallBuffer) -> Result<()> {
        let mut active_transaction = self.channel.active_transaction.lock();
        let Some(ref mut transaction) = *active_transaction else {
            return Err(Error::Unavailable);
        };
        if response_buffer.size() > transaction.recv_buffer.size() {
            return Err(Error::OutOfRange);
        }
        response_buffer.copy_into(0, &mut transaction.recv_buffer)?;

        transaction.recv_buffer.truncate(response_buffer.size());
        self.channel.handler_base.signal(
            kernel,
            SignalUpdate::clear(Signals::READABLE | Signals::WRITEABLE),
        );
        self.channel
            .initiator_base
            .signal(kernel, SignalUpdate::raise(Signals::READABLE));
        Ok(())
    }

    fn object_set_peer_user_signal(&self, kernel: K, set: bool) -> Result<()> {
        self.channel
            .initiator_base
            .signal(kernel, SignalUpdate::set_if(Signals::USER, set));
        Ok(())
    }

    /// Reset the handler object. If there is a mid-flight transaction, cancel it.
    fn reset(&self, kernel: K) -> Result<()> {
        // Clear peer USER signal on initiator.
        self.channel
            .initiator_base
            .signal(kernel, SignalUpdate::clear(Signals::USER));

        let mut active_transaction = self.channel.active_transaction.lock();
        if active_transaction.take().is_some() {
            drop(active_transaction);

            self.channel
                .initiator_base
                .signal(kernel, SignalUpdate::raise(Signals::ERROR));
        }
        Ok(())
    }
}

/// The initiator endpoint of a [`Channel`].
pub struct ChannelInitiatorObject<K: Kernel> {
    channel: ForeignRc<<K as Arch>::AtomicUsize, Channel<K>>,
}

impl<K: Kernel> ChannelInitiatorObject<K> {
    #[must_use]
    pub fn new(channel: ForeignRc<<K as Arch>::AtomicUsize, Channel<K>>) -> Self {
        Self { channel }
    }
}

impl<K: Kernel> KernelObject<K> for ChannelInitiatorObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.channel.initiator_base)
    }

    /// Reset the initiator object. Clear any active transaction, and
    /// restore the initial signals.
    fn reset(&self, kernel: K) -> Result<()> {
        // Clear peer USER signal on handler.
        self.channel
            .handler_base
            .signal(kernel, SignalUpdate::clear(Signals::USER));

        // Cancel the active transaction.
        if self.channel.active_transaction.lock().take().is_some() {
            self.channel
                .handler_base
                .signal(kernel, SignalUpdate::raise(Signals::ERROR));
        }

        // Restore objects initial signals.
        self.channel.initiator_base.signal(
            kernel,
            SignalUpdate::raise(Signals::WRITEABLE).and_clear(Signals::READABLE | Signals::ERROR),
        );

        Ok(())
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<<K>::Clock>,
    ) -> Result<WaitReturn> {
        self.channel
            .initiator_base
            .wait_until(kernel, signal_mask, deadline)
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
        let active_signals = self.channel.initiator_base.active_signals(kernel);
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

    fn object_set_peer_user_signal(&self, kernel: K, set: bool) -> Result<()> {
        self.channel
            .handler_base
            .signal(kernel, SignalUpdate::set_if(Signals::USER, set));
        Ok(())
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

        let mut active_transaction = self.channel.active_transaction.lock();

        // Check to see if a transaction is already active on the channel.
        if active_transaction.is_some() {
            return Err(Error::Unavailable);
        }

        *active_transaction = Some(Transaction {
            send_buffer,
            recv_buffer,
        });

        drop(active_transaction);

        // Clear Readable and Writable & Error signals on our side before
        // signaling the handler.
        self.channel.initiator_base.signal(
            kernel,
            SignalUpdate::clear(Signals::READABLE | Signals::WRITEABLE | Signals::ERROR),
        );

        self.channel.handler_base.signal(
            kernel,
            SignalUpdate::raise(Signals::READABLE).and_clear(Signals::WRITEABLE),
        );

        Ok(())
    }

    fn finish_transaction(&self, kernel: K) -> Result<usize> {
        // TODO: konkers - Rationalize signal behavior with syscall_defs.rs.
        // Go back to the writable state now that the transaction is finished.
        self.channel.initiator_base.signal(
            kernel,
            SignalUpdate::raise(Signals::WRITEABLE).and_clear(Signals::READABLE),
        );

        // Also reset the handler signals.
        self.channel.handler_base.signal(
            kernel,
            SignalUpdate::clear(Signals::READABLE | Signals::WRITEABLE),
        );

        let mut active_transaction = self.channel.active_transaction.lock();

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
