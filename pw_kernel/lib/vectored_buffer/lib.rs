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
#![no_std]
use core::cmp::min;
use core::ptr::NonNull;

// NOTE: we assume a rust slice is a fat pointer consisting of a pointer to data and a length.
/// A Slice<T> is a struct with identical layout to a Rust slice.
pub struct Slice<T> {
    /// `addr` is a non-null pointer to the slice.
    pub addr: NonNull<T>,
    /// `length` is the number of elements in the slice.
    pub length: usize,
}

impl<T> Slice<T> {
    #[must_use]
    pub fn as_slice(&self) -> &[T] {
        // SAFETY: The `addr` is a non-null pointer to a slice of `length` elements.
        unsafe { core::slice::from_raw_parts(self.addr.as_ptr(), self.length) }
    }
}

/// Vectord buffer copy allows copying data between arbitrarily shaped vectors of slices.
/// - `src_offset` allows starting the copy at an arbitrary item (regardless of the shape of the slice).
/// - 'copy_len' specifies the number of items to be copied.
/// - The `src` and `dst` are vectors of slices.
///
/// Returns the number of items copied.
#[must_use]
pub fn vectored_buffer_copy<T: Sized>(
    mut src_offset: usize,
    copy_len: usize,
    src: &[Slice<T>],
    dst: &[Slice<T>],
) -> usize {
    if src.is_empty() || dst.is_empty() || copy_len == 0 {
        return 0;
    }
    let mut copied = 0;
    // src and dst indices into the slice of slices.
    let mut src_slice_index = 0;
    let mut dst_slice_index = 0;

    // Advance src index to the src slice that contains the actual start.
    while src_offset >= src[src_slice_index].length {
        src_offset -= src[src_slice_index].length;
        src_slice_index += 1;
        if src_slice_index >= src.len() {
            return copied;
        }
    }
    // src and dst offsets of the slice indexed by src_slice_index or dst_slice_index.
    let mut src_slice_offset = src_offset;
    let mut dst_slice_offset = 0;
    // Number of elements left to copy.
    let mut remaining_elements = copy_len;

    while let Some(src_item) = src.get(src_slice_index) {
        // Determine how many elements we can copy.
        let n = min(
            src_item.length - src_slice_offset,
            dst[dst_slice_index].length - dst_slice_offset,
        );
        let n = min(n, remaining_elements);
        unsafe {
            // SAFETY: The src_item and dst are valid slices.
            src_item
                .addr
                .add(src_slice_offset)
                .copy_to(dst[dst_slice_index].addr.add(dst_slice_offset), n);
        }

        copied += n;
        remaining_elements -= n;
        if remaining_elements == 0 {
            // Exit if we're done.
            break;
        }

        // Advance to the next src slice if we've exhausted this one.
        src_slice_offset += n;
        if done_with_slice(src, &mut src_slice_index, &mut src_slice_offset) {
            break;
        }

        // Advance to the next dst slice if we've exhausted this one.
        dst_slice_offset += n;
        if done_with_slice(dst, &mut dst_slice_index, &mut dst_slice_offset) {
            break;
        }
    }
    copied
}

// Update the index and offset.  Returns `true` if we reach the end of the slice of slices.
#[inline(always)]
fn done_with_slice<T>(slice: &[Slice<T>], index: &mut usize, offset: &mut usize) -> bool {
    if *offset == slice[*index].length {
        *index += 1;
        *offset = 0;
        if *index >= slice.len() {
            return true;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use unittest::{Result, assert_eq, test};

    use super::*;

    #[allow(dead_code)]
    impl<T> Slice<T> {
        fn new<'a>(s: &'a [&'a [T]]) -> &'a [Slice<T>] {
            // SAFETY: &[&[T]] has the same layout as &[Slice<T>].
            unsafe { core::mem::transmute::<&[&[T]], &[Slice<T>]>(s) }
        }
        fn new_mut<'a>(s: &'a mut [&'a mut [T]]) -> &'a [Slice<T>] {
            // SAFETY: &[&[T]] has the same layout as &[Slice<T>].
            unsafe { core::mem::transmute::<&mut [&mut [T]], &[Slice<T>]>(s) }
        }
    }

    // Checks that the layout of `Slice<u8>` is the same as `&[u8]`.
    #[test]
    fn vecbuf_slice_layout_u8() -> Result<()> {
        let a = &mut [0u8, 1, 2, 3];
        // SAFETY: Transmute to verify that Slice<T> has identical layout to &[T].
        let b = unsafe { core::mem::transmute::<&[u8], Slice<u8>>(a) };
        let a_ptr = NonNull::new(a.as_mut_ptr()).expect("ptr is not null");
        let a_len = a.len();
        assert_eq!(a_ptr, b.addr);
        assert_eq!(a_len, b.length);
        Ok(())
    }

    // Checks that the layout of `Slice<u16>` is the same as `&[u16]`.
    #[test]
    fn vecbuf_slice_layout_u16() -> Result<()> {
        let a = &mut [0u16, 1, 2, 3];
        // SAFETY: Transmute to verify that Slice<T> has identical layout to &[T].
        let b = unsafe { core::mem::transmute::<&[u16], Slice<u16>>(a) };
        let a_ptr = NonNull::new(a.as_mut_ptr()).expect("ptr is not null");
        let a_len = a.len();
        assert_eq!(a_ptr, b.addr);
        assert_eq!(a_len, b.length);
        Ok(())
    }

    // Checks that the layout of `Slice<u32>` is the same as `&[u32]`.
    #[test]
    fn vecbuf_slice_layout_u32() -> Result<()> {
        let a = &mut [0u32, 1, 2, 3];
        // SAFETY: Transmute to verify that Slice<T> has identical layout to &[T].
        let b = unsafe { core::mem::transmute::<&[u32], Slice<u32>>(a) };
        let a_ptr = NonNull::new(a.as_mut_ptr()).expect("ptr is not null");
        let a_len = a.len();
        assert_eq!(a_ptr, b.addr);
        assert_eq!(a_len, b.length);
        Ok(())
    }

    #[test]
    fn vecbuf_copy_one_buffer() -> Result<()> {
        let a = [0u8, 1, 2, 3];
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            b.len(),
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 4);
        assert_eq!(a, b);
        Ok(())
    }

    #[test]
    fn vecbuf_copy_buffers() -> Result<()> {
        let a1 = [0u8, 1, 2, 3];
        let a2 = [4u8, 5];
        let a3 = [6u8, 7];
        let mut b = [0u8; 8];
        let len = vectored_buffer_copy(
            0,
            b.len(),
            Slice::new(&[a1.as_slice(), a2.as_slice(), a3.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 8);
        assert_eq!(b, [0u8, 1, 2, 3, 4, 5, 6, 7]);
        Ok(())
    }

    // 1. Basic Boundary and Edge Cases
    #[test]
    fn vecbuf_copy_zero_len() -> Result<()> {
        let a = [1u8, 2, 3];
        let mut b = [0u8; 3];
        let len = vectored_buffer_copy(
            0,
            0, // copy_len is 0
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 0);
        assert_eq!(b, [0, 0, 0]);
        Ok(())
    }

    #[test]
    fn vecbuf_empty_src_array() -> Result<()> {
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            4,
            Slice::new(&[]), // Empty source vector
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 0);
        Ok(())
    }

    #[test]
    fn vecbuf_empty_dst_array() -> Result<()> {
        let a = [1u8, 2];
        let len = vectored_buffer_copy(
            0,
            2,
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut []), // Empty dest vector
        );
        assert_eq!(len, 0);
        Ok(())
    }

    // 2. Source Offsets
    #[test]
    fn vecbuf_src_offset_skips_slice() -> Result<()> {
        let a1 = [1u8, 2];
        let a2 = [3u8, 4, 5];
        let mut b = [0u8; 5];
        let len = vectored_buffer_copy(
            3, // Skip first slice entirely, and skip 1 element of the second slice
            5,
            Slice::new(&[a1.as_slice(), a2.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 2); // Should only copy [4, 5]
        assert_eq!(b, [4, 5, 0, 0, 0]);
        Ok(())
    }

    #[test]
    fn vecbuf_src_offset_exact_boundary() -> Result<()> {
        let a1 = [1u8, 2];
        let a2 = [3u8, 4];
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            2, // Starts exactly at the beginning of a2
            4,
            Slice::new(&[a1.as_slice(), a2.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 2);
        assert_eq!(b, [3, 4, 0, 0]);
        Ok(())
    }

    #[test]
    fn vecbuf_src_offset_out_of_bounds() -> Result<()> {
        let a = [1u8, 2];
        let mut b = [0u8; 2];
        let len = vectored_buffer_copy(
            5, // Out of bounds
            2,
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 0);
        assert_eq!(b, [0, 0]);
        Ok(())
    }

    // 3. Lengths and Truncations
    #[test]
    fn vecbuf_copy_len_less_than_available() -> Result<()> {
        let a = [1u8, 2, 3, 4];
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            2, // Restrict to copying only 2 elements
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 2);
        assert_eq!(b, [1, 2, 0, 0]);
        Ok(())
    }

    #[test]
    fn vecbuf_dst_shorter_than_src() -> Result<()> {
        let a = [1u8, 2, 3, 4];
        let mut b = [0u8; 2];
        let len = vectored_buffer_copy(
            0,
            10, // Try to copy more than dst can hold
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 2); // Truncates at dst bounds
        assert_eq!(b, [1, 2]);
        Ok(())
    }

    #[test]
    fn vecbuf_src_shorter_than_dst() -> Result<()> {
        let a = [1u8, 2];
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            10, // Try to copy more than src has
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 2); // Truncates at src bounds
        assert_eq!(b, [1, 2, 0, 0]);
        Ok(())
    }

    // 4. Shape Mismatch.
    #[test]
    fn vecbuf_scatter() -> Result<()> {
        let a = [1u8, 2, 3, 4];
        let mut b1 = [0u8; 1];
        let mut b2 = [0u8; 2];
        let mut b3 = [0u8; 1];
        let len = vectored_buffer_copy(
            0,
            4,
            Slice::new(&[a.as_slice()]), // 1 large src slice
            Slice::new_mut(&mut [b1.as_mut_slice(), b2.as_mut_slice(), b3.as_mut_slice()]), // Spread over 3 dst slices
        );
        assert_eq!(len, 4);
        assert_eq!(b1, [1]);
        assert_eq!(b2, [2, 3]);
        assert_eq!(b3, [4]);
        Ok(())
    }

    #[test]
    fn vecbuf_misaligned_slices() -> Result<()> {
        let a1 = [1u8, 2, 3];
        let a2 = [4u8, 5];
        let mut b1 = [0u8; 2];
        let mut b2 = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            5,
            Slice::new(&[a1.as_slice(), a2.as_slice()]), // Boundaries at [3, 2]
            Slice::new_mut(&mut [b1.as_mut_slice(), b2.as_mut_slice()]), // Boundaries at [2, 4]
        );
        assert_eq!(len, 5);
        assert_eq!(b1, [1, 2]);
        assert_eq!(b2, [3, 4, 5, 0]);
        Ok(())
    }

    // 5. Zero-Length Inner Slices
    #[test]
    fn vecbuf_empty_slices_in_src() -> Result<()> {
        let a1 = [1u8, 2];
        let a2: [u8; 0] = [];
        let a3 = [3u8, 4];
        let mut b = [0u8; 4];
        let len = vectored_buffer_copy(
            0,
            4,
            Slice::new(&[a1.as_slice(), a2.as_slice(), a3.as_slice()]), // inner slice a2 has length 0
            Slice::new_mut(&mut [b.as_mut_slice()]),
        );
        assert_eq!(len, 4);
        assert_eq!(b, [1, 2, 3, 4]); // Zero length slice should be cleanly skipped
        Ok(())
    }

    #[test]
    fn vecbuf_empty_slices_in_dst() -> Result<()> {
        let a = [1u8, 2, 3, 4];
        let mut b1 = [0u8; 2];
        let mut b2: [u8; 0] = [];
        let mut b3 = [0u8; 2];
        let len = vectored_buffer_copy(
            0,
            4,
            Slice::new(&[a.as_slice()]),
            Slice::new_mut(&mut [b1.as_mut_slice(), b2.as_mut_slice(), b3.as_mut_slice()]), // inner slice b2 has length 0
        );
        assert_eq!(len, 4);
        assert_eq!(b1, [1, 2]);
        assert_eq!(b2, []); // Zero length slice should be cleanly skipped
        assert_eq!(b3, [3, 4]);
        Ok(())
    }
}
