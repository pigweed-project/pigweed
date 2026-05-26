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

/// A const-generic templated, fixed-size lock-free FIFO ring buffer.
pub struct RingBuffer<const SIZE: usize> {
    buffer: [u8; SIZE],
    head: usize,
    tail: usize,
}

impl<const SIZE: usize> RingBuffer<SIZE> {
    /// Creates a new, empty `RingBuffer` instance.
    pub const fn new() -> Self {
        Self {
            buffer: [0; SIZE],
            head: 0,
            tail: 0,
        }
    }

    /// Pushes a byte to the back of the ring buffer.
    /// Returns `false` if the buffer is full, or `true` on success.
    pub fn push(&mut self, value: u8) -> bool {
        let next = (self.head + 1) % self.buffer.len();
        if next == self.tail {
            false // Full
        } else {
            self.buffer[self.head] = value;
            self.head = next;
            true
        }
    }

    /// Pushes a slice of bytes to the back of the ring buffer.
    ///
    /// Supports partial pushes if the buffer is almost full, and returns the number
    /// of bytes successfully pushed.  Performs at most two block copies.
    pub fn push_slice(&mut self, data: &[u8]) -> usize {
        if data.is_empty() {
            return 0;
        }

        let mut bytes_written = 0;
        let buffer_len = self.buffer.len();

        {
            // This mutably borrows `self` so it is wrapped in a sub-scope so that
            // the borrow is dropped before `self.head` is updated below.
            let (segment1, segment2) = self.free_space_segments();

            for segment in [segment1, segment2] {
                if !segment.is_empty() && bytes_written < data.len() {
                    let write_len = core::cmp::min(segment.len(), data.len() - bytes_written);
                    segment[..write_len]
                        .copy_from_slice(&data[bytes_written..bytes_written + write_len]);
                    bytes_written += write_len;
                }
            }
        }

        self.head = (self.head + bytes_written) % buffer_len;

        bytes_written
    }

    /// Returns the two contiguous segments of free space in the buffer, in logical order.
    pub fn free_space_segments(&mut self) -> (&mut [u8], &mut [u8]) {
        let len = self.buffer.len();
        let head = self.head;
        let tail = self.tail;

        let (left, right) = self.buffer.split_at_mut(head);
        if head >= tail {
            let (right_len, left_len) = if tail > 0 {
                (len - head, tail - 1)
            } else {
                (len - 1 - head, 0)
            };
            (&mut right[..right_len], &mut left[..left_len])
        } else {
            // When head < tail, the free space is always in a single contiguous buffer.
            let right_len = tail - 1 - head;
            (&mut right[..right_len], &mut [])
        }
    }

    /// Pops a byte from the front of the ring buffer.
    /// Returns `Some(byte)` if data is available, or `None` if empty.
    pub fn pop(&mut self) -> Option<u8> {
        if self.head == self.tail {
            None // Empty
        } else {
            let value = self.buffer[self.tail];
            self.tail = (self.tail + 1) % self.buffer.len();
            Some(value)
        }
    }

    /// Pops a slice of the maximum contiguous segment of at most `max_len` bytes.
    ///
    /// Invokes the closure `f` with a reference to this slice, then advances the ring buffer
    /// by the amount of bytes consumed. Returns the closure's result.
    pub fn pop_slice<R>(&mut self, max_len: usize, f: impl FnOnce(&[u8]) -> R) -> R {
        if self.head == self.tail {
            return f(&[]);
        }

        let segment = if self.head >= self.tail {
            &self.buffer[self.tail..self.head]
        } else {
            &self.buffer[self.tail..self.buffer.len()]
        };

        let segment_len = core::cmp::min(segment.len(), max_len);
        let result = f(&segment[..segment_len]);

        self.tail = (self.tail + segment_len) % self.buffer.len();

        result
    }

    /// Returns `true` if the ring buffer contains no elements.
    pub fn is_empty(&self) -> bool {
        self.head == self.tail
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_ring_buffer_basic() {
        let mut ring_buffer = RingBuffer::<4>::new();
        assert!(ring_buffer.is_empty());

        // A ring buffer of size 4 can only hold 3 items (due to head/tail tracking boundary)
        assert!(ring_buffer.push(1));
        assert!(ring_buffer.push(2));
        assert!(ring_buffer.push(3));
        assert!(!ring_buffer.push(4)); // Full

        assert_eq!(ring_buffer.pop(), Some(1));
        assert_eq!(ring_buffer.pop(), Some(2));
        assert_eq!(ring_buffer.pop(), Some(3));
        assert_eq!(ring_buffer.pop(), None);
        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_wrap_around() {
        let mut ring_buffer = RingBuffer::<3>::new();

        assert!(ring_buffer.push(10));
        assert!(ring_buffer.push(20));
        assert_eq!(ring_buffer.pop(), Some(10));

        assert!(ring_buffer.push(30)); // Wraps around head pointer
        assert_eq!(ring_buffer.pop(), Some(20));
        assert_eq!(ring_buffer.pop(), Some(30));
        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_pop_slice_contiguous() {
        let mut ring_buffer = RingBuffer::<5>::new();
        assert!(ring_buffer.push(1));
        assert!(ring_buffer.push(2));
        assert!(ring_buffer.push(3));

        // Pop at most 2 bytes
        let popped = ring_buffer.pop_slice(2, |slice| {
            assert_eq!(slice, &[1, 2]);
            slice.len()
        });
        assert_eq!(popped, 2);

        // Verify remaining byte
        assert_eq!(ring_buffer.pop(), Some(3));
        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_pop_slice_wrap_around() {
        let mut ring_buffer = RingBuffer::<4>::new();
        assert!(ring_buffer.push(10));
        assert!(ring_buffer.push(20));
        assert!(ring_buffer.push(30));
        // 0   1   2   3
        // [T  _   _   H]
        // 10  20  30  .

        assert_eq!(ring_buffer.pop(), Some(10));
        // 0   1   2   3
        // _   [T  _   H]
        // .   20  30  .

        assert_eq!(ring_buffer.pop(), Some(20));
        // 0   1   2   3
        // _   _   [T  H]
        // .   .   30  .

        assert!(ring_buffer.push(40));
        // 0   1   2   3
        // H]  _   [T  _
        // .   .   30  40

        assert!(ring_buffer.push(50));
        // 0   1   2   3
        // _   H]  [T  _
        // 50  .   30  40

        // Active elements: 30 (at 2), 40 (at 3), 50 (at 0)
        // Contiguous segment from tail (2) to end of buffer (3) is &[30, 40]
        let popped1 = ring_buffer.pop_slice(5, |slice| {
            assert_eq!(slice, &[30, 40]);
            slice.len()
        });
        assert_eq!(popped1, 2);
        // 0   1   2   3
        // [T  H]  _   _
        // 50  .   .   .

        // Next contiguous segment is &[50]
        let popped2 = ring_buffer.pop_slice(5, |slice| {
            assert_eq!(slice, &[50]);
            slice.len()
        });
        assert_eq!(popped2, 1);
        // 0   1     2   3
        // _   [TH]  _   _
        // 50  .     .   .

        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_push_slice_basic() {
        let mut ring_buffer = RingBuffer::<5>::new();
        let pushed = ring_buffer.push_slice(&[1, 2, 3]);
        assert_eq!(pushed, 3);

        assert_eq!(ring_buffer.pop(), Some(1));
        assert_eq!(ring_buffer.pop(), Some(2));
        assert_eq!(ring_buffer.pop(), Some(3));
        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_push_slice_partial() {
        let mut ring_buffer = RingBuffer::<4>::new();
        // RingBuffer of size 4 has capacity 3
        let pushed = ring_buffer.push_slice(&[10, 20, 30, 40]);
        assert_eq!(pushed, 3); // Partial push, 40 is dropped

        assert_eq!(ring_buffer.pop(), Some(10));
        assert_eq!(ring_buffer.pop(), Some(20));
        assert_eq!(ring_buffer.pop(), Some(30));
        assert!(ring_buffer.is_empty());
    }

    #[test]
    fn test_ring_buffer_push_slice_wrap_around() {
        let mut ring_buffer = RingBuffer::<5>::new();
        assert_eq!(ring_buffer.push_slice(&[1, 2, 3]), 3);
        assert_eq!(ring_buffer.pop(), Some(1)); // tail = 1

        // head = 3, tail = 1. Contiguous free end is 3..5.
        let pushed = ring_buffer.push_slice(&[4, 5]);
        assert_eq!(pushed, 2); // head wraps to 0

        assert_eq!(ring_buffer.pop(), Some(2));
        assert_eq!(ring_buffer.pop(), Some(3)); // tail = 3

        // head = 0, tail = 3. Contiguous free segment is 0..2.
        let pushed2 = ring_buffer.push_slice(&[6, 7]);
        assert_eq!(pushed2, 2);

        assert_eq!(ring_buffer.pop(), Some(4));
        assert_eq!(ring_buffer.pop(), Some(5));
        assert_eq!(ring_buffer.pop(), Some(6));
        assert_eq!(ring_buffer.pop(), Some(7));
        assert!(ring_buffer.is_empty());
    }
}
