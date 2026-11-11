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

use core::mem;

use crate::scheduler::Priority;

/// Provides bitmask utility sufficient for tracking the status of all priorities.
/// All operations are restricted to the valid range of priorities.
pub struct PriorityBitmask {
    /// Invariant: The bits set within `bitmasks` always correspond to valid
    /// `Priority` enum values. This is enforced by the `set_priority` and
    /// `clear_priority` methods, which only accept `Priority` enum values.
    bitmasks: [usize; Self::CHUNKS],
}

impl Default for PriorityBitmask {
    fn default() -> Self {
        Self::new()
    }
}

impl PriorityBitmask {
    const BITS_PER_CHUNK: usize = pw_cast::cast!(usize::BITS => usize);
    const CHUNKS: usize = Priority::NUM_PRIORITIES.div_ceil(Self::BITS_PER_CHUNK);

    #[must_use]
    pub const fn new() -> Self {
        // Run compile-time assertions that the Priority enum is suitable for
        // use in the PriorityBitmask. This is necessary to guarantee the
        // transmute in `get_highest_priority` is safe.
        const _: () = {
            // Guarantee assumptions on the size and range of the Priority enum.
            assert!(mem::size_of::<Priority>() <= mem::size_of::<u8>());
            assert!(Priority::NUM_PRIORITIES - 1 <= u8::MAX as usize);
        };

        Self {
            bitmasks: [0; Self::CHUNKS],
        }
    }

    /// Marks a priority as active.
    pub fn set_priority(&mut self, priority: Priority) {
        let chunk_index = priority as usize / Self::BITS_PER_CHUNK;
        let bit_in_chunk = priority as usize % Self::BITS_PER_CHUNK;
        self.bitmasks[chunk_index] |= 1 << bit_in_chunk;
    }

    /// Marks a priority as inactive.
    pub fn clear_priority(&mut self, priority: Priority) {
        let chunk_index = priority as usize / Self::BITS_PER_CHUNK;
        let bit_in_chunk = priority as usize % Self::BITS_PER_CHUNK;
        self.bitmasks[chunk_index] &= !(1 << bit_in_chunk);
    }

    /// Retrieves the highest priority that is active. If no priorities are active,
    /// `None` is returned.
    #[must_use]
    pub fn get_highest_priority(&self) -> Option<Priority> {
        use pw_cast::CastInto as _;

        const OFFSET: u32 = usize::BITS - 1;
        for i in (0..Self::CHUNKS).rev() {
            if self.bitmasks[i] != 0 {
                let highest_bit_index: usize =
                    (OFFSET - self.bitmasks[i].leading_zeros()).cast_into();
                let raw_priority_value = highest_bit_index + (i * Self::BITS_PER_CHUNK);

                // Priority values are guaranteed to be u8 sized.
                #[expect(clippy::cast_possible_truncation)]
                let priority_value = raw_priority_value as u8;

                // SAFETY: Per the `PriorityBitmask` invariant, all set bits
                // correspond to valid `Priority` enum values. Thus, the
                // extracted `priority_value` is guaranteed to be a valid
                // discriminant for `Priority`.
                let priority = unsafe { mem::transmute::<u8, Priority>(priority_value) };
                return Some(priority);
            }
        }

        None
    }
}

#[cfg(test)]
mod tests {
    use unittest::test;

    use super::*;

    #[test]
    fn empty_bitmask_returns_none() -> unittest::Result<()> {
        let bitmask = PriorityBitmask::new();
        unittest::assert_eq!(bitmask.get_highest_priority(), None);
        Ok(())
    }

    #[test]
    fn set_and_clear_priorities_tracks_highest() -> unittest::Result<()> {
        let mut bitmask = PriorityBitmask::new();
        bitmask.set_priority(Priority::Level0);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level0));

        bitmask.set_priority(Priority::Level15);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level15));

        bitmask.set_priority(Priority::Level31);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level31));

        bitmask.clear_priority(Priority::Level31);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level15));

        bitmask.clear_priority(Priority::Level15);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level0));

        bitmask.clear_priority(Priority::Level0);
        unittest::assert_eq!(bitmask.get_highest_priority(), None);
        Ok(())
    }

    #[test]
    fn non_monotonic_clear_and_duplicate_ops_preserve_invariants() -> unittest::Result<()> {
        let mut bitmask = PriorityBitmask::new();

        // Clearing an already-unset priority is a safe no-op.
        bitmask.clear_priority(Priority::Level15);
        unittest::assert_eq!(bitmask.get_highest_priority(), None);

        // Duplicate `set_priority` calls are idempotent.
        bitmask.set_priority(Priority::Level0);
        bitmask.set_priority(Priority::Level15);
        bitmask.set_priority(Priority::Level31);
        bitmask.set_priority(Priority::Level15);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level31));

        // Clearing a middle priority while a higher priority remains set keeps `Level31`.
        bitmask.clear_priority(Priority::Level15);
        bitmask.clear_priority(Priority::Level15);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level31));

        // Clearing `Level31` skips the cleared `Level15` bit directly down to `Level0`.
        bitmask.clear_priority(Priority::Level31);
        unittest::assert_eq!(bitmask.get_highest_priority(), Some(Priority::Level0));

        bitmask.clear_priority(Priority::Level0);
        unittest::assert_eq!(bitmask.get_highest_priority(), None);
        Ok(())
    }
}
