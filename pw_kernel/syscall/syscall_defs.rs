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

//! # pw_kernel User Space API
//!
//! ## Core Concepts
//!
//! ### Objects
//! Objects are the basic building block of functionality exposed to user space.
//! They are polymorphic and may be one of a limited set of types:
//!
//! - [Channel](#channel)
//! - [Wait Group](#wait-group)
//!
//! ### Handles
//! All system calls reference objects through a u32 handle which indexes into
//! a process-local handle table.
//!
//! ### Signals and Waiting
//! Every kernel object has a set of signals that can be pending and waited
//! upon.  The exact meaning and semantics of each signal vary between kernel
//! objects.  The signal types are:
//! - `Signals::READABLE`: Object is readable.
//! - `Signals::WRITABLE`: Object is writable.
//! - `Signals::ERROR`: Object is in an error state.
//! - `Signals::USER`: User defined signal.  Useful for out of band signaling
//!   between peers on a [Channel](#channel).
//!
//! Any kernel object can be waited for signals to assert using the
//! [`object_wait()`] syscall.  Multiple objects can be waited on simultaneously
//! using a [Wait Group](#wait-group) object.
//!
//! #### Open Questions
//! - What are the multi-threaded semantics of waiting.
//!
//! ### Object and Handle Creation
//! Initially only statically defined and allocated objects and handles are
//! supported.  The creation of these will be driven through build time
//! configuration and the necessary code will be generated to allocate kernel
//! data structures as well as expose handle definitions to user space
//! processes.
//!
//! ## Object Types
//!
//! ### Channel
//! A channel is a unidirectional connection between two asymmetric peers: an
//! initiator and a handler.  A channel allows the initiator peer to send a
//! buffer of data to the handler peer and wait for it's response.  A channel
//! can have a maximum of one transaction pending at a time and is designed to
//! not require intermediate kernel buffers.
//!
//! Both a synchronous and asynchronous API is offered to the initiator
//! while the handler side API is strictly non-blocking.  All data copies
//! between peers happen during the system call.
//!
//! The flow of a transaction is as follows:
//! - The initiator starts the transaction by providing send and receive
//!   buffers to one of the two transact system calls ([`channel_transact()`] or
//!   [`channel_async_transact()`]). This has the additional side effect of
//!   clearing `Signals::READABLE` and `Signals::WRITABLE` on the initiator.
//! - The handler's `Signals::READABLE` will become asserted.
//! - The handler can now read the message in multiple calls to
//!   [`channel_read()`] causing the kernel to copy the data from the
//!   initiator's send buffer to the buffer provided to [`channel_read()`].
//! - The handler completes the transaction by calling [`channel_respond()`]
//!   and providing a response.  The kernel will immediately copy the response
//!   from the handler's buffer to the initiator's receive buffer.
//!   There is no built in mechanism for the handler to signal an error
//!   to the initiator.  This is left to the higher level protocol used to
//!   communicate over the channel.  This will clear `Signals::READABLE` and
//!   `Signals::WRITABLE` on the handler and raise `Signals::READABLE` on
//!   the initiator.
//!
//! The handler's only ways of communicating with the initiator are by
//! - responding to an initiated transaction
//! - raising `Signals::USER` on the initiator by calling
//!   [`object_raise_peer_user_signal()`]
//!
//! #### Initiator Signals
//! - `Signals::WRITABLE` indicates there is no pending transaction and one
//!   can be started. Cleared on transaction initiation.
//! - `Signals::READABLE` indicates the handler has responded to the
//!   pending transaction.  Cleared on transaction initiation.
//! - `Signals::ERROR` indicates pending transaction has an error.  Cleared
//!   when the initiator is waited on.
//! - `Signals::USER` indicates the handler calls [`object_raise_peer_user_signal()`].
//!   Cleared when the initiator is waited on.
//!
//! #### Handler Signals
//! - `Signals::READABLE` indicates there is a pending transaction.  Cleared when
//!   the handler calls [`channel_respond()`].
//! - `Signals::WRITABLE` indicates there is a pending transaction.  Cleared when
//!   the handler calls [`channel_respond()`].
//! - `Signals::ERROR` indicates a pending transaction error.  No error states
//!   are defined at the moment.  In the future an error may be raised when
//!   the remote peer closes.
//! - `Signals::USER` indicates the initiator calls [`object_raise_peer_user_signal()`].
//!   Cleared when the initiator is waited on.
//!
//! ### Wait Group
//! Wait groups provide a mechanism for waiting on multiple handles at once.
//! Handles can be added to and removed from a wait group with [`wait_group_add()`]
//! and [`wait_group_remove()`].  In addition to a set of signals to wait on,
//! an arbitrary `user_data` is provided to [`wait_group_add()`].  This value
//! is returned by [`object_wait()`], unmodified by the kernel, when the wait group is
//! waited on.
//!
//! #### Open questions:
//! - How is the wait group's member list allocated in there kernel.  To support
//!   a fully statically allocated kernel one of two approaches are being
//!   considered:
//!   - an object (or possibly handle) may only be in a single wait group at
//!     one time.  This allows a wait group to maintain an intrusive list of
//!     objects with the list element storage being stored in the object.
//!   - wait queues are statically sized at compile time and adding more objects
//!     than there is space for will return an error.
//!
//! ### Interrupt
//! Interrupt objects provide a mechanism for handling hardware interrupts.
//! A single Interrupt object can be configured to handle multiple interrupt
//! sources (IRQs), up to a maximum of 16 per object.

//! Each interrupt source handled by an Interrupt object is mapped to a unique
//! signal bit within the higher 16 bits of `Signals`. These signals
//! range from `Signals::INTERRUPT_A` (bit 16) to `Signals::INTERRUPT_O` (bit 30).
//!
//! When a interrupt occurs for a source handled by an Interrupt object, the kernel
//! masks the interrupt and then signals the corresponding `INTERRUPT_` bit on the
//! object. Userspace threads can wait on the Interrupt object using the
//! [`object_wait()`] syscall.
//!
//! Upon waking from `object_wait()`, the returned `Signals` mask indicates which
//! interrupt(s) have triggered. After the userspace handler has serviced the
//! interrupt(s), it must call the [`interrupt_ack()`] syscall to allow the
//! interrupt(s) to be triggered again. This syscall must be provided with a
//! `Signals` mask containing the bits for the interrupts that have been
//! handled.
//!
//! [`interrupt_ack()`] accomplishes two things:
//! 1. It clears the specified signal bits on the Interrupt object, allowing it to
//!    receive new signals for those interrupts.
//! 2. It signals to the underlying hardware interrupt controller (e.g., PLIC or
//!    NVIC) that the associated IRQ(s) have been acknowledged, re-enabling them
//!    at the hardware level.
//!
//! ### Futex
//! In design
//!
//! ## System Calls
//! The C ABI system calls listed here are not intended to be called directly
//! by user space code and instead be accessed through language idiomatic
//! wrapper libraries.
//!
//! ### Generic Syscalls
//! - [`object_wait()`]
//! - [`object_raise_peer_user_signal()`]
//!
//! ### Channel Initiator Syscalls
//! - [`channel_transact()`]
//! - [`channel_async_transact()`]
//! - [`channel_async_cancel()`]
//!
//! ### Channel Handler Syscalls
//! - [`channel_read()`]
//! - [`channel_respond()`]
//!
//! ### Wait Group Syscalls
//! - [`wait_group_add()`]
//! - [`wait_group_remove()`]

#![no_std]

use bitflags::bitflags;
use pw_status::{Error, Result};

pub struct SysCallReturnValue {
    // The return value representing the two return registers.
    pub value: [usize; 2],
}

impl From<SysCallReturnValue> for Result<()> {
    fn from(ret_value: SysCallReturnValue) -> Result<()> {
        let val = ret_value.value[0].cast_signed();
        if val < 0 {
            // TODO debug assert if error number is out of range
            let val = (-val).cast_unsigned();
            // TODO(421404517): Avoid the lossy cast
            #[allow(clippy::cast_possible_truncation)]
            Err(unsafe { core::mem::transmute::<u32, Error>(val as u32) })
        } else {
            Ok(())
        }
    }
}

impl From<SysCallReturnValue> for Result<u32> {
    fn from(ret_value: SysCallReturnValue) -> Result<u32> {
        let val = ret_value.value[0].cast_signed();
        if val < 0 {
            // TODO debug assert if error number is out of range
            let val = (-val).cast_unsigned();
            // TODO(421404517): Avoid the lossy cast
            #[allow(clippy::cast_possible_truncation)]
            Err(unsafe { core::mem::transmute::<u32, Error>(val as u32) })
        } else {
            // TODO(421404517): Avoid the lossy cast
            #[allow(clippy::cast_possible_truncation)]
            Ok(ret_value.value[0] as u32)
        }
    }
}

impl From<i64> for SysCallReturnValue {
    fn from(value: i64) -> Self {
        let unsigned_value = value.cast_unsigned();
        Self {
            #[expect(clippy::cast_possible_truncation)]
            value: [
                unsigned_value as u32 as usize,
                (unsigned_value >> 32) as u32 as usize,
            ],
        }
    }
}

impl From<Result<u64>> for SysCallReturnValue {
    fn from(value: Result<u64>) -> Self {
        match value {
            Ok(val) => {
                #[expect(clippy::cast_possible_truncation)]
                let low = val as u32 as usize;
                let high = (val >> 32) as u32 as usize;
                Self { value: [low, high] }
            }
            Err(error) => Self {
                #[expect(clippy::cast_sign_loss)]
                value: [-(error as isize) as usize, 0],
            },
        }
    }
}

impl From<SysCallReturnValue> for Result<WaitReturn> {
    fn from(ret: SysCallReturnValue) -> Result<WaitReturn> {
        // TODO(421404517): Avoid the lossy cast
        #[allow(clippy::cast_possible_truncation)]
        let val = ret.value[1] as u32;
        let signals = Signals::from_bits_truncate(val);
        // Encode that there was an error in the reserved high bit of
        // the signal mask, and store the error in the data field.
        if signals.contains(Signals::RESERVED) {
            // TODO(421404517): Avoid the lossy cast
            #[allow(clippy::cast_possible_truncation)]
            Err(unsafe { core::mem::transmute::<u32, Error>(ret.value[0] as u32) })
        } else {
            Ok(WaitReturn {
                user_data: ret.value[0],
                pending_signals: signals,
            })
        }
    }
}

impl From<Result<WaitReturn>> for SysCallReturnValue {
    fn from(value: Result<WaitReturn>) -> Self {
        match value {
            Ok(val) => Self {
                value: [val.user_data, val.pending_signals.bits() as usize],
            },
            Err(error) => Self {
                // Error value is saved in the data field and the reserved bit
                // is used to indicate an error in the signal mask.
                value: [error as usize, Signals::RESERVED.bits() as usize],
            },
        }
    }
}

#[derive(Copy, Clone)]
#[repr(u16)]
#[non_exhaustive]
pub enum SysCallId {
    // IDs are not ABI stable yet and are subject to change.
    ObjectWait = 0x0000,
    ChannelTransact = 0x0001,
    ChannelRead = 0x0002,
    ChannelRespond = 0x0003,
    InterruptAck = 0x0004,

    // System calls prefixed with 0xF000 are reserved development/debugging use.
    DebugPutc = 0xf000,
    DebugShutdown = 0xf001,
    DebugLog = 0xf002,
    DebugNop = 0xf003,
    DebugTriggerInterrupt = 0xf004,
}

impl From<u16> for SysCallId {
    fn from(value: u16) -> Self {
        // SAFETY: SysCallId is repr(u16) and non-exhaustive.
        unsafe { core::mem::transmute::<u16, SysCallId>(value) }
    }
}

/// A set of object signals
#[derive(Copy, Clone)]
#[repr(C)]
pub struct Signals(u32);

bitflags! {
    impl Signals: u32 {
        /// Object is readable.
        const READABLE = 1 << 0;

        /// Object is writeable.
        const WRITEABLE = 1 << 1;

        /// Object is in an error state.
        const ERROR = 1 << 2;

        /// Object has a protocol specific user signal pending.
        const USER = 1 << 15;

        /// Bits 16-30 are used to denote which interrupt on the
        /// interrupt object was signaled.  They are intentionally
        /// named by letter not number so as not to confuse the
        /// position within an object mask to the IRQ number.
        const INTERRUPT_A = 1 << 16;
        const INTERRUPT_B = 1 << 17;
        const INTERRUPT_C = 1 << 18;
        const INTERRUPT_D = 1 << 19;
        const INTERRUPT_E = 1 << 20;
        const INTERRUPT_F = 1 << 21;
        const INTERRUPT_G = 1 << 22;
        const INTERRUPT_H = 1 << 23;
        const INTERRUPT_I = 1 << 24;
        const INTERRUPT_J = 1 << 25;
        const INTERRUPT_K = 1 << 26;
        const INTERRUPT_L = 1 << 27;
        const INTERRUPT_M = 1 << 28;
        const INTERRUPT_N = 1 << 29;
        const INTERRUPT_O = 1 << 30;

        /// Reserved for internal kernel use.
        const RESERVED = 1 << 31;
    }
}

impl Signals {
    #[must_use]
    pub const fn new() -> Self {
        Self(0)
    }
}

/// Return value from the [`object_wait()`] syscall.
///
/// TODO: This is a bit heavy.  Define exact syscall ABI for this data.
#[derive(Copy, Clone)]
#[repr(C)]
pub struct WaitReturn {
    /// `user_data` of the wait group member.
    pub user_data: usize,

    /// Signals pending on the wait group member.
    pub pending_signals: Signals,
}

unsafe extern "C" {
    /// Perform a synchronous channel transaction
    ///
    /// Performs a transaction from the initiator side of a channel and blocks
    /// until the handler side has completed the transaction.  `send_data` and
    /// `recv_data` may overlap or be the same buffer.
    ///
    /// While non-block and infinite blocking semantics are not explicitly
    /// supported, they can be effectively achieved with:
    /// - Non-blocking: `deadline` == 0
    /// - Infinite blocking: `deadline` == `u64::MAX`
    ///
    /// This call will cause `Signals::READABLE` to be cleared on the initiator
    /// channel object at the beginning of execution.  However by the time it
    /// returns without error, `Signals::READABLE` will be set again.
    ///
    /// The maximum size of buffers that may be passed is `isize::MAX`.
    ///
    /// # Returns
    /// - `>=0`: Number of bytes received from the handler side.
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid initiator
    ///   channel object.
    /// - [`Error::ResourceExhausted`]: The channel already has a pending transaction.
    /// - [`Error::PermissionDenied`]: `send_data` or `recv_data` do not reference
    ///   valid memory regions in this processes' address space.
    /// - [`Error::DeadlineExceeded`]: The handler side did not respond before
    ///   `deadline` was exceeded.
    pub fn channel_transact(
        object_handle: u32,
        send_data: *mut u8,
        send_len: usize,
        recv_data: *mut u8,
        recv_len: usize,
        deadline: u64,
    ) -> isize;

    /// Perform an asynchronous channel transaction
    ///
    /// Initiates a transaction from the initiator side of a channel.
    /// send_data` must remain valid and readable and `recv_data` must remain
    /// valid and writeable for the duration of the transaction (completed or
    /// canceled.)  `send_data` and `recv_data` may overlap or be the same
    /// buffer.
    ///
    /// This call will cause `Signals::READABLE` to be cleared on the initiator
    /// channel object.  It will be signaled by the handler side when it
    /// responds.
    ///
    /// # Returns
    /// - `0`: Transaction was successfully initiated.
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid initiator
    ///   channel object.
    /// - [`Error::ResourceExhausted`]: The channel already has a pending transaction.
    /// - [`Error::PermissionDenied`]: `send_data` or `recv_data` do not reference
    ///   valid memory regions in this processes' address space.
    pub fn channel_async_transact(
        object_handle: u32,
        send_data: *const u8,
        send_len: usize,
        recv_data: *mut u8,
        recv_len: usize,
    ) -> isize;

    /// Cancels a pending transaction on a channel
    ///
    /// # Returns
    /// - `0`: Pending transaction was successfully canceled.
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid initiator
    ///   channel object.
    /// - [`Error::FailedPrecondition`]: No transaction was pending on the channel.
    pub fn channel_async_cancel(object_handle: u32) -> isize;

    /// Perform a non-blocking read from a pending transaction.
    ///
    /// Attempts to read up to `buf_len` bytes from the `send_buffer` of the
    /// pending transaction starting from `offset`.  The kernel will copy the
    /// data from the initiators `send_buffer` into `buffer` before returning.
    ///
    /// The maximum size of buffer that may be passed is `isize::MAX`.
    ///
    /// # Returns
    /// - `>=0`: Number of bytes read from the send_buffer.
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid handler
    ///   channel object.
    /// - [`Error::OutOfRange`]: A read was requested outside the bound of the
    ///   initiator's `send_buffer`.
    /// - [`Error::FailedPrecondition`]: No transaction was pending on the channel.
    ///   This can happen in the middle of handling a transaction if the initiator
    ///   cancels the transaction.
    /// - [`Error::PermissionDenied`]: `buffer` does not reference a valid memory
    ///   region in this processes' address space.
    /// - TODO: What error should be returned if the initiator's `send_buffer`
    ///   is invalid.  Is that also permission denied?
    pub fn channel_read(
        object_handle: u32,
        offset: usize,
        buffer: *mut u8,
        buffer_len: usize,
    ) -> isize;

    /// Respond to and complete a pending transaction
    ///
    /// Causes the kernel to copy `buffer` into the initiator's `recv_buffer`
    /// and set `Signals::READABLE` on the initiator channel object.
    ///
    /// The maximum size of buffer that may be passed is `isize::MAX`.
    ///
    /// # Returns
    /// - `0`: On success.
    /// - [`Error::OutOfRange`]: The initiator's `recv_buffer` is not large enough
    ///   to fit the provided `buffer`.
    /// - [`Error::FailedPrecondition`]: No transaction was pending on the channel.
    ///   This can happen in the middle of handling a transaction if the initiator
    ///   cancels the transaction.
    /// - [`Error::PermissionDenied`]: `buffer` does not reference a valid memory
    ///   region in this processes' address space.
    pub fn channel_respond(object_handle: u32, buffer: *mut u8, buffer_len: usize) -> isize;

    /// Raise `Signals::USER` on a paired object's peer
    ///
    /// Since channels are unidirectional, this serves as a way for the handler
    /// to signal the initiator.
    pub fn object_raise_peer_user_signal(object_handle: u32) -> isize;

    /// Acknowledges the signaled interrupts allowing them to be signaled again.
    ///
    /// This must be called by the interrupt handler after processing the
    /// interrupts received via an interrupt object. It signals to the
    /// underlying interrupt controller that the interrupt handling is complete
    /// and can be signaled again.
    ///
    /// # Returns
    /// - `0`: On success.
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid Interrupt
    ///   object, or the `signal_mask` is invalid.
    pub fn interrupt_ack(object_handle: u32, signal_mask: Signals) -> isize;

    //
    // waiting
    //
    // Need to define multi threaded semantics

    /// Wait on a single object
    ///
    /// Waits for one of the signals in `signal_mask` to be pending on `object_handle`.
    ///
    /// # Returns
    /// - `WaitReturn.status >= 0`: Returns object specific metadata
    /// - [`Error::InvalidArgument`]: `object_handle` is not a valid object.
    /// - [`Error::DeadlineExceeded`]: The handler side did not respond before
    ///   `deadline` was exceeded.
    pub fn object_wait(object_handle: u32, signal_mask: Signals, deadline: u64) -> WaitReturn;

    /// Adds an object to a wait group
    ///
    /// Add `object` to `wait_group`.  `wait_group` will signal when one of the
    /// signals in `signal_mask` is raised on `object`.  `user_data` is passed,
    /// untouched by the kernel to the return value of `wait()`.
    ///
    /// # Returns
    /// `0`: Success
    /// - [`Error::InvalidArgument`]: `wait_group` is not a valid wait group or
    ///   `object` is not a valid object.
    /// - [`Error::ResourceExhausted`]: `object` is already in a wait group.
    pub fn wait_group_add(
        wait_group: u32,
        object: u32,
        signal_mask: Signals,
        user_data: usize,
    ) -> isize;

    /// Removes an object from a wait group
    ///
    /// # Returns
    /// `0`: Success
    /// - [`Error::InvalidArgument`]: `wait_group` is not a valid wait group or
    ///   `object` is not a valid object.
    /// - [`Error::NotFound`]: `object` is not in `wait_group`.
    pub fn wait_group_remove(wait_group: u32, object: u32) -> isize;
}

pub trait SysCallInterface {
    fn object_wait(handle: u32, signal_mask: u32, deadline: u64) -> Result<WaitReturn>;

    #[expect(clippy::missing_safety_doc)]
    unsafe fn channel_transact(
        handle: u32,
        send_data: *const u8,
        send_len: usize,
        recv_data: *mut u8,
        recv_len: usize,
        deadline: u64,
    ) -> Result<u32>;

    #[expect(clippy::missing_safety_doc)]
    unsafe fn channel_read(
        object_handle: u32,
        offset: usize,
        buffer: *mut u8,
        buffer_len: usize,
    ) -> Result<u32>;

    #[expect(clippy::missing_safety_doc)]
    unsafe fn channel_respond(
        object_handle: u32,
        buffer: *const u8,
        buffer_len: usize,
    ) -> Result<()>;

    fn interrupt_ack(object_handle: u32, signal_mask: Signals) -> Result<()>;

    fn debug_putc(a: u32) -> Result<u32>;
    // TODO: Consider adding an feature flagged PowerManager object and move
    // this shutdown call to it.
    fn debug_shutdown(a: u32) -> Result<()>;

    #[expect(clippy::missing_safety_doc)]
    unsafe fn debug_log(buffer: *const u8, buffer_len: usize) -> Result<()>;

    fn debug_nop() -> Result<()>;

    fn debug_trigger_interrupt(irq: u32) -> Result<()>;
}
