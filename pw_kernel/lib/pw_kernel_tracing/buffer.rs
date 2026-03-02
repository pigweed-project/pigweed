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

use core::cell::UnsafeCell;
use core::sync::atomic::Ordering;

use crate::{EventType, Record};

/// A single entry in the trace buffer.
#[allow(dead_code)]
#[repr(C)]
struct RecordEntry([u32; 4]);
impl RecordEntry {
    pub const fn new() -> Self {
        Self([0u32; 4])
    }
}

/// A circular buffer for storing trace records.
///
/// The buffer uses atomic operations to manage a write position, allowing multiple
/// producers to write concurrently without locking.
///
/// # Type Parameters
/// * `AtomicUsize`: The atomic type used for the position counter (e.g., `pw_atomic::AtomicUsize`).
/// * `NUM_ENTRIES`: The number of entries in the buffer.
pub struct Buffer<AtomicUsize: pw_atomic::AtomicUsize, const NUM_ENTRIES: usize> {
    records: UnsafeCell<[RecordEntry; NUM_ENTRIES]>,
    pos: AtomicUsize,
}

/// Safety: The buffer is safe to share across threads because:
/// 1. `pos` is an `AtomicUsize` which handles concurrent updates safely.
/// 2. `records` is accessed via `UnsafeCell`, but the protocol for writing ensures
///    that each slot is effectively owned by a single writer at a time (barring wraparound collisions).
unsafe impl<AtomicUsize: pw_atomic::AtomicUsize, const NUM_ENTRIES: usize> Sync
    for Buffer<AtomicUsize, NUM_ENTRIES>
{
}

impl<AtomicUsize: pw_atomic::AtomicUsize, const NUM_ENTRIES: usize>
    Buffer<AtomicUsize, NUM_ENTRIES>
{
    /// Creates a new, empty buffer.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            records: UnsafeCell::new([const { RecordEntry::new() }; NUM_ENTRIES]),
            pos: AtomicUsize::ZERO,
        }
    }

    /// Returns the total size of the buffer in bytes.
    pub const fn buffer_len(&self) -> usize {
        NUM_ENTRIES * core::mem::size_of::<RecordEntry>()
    }

    /// Returns a raw pointer to the underlying buffer storage.
    ///
    /// # Safety
    /// This provides direct access to the raw buffer storage. The caller must ensure
    /// that they do not violate any safety guarantees, especially if reading while
    /// other threads are writing.
    pub const unsafe fn buffer(&self) -> *const () {
        self.records.get() as *const ()
    }

    /// Adds a record to the buffer.
    ///
    /// This method reserves a slot in the circular buffer using atomic operations
    /// and writes the record data.
    ///
    /// # Arguments
    /// * `time_stamp`: The timestamp of the event.
    /// * `event_type`: The type of event.
    /// * `event_data`: The raw payload of the event.
    ///
    /// # Example
    /// ```
    /// use pw_kernel_tracing::{Buffer, EventType, ContextSwitchEvent};
    /// use core::sync::atomic::AtomicUsize;
    ///
    /// let buffer = Buffer::<AtomicUsize, 4>::new();
    /// let event = ContextSwitchEvent {
    ///     old_thread_id: 1,
    ///     new_thread_id: 2,
    ///     old_thread_state: 3,
    /// };
    /// buffer.add_record(0x1234, EventType::ContextSwitch, event.encode());
    /// ```
    pub fn add_record(&self, time_stamp: u32, event_type: EventType, event_data: [u32; 3]) {
        let pos = loop {
            let pos = self.pos.load(Ordering::SeqCst);
            let next_pos = (pos + 1) % NUM_ENTRIES;
            if self
                .pos
                .compare_exchange(pos, next_pos, Ordering::SeqCst, Ordering::SeqCst)
                .is_ok()
            {
                break pos;
            }
        };

        let record = Record {
            event_type,
            time_stamp,
            event_data,
        };

        // Safety:
        // * The offset into the records array is guaranteed to be in bounds
        //   because it is bounds checked above.
        // * "Reasonable" exclusive access can be assumed because entries written
        //   as soon as their storage is reserved. This makes the likelihood of
        //   the trace buffer wrapping before the entry is re-written extremely
        //   low.
        // * In the unlikely event that the buffer wraps and an entry is corrupted,
        //   the reader of the trace buffer is expected to handle the corrupted
        //   entry.

        unsafe {
            let entry = self.records.get().cast::<RecordEntry>().add(pos);
            *entry = RecordEntry(record.encode());
        }
    }
}

// These tests require an implementation of core::atomic.
#[cfg(all(test, feature = "builtin_atomics"))]
mod tests {
    use core::sync::atomic::AtomicUsize as StdAtomicUsize;

    use unittest::test;

    use super::*;
    // Helper to access private records for testing
    impl<AtomicUsize: pw_atomic::AtomicUsize, const NUM_ENTRIES: usize>
        Buffer<AtomicUsize, NUM_ENTRIES>
    {
        fn get_record_entry(&self, index: usize) -> &RecordEntry {
            unsafe { &(*self.records.get())[index] }
        }
    }

    #[test]
    fn add_record_writes_to_buffer() -> unittest::Result<()> {
        let buffer = Buffer::<StdAtomicUsize, 4>::new();

        buffer.add_record(0x123456, EventType::ContextSwitch, [1, 2, 3]);

        let entry = buffer.get_record_entry(0);
        let header = entry.0[0];
        let timestamp = header & 0x03ff_ffff;
        let event_type_val = (header >> 26) & 0x3f;

        unittest::assert_eq!(timestamp, 0x123456);
        unittest::assert_eq!(event_type_val, EventType::ContextSwitch as u32);
        unittest::assert_eq!(entry.0[1], 1);
        unittest::assert_eq!(entry.0[2], 2);
        unittest::assert_eq!(entry.0[3], 3);

        Ok(())
    }

    #[test]
    fn buffer_wraps_around() -> unittest::Result<()> {
        let buffer = Buffer::<StdAtomicUsize, 2>::new();

        buffer.add_record(0x100, EventType::ContextSwitch, [1, 1, 1]);
        buffer.add_record(0x200, EventType::ContextSwitch, [2, 2, 2]);
        buffer.add_record(0x300, EventType::ContextSwitch, [3, 3, 3]); // Should overwrite index 0

        // Check index 0 (should be [3,3,3])
        let entry0 = buffer.get_record_entry(0);
        unittest::assert_eq!(entry0.0[1], 3);

        // Check index 1 (should be [2,2,2])
        let entry1 = buffer.get_record_entry(1);
        unittest::assert_eq!(entry1.0[1], 2);

        Ok(())
    }

    #[test]
    fn buffer_allocates_slots_correctly() -> unittest::Result<()> {
        let buffer = Buffer::<StdAtomicUsize, 4>::new();

        // internal position should be 0
        unittest::assert_eq!(buffer.pos.load(Ordering::SeqCst), 0);

        buffer.add_record(0x0, EventType::ContextSwitch, [0; 3]);
        // internal position should be 1
        unittest::assert_eq!(buffer.pos.load(Ordering::SeqCst), 1);

        buffer.add_record(0x1, EventType::ContextSwitch, [0; 3]);
        buffer.add_record(0x2, EventType::ContextSwitch, [0; 3]);
        buffer.add_record(0x3, EventType::ContextSwitch, [0; 3]);
        // Should wrap to 0
        unittest::assert_eq!(buffer.pos.load(Ordering::SeqCst), 0);

        Ok(())
    }
}
