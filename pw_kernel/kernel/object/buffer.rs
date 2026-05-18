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
use core::cmp::min;
use core::ptr::NonNull;

use memory_config::MemoryRegionType;
use pw_status::{Error, Result};
use vectored_buffer::{Slice, vectored_buffer_copy};

use crate::Kernel;
use crate::scheduler::thread::ProcessRef;

/// A `Buffer` represents a buffer or vector of buffers in a user process.
pub enum Buffer {
    Flat(Slice<u8>),
    Vector(Slice<Slice<u8>>),
}

impl Buffer {
    fn new(address: usize, length: isize) -> Result<Self> {
        let Some(address) = NonNull::<u8>::new(core::ptr::with_exposed_provenance_mut(address))
        else {
            return Err(Error::InvalidArgument);
        };
        if length >= 0 {
            Ok(Buffer::Flat(Slice {
                addr: address,
                length: length.cast_unsigned(),
            }))
        } else {
            Ok(Buffer::Vector(Slice {
                addr: address.cast(),
                length: (-length).cast_unsigned(),
            }))
        }
    }

    fn _check_access<K: Kernel>(
        start: usize,
        size: usize,
        access_type: MemoryRegionType,
        process: &ProcessRef<K>,
    ) -> Result<usize> {
        if size > 0 {
            // We only need to check if the size is greater than zero.
            // The pointer for zero-sized objects will be invalid, so
            // we shouldn't check access.
            if !process.range_has_access(access_type, start..(start + size)) {
                return Err(Error::PermissionDenied);
            }
        }
        Ok(size)
    }
    fn check_access<K: Kernel>(
        &self,
        access_type: MemoryRegionType,
        process: &ProcessRef<K>,
    ) -> Result<usize> {
        let total_size = match self {
            Buffer::Flat(buf) => {
                Self::_check_access(buf.addr.as_ptr() as usize, buf.length, access_type, process)?
            }

            Buffer::Vector(vec) => {
                let mut sum = 0;
                // First make sure we can access the vector of slices.
                let _ = Self::_check_access(
                    vec.addr.as_ptr() as usize,
                    vec.length * core::mem::size_of::<Slice<u8>>(),
                    access_type,
                    process,
                )?;
                // Then check each item in the vector and sum up the total size in bytes.
                for item in vec.as_slice() {
                    sum += Self::_check_access(
                        item.addr.as_ptr() as usize,
                        item.length,
                        access_type,
                        process,
                    )?;
                }
                sum
            }
        };
        Ok(total_size)
    }

    #[must_use]
    fn as_slices(&self) -> &[Slice<u8>] {
        match self {
            Buffer::Flat(buf) => core::slice::from_ref(buf),
            Buffer::Vector(vec) => vec.as_slice(),
        }
    }
}

// A buffer that resides in a process
//
// INVARIANTS:
// * All access to the underlying memory is done through the `UserBuffer`. The
//  `UserBuffer` will never expose a pointer or reference to the underlying memory.
//  Additionally `UserBuffer` is in its own leaf module to guard against other
//  code having access to its private fields.
// * Operation on the buffer always respects the access rights of the process to which
//   it is bound.
pub struct SyscallBuffer {
    // Pointer to the user buffer.
    buffer: Buffer,
    // The sum of the sizes of all the buffers in the iovec.
    size: usize,
    access_type: MemoryRegionType,
}

impl SyscallBuffer {
    /// Create a new buffer with access rights bound to the current process.
    pub fn new_in_current_process<K: Kernel>(
        kernel: K,
        access_type: MemoryRegionType,
        address: usize,
        length: isize,
    ) -> Result<Self> {
        let sched = kernel.get_scheduler().lock(kernel);
        let process = sched.current_thread().process();
        let buffer = Buffer::new(address, length)?;

        // SAFETY: Since there is no dynamic memory and processes can not be
        // terminated and restarted, the access check can happen at buffer creates
        // and still uphold the invariant that the processes access rights are respected.
        //
        // TODO: https://pwbug.dev/442660183 - Handle access checks with process termination.
        let size = buffer.check_access(access_type, &process)?;
        Ok(Self {
            buffer,
            size,
            access_type,
        })
    }

    /// Returns the size of the buffer.
    #[must_use]
    pub fn size(&self) -> usize {
        self.size
    }

    /// Reduce the size of the buffer
    ///
    /// # Panics
    /// Panics if new_len is greater than the current buffer length.
    pub fn truncate(&mut self, new_size: usize) {
        pw_assert::assert!(new_size <= self.size);
        self.size = new_size;
    }

    /// Copy from this buffer into another.
    ///
    /// Copies data from this buffer into `into_buffer`, starting at `offset` bytes
    /// from the beginning of this buffer.  Will copy maximum number of bytes
    /// that exist and this buffer and will fit into `into_buffer`
    ///
    /// # Returns
    /// - Ok(0): No data was copied.  This can occur if `offset` is equal to the
    ///   size of the buffer or `into_buffer is of size zero.
    /// - Ok(len): Returns the number of bytes copied.
    /// - Error::PermissionDenied: Either this buffer is not readable or `into_buffer`
    ///   is not writeable
    /// - Error::OutOfRange: `offset` is larger than the size of the buffer.
    pub fn copy_into(&self, offset: usize, into_buffer: &mut SyscallBuffer) -> Result<usize> {
        if !self.access_type.is_readable() || !into_buffer.access_type.is_writeable() {
            return Err(Error::PermissionDenied);
        }

        if offset > self.size {
            return Err(Error::OutOfRange);
        }

        let available_bytes = self.size - offset;
        let copy_len = min(available_bytes, into_buffer.size);

        // SAFETY: The access right invariant is upheld at buffer creation time
        // where the addr and size fields are validated.
        let copied = vectored_buffer_copy(
            offset,
            copy_len,
            self.buffer.as_slices(),
            into_buffer.buffer.as_slices(),
        );
        Ok(copied)
    }

    #[must_use]
    pub fn as_slices(&self) -> &[&[u8]] {
        unsafe {
            // Safety: Address and size are checked and validated in `new()`.
            // The `Slice` type is isomorphic to the underlying rust representation of a slice.
            core::mem::transmute(self.buffer.as_slices())
        }
    }
}
