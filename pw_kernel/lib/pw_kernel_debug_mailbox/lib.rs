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

//! # Pigweed Kernel Debug Mailbox
//!
//! `pw_kernel_debug_mailbox` provides a lightweight, lock-free communication channel
//! for host-driven system debugging and observability (e.g., via GDB or probe-rs).
//!
//! ## Overview
//!
//! Debug mailboxes allow external debugging tools to inspect or inject messages/commands
//! into running Pigweed kernel systems. Each mailbox consists of:
//! - A static [`Mailbox`] memory structure containing `ready` and `unread` atomic flags,
//!   plus a payload `value`.
//! - An annotation entry ([`DebugMailboxAnnotation`]) placed in the custom ELF linker section
//!   `.pw_kernel.annotations.debug_mailbox`.
//!
//! External host tools parse the `.pw_kernel.annotations.debug_mailbox` linker section
//! in the ELF binary to discover all registered debug mailboxes, their names, memory
//! addresses, and payload sizes.
//!
//! ## Communication Protocol
//!
//! 1. **Initialization**: The target system creates a static [`Mailbox`] instance.
//! 2. **Host Injection**: An external debugger inspects mailbox annotations to locate
//!    a mailbox by name, and once the `ready` flag is 1, writes a payload to `value`,
//!    and sets the `unread` atomic flag to 1.
//! 3. **Target Retrieval**: The target system calls [`Mailbox::try_take()`] or [`Mailbox::poll()`]:
//!    - Sets `ready` to 0 (indicating processing state).
//!    - Checks if `unread` is 1. If set, clears `unread` back to 0 and processes the payload.
//!    - Resets `ready` to 1 (indicating ready for subsequent commands).
//!
//! ## Examples
//!
//! ```
//! #[derive(Copy, Clone, Default)]
//! #[repr(u8)]
//! pub enum TestMessage {
//!     #[default]
//!     None = 0,
//!     Stop = 1,
//!     Resume = 2,
//! }
//!
//! static TEST_MAILBOX: pw_kernel_debug_mailbox::Mailbox<
//!     TestMessage,
//!     core::sync::atomic::AtomicU32,
//! > = pw_kernel_debug_mailbox::Mailbox::new(TestMessage::None);
//!
//! pw_kernel_debug_mailbox::annotate_kernel_debug_mailbox!("test_mailbox", TEST_MAILBOX);
//! ```

#![no_std]
#![allow(dead_code)]

use core::marker::{Copy, Sync};
use core::option::Option;
use core::ptr;
use core::sync::atomic::Ordering;

use pw_atomic::{AtomicLoad, AtomicNew, AtomicStore, AtomicZero};

#[doc(hidden)]
pub mod __private {
    pub use paste::paste;
}

/// Annotation metadata entry generated for each registered debug mailbox.
///
/// Instances of this struct are placed in the `.pw_kernel.annotations.debug_mailbox`
/// linker section so external debugging tools can discover static mailboxes.
#[repr(C, packed(1))]
pub struct DebugMailboxAnnotation {
    /// Name of the mailbox (e.g., `"test_mailbox"`).
    pub name: &'static str,
    /// Memory address of the static [`Mailbox`] instance.
    pub addr: *const (),
    /// Size of the payload type in bytes.
    pub size: usize,
}

// SAFETY: `DebugMailboxAnnotation` contains a raw pointer (`addr`) pointing to a
// static [`Mailbox`] instance. The annotation struct itself is immutable static
// metadata read by host debuggers, and the referenced `Mailbox` is thread-safe (`Sync`).
unsafe impl Sync for DebugMailboxAnnotation {}

/// Emits a static [`DebugMailboxAnnotation`] into the `.pw_kernel.annotations.debug_mailbox` section.
#[macro_export]
macro_rules! annotate_kernel_debug_mailbox {
    ($name:expr, $debug_mailbox:ident) => {
        $crate::__private::paste! {
            #[cfg_attr(not(target_os = "macos"), unsafe(link_section = ".pw_kernel.annotations.debug_mailbox"))]
            #[used]
            pub static [<$debug_mailbox _DEBUG_MAILBOX_ANNOTATION>]: $crate::DebugMailboxAnnotation =
                $crate::DebugMailboxAnnotation {
                    name: $name,
                    addr: &raw const $debug_mailbox as *const (),
                    size: $debug_mailbox.value_size(),
                };
        }
    };
}

/// A lock-free mailbox structure for host-to-target debug communication.
///
/// # Layout
///
/// `Mailbox` uses `#[repr(C)]` so its fields have a fixed, deterministic layout
/// expected by host debug tools:
/// - `ready`: Atomic flag (1 when ready to receive commands, 0 when busy processing).
/// - `unread`: Atomic flag (set to 1 by host to signal a pending message in `value`).
/// - `value`: Payload of type `T`.
#[repr(C)]
pub struct Mailbox<
    T: Copy + Sized,
    U: AtomicNew<u32> + AtomicLoad<u32> + AtomicStore<u32> + AtomicZero,
> {
    ready: U,
    unread: U,
    value: T,
}

impl<T, U> Mailbox<T, U>
where
    T: Copy + Sized,
    U: AtomicNew<u32> + AtomicLoad<u32> + AtomicStore<u32> + AtomicZero,
{
    /// Creates a new `Mailbox` with initial payload `value`.
    pub const fn new(value: T) -> Self {
        Mailbox {
            ready: U::ZERO,
            unread: U::ZERO,
            value,
        }
    }

    /// Returns the size of payload type `T` in bytes.
    pub const fn value_size(&self) -> usize {
        core::mem::size_of::<T>()
    }

    /// Attempts to take a message from the mailbox if a host debugger has set `unread`.
    ///
    /// If there is something in the mailbox, return it and set the mailbox as ready. We do not
    /// clear the value.
    pub fn try_take(&self) -> Option<T> {
        let mut value = None;
        self.poll(|v| value = Some(v));
        value
    }

    /// Polls the mailbox for a message from a host debugger.
    ///
    /// If `unread` is 1, we read the value in the mailbox
    /// and execute a callback in a "critical section"
    pub fn poll(&self, callback: impl FnOnce(T)) {
        // We are no longer accepting new commands while inspecting/processing.
        self.ready.store(0, Ordering::SeqCst);

        if self.unread.load(Ordering::SeqCst) == 1 {
            self.unread.store(0, Ordering::SeqCst);

            // We need to do a volatile read to prevent this access from being cached or elided.
            // SAFETY: the pointer is valid since we hold a reference to self, and T is Copy so it
            // is trivially copyable.
            let copy = unsafe { ptr::read_volatile(&raw const self.value) };

            callback(copy);
        }

        // We are once again accepting new commands.
        self.ready.store(1, Ordering::SeqCst);
    }
}

#[cfg(test)]
mod tests {
    use core::sync::atomic::{AtomicU32, Ordering};

    use unittest::test;

    use crate::Mailbox;

    #[derive(Copy, Clone, PartialEq, Debug, Default)]
    #[repr(u8)]
    pub enum TestMessage {
        #[default]
        None = 0,
        Add = 1,
        Subtract = 2,
    }

    #[unsafe(no_mangle)]
    static TEST_MAILBOX: Mailbox<TestMessage, AtomicU32> = Mailbox::new(TestMessage::Add);

    #[used]
    static TEST_MAILBOX2: Mailbox<u32, AtomicU32> = Mailbox::new(0);

    annotate_kernel_debug_mailbox!("test_mailbox", TEST_MAILBOX);
    annotate_kernel_debug_mailbox!("test_mailbox2", TEST_MAILBOX2);

    #[test]
    fn test_mailbox() -> unittest::Result<()> {
        unittest::assert_eq!(
            { TEST_MAILBOX_DEBUG_MAILBOX_ANNOTATION.name },
            "test_mailbox"
        );
        unittest::assert_eq!(
            { TEST_MAILBOX2_DEBUG_MAILBOX_ANNOTATION.name },
            "test_mailbox2"
        );

        // Before unread flag is set, try_take returns None.
        unittest::assert_eq!(TEST_MAILBOX2.try_take(), None);

        // Simulate debugger setting unread flag to 1.
        TEST_MAILBOX2.unread.store(1, Ordering::SeqCst);

        // After unread flag is set, try_take returns the payload and resets unread to 0.
        unittest::assert_eq!(TEST_MAILBOX2.try_take(), Some(0));

        // Subsequent call returns None since unread was reset.
        unittest::assert_eq!(TEST_MAILBOX2.try_take(), None);

        Ok(())
    }

    #[test]
    fn test_mailbox_poll() -> unittest::Result<()> {
        let mut called = false;
        let mut value_received = TestMessage::None;
        let mut ready_during_callback = 1;
        let mut unread_during_callback = 1;

        // Before unread flag is set, poll does not invoke callback.
        TEST_MAILBOX.poll(|_| {
            called = true;
        });
        unittest::assert_eq!(called, false);
        unittest::assert_eq!(TEST_MAILBOX.ready.load(Ordering::SeqCst), 1);

        // Simulate debugger setting unread flag to 1.
        TEST_MAILBOX.unread.store(1, Ordering::SeqCst);

        // Poll invokes callback with current value, resetting unread to 0 and ready to 0 during callback.
        TEST_MAILBOX.poll(|v| {
            called = true;
            value_received = v;
            ready_during_callback = TEST_MAILBOX.ready.load(Ordering::SeqCst);
            unread_during_callback = TEST_MAILBOX.unread.load(Ordering::SeqCst);
        });

        unittest::assert_eq!(called, true);
        unittest::assert_eq!(value_received, TestMessage::Add);
        unittest::assert_eq!(ready_during_callback, 0);
        unittest::assert_eq!(unread_during_callback, 0);
        unittest::assert_eq!(TEST_MAILBOX.ready.load(Ordering::SeqCst), 1);
        unittest::assert_eq!(TEST_MAILBOX.unread.load(Ordering::SeqCst), 0);

        // Subsequent poll call does not invoke callback since unread was reset.
        called = false;
        TEST_MAILBOX.poll(|_| {
            called = true;
        });
        unittest::assert_eq!(called, false);

        Ok(())
    }
}
