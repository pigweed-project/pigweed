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

#[doc(hidden)]
pub mod __private {
    use core::ptr;

    pub use paste::paste;

    /// Read a raw value from the register
    ///
    /// # Safety
    /// The caller must guarantee that provided `addr` is accessible.
    #[inline]
    #[must_use]
    pub unsafe fn raw_read<T>(addr: usize) -> T {
        let ptr = ptr::with_exposed_provenance::<T>(addr);
        unsafe { ptr.read_volatile() }
    }

    /// Write a raw value to the specified register
    ///
    /// # Safety
    /// The caller must guarantee that the value is valid for the provided `addr`
    /// and the `addr` is accessible.
    #[inline]
    pub unsafe fn raw_write<T>(addr: usize, val: T) {
        let ptr = ptr::with_exposed_provenance_mut::<T>(addr);
        unsafe { ptr.write_volatile(val) }
    }
}

pub trait BaseAddress {
    fn base_address(&self) -> usize;
}

#[macro_export]
macro_rules! ro_bool_field {
    ($val_type:ty, $name:ident, $offset:literal, $desc:literal) => {
        #[doc = "Extract "]
        #[doc = $desc]
        #[doc = "field"]
        #[inline]
        pub const fn $name(&self) -> bool {
            $crate::ops::get_bool(self.0 as usize, $offset)
        }
    };
}

#[macro_export]
macro_rules! rw_bool_field {
    ($val_type:ty, $name:ident, $offset:literal, $desc:literal) => {
        $crate::ro_bool_field!($val_type, $name, $offset, $desc);
        $crate::__private::paste! {
          #[doc = "Update "]
          #[doc = $desc]
          #[doc = "field"]
          #[inline]
          pub const fn [<with_ $name>](self, val: bool) -> Self {
              Self($crate::ops::set_bool(self.0 as usize, $offset, val) as $val_type)
          }
        }
    };
}

#[macro_export]
macro_rules! ro_int_field {
    ($val_type:ty, $name:ident, $start:literal, $end:literal, $ty:ty, $desc:literal) => {
        #[doc = "Extract "]
        #[doc = $desc]
        #[doc = "field"]
        #[inline]
        pub const fn $name(&self) -> $ty {
            $crate::ops::get_usize(self.0 as usize, $start, $end) as $ty
        }
    };
}

#[macro_export]
macro_rules! rw_int_field {
    ($val_type:ty, $name:ident, $start:literal, $end:literal, $ty:ty, $desc:literal) => {
        $crate::ro_int_field!($val_type, $name, $start, $end, $ty, $desc);
        $crate::__private::paste! {
          #[doc = "Update "]
          #[doc = $desc]
          #[doc = "field"]
          #[inline]
          pub const fn [<with_ $name>](self, val: $ty) -> Self {
              Self($crate::ops::set_usize(self.0 as usize, $start, $end, val as usize) as $val_type)
          }
        }
    };
}

/// Safety: $enum must be fully specified for the value range of the field or
/// marked as non-inclusive.
#[macro_export]
macro_rules! ro_enum_field {
    ($val_type:ty, $name:ident, $start:literal, $end:literal, $enum:ty, $desc:literal) => {
        #[doc = "Extract "]
        #[doc = $desc]
        #[doc = "field"]
        #[inline]
        pub const fn $name(&self) -> $enum {
            let raw_val = $crate::ops::get_usize(self.0 as usize, $start, $end);
            unsafe { core::mem::transmute::<$val_type, $enum>(raw_val as $val_type) }
        }
    };
}

/// Safety: $enum must be fully specified for the value range of the field or
/// marked as non-inclusive.
#[macro_export]
macro_rules! rw_enum_field {
    ($val_type:ty, $name:ident, $start:literal, $end:literal, $enum:ty, $desc:literal) => {
        $crate::ro_enum_field!($val_type, $name, $start, $end, $enum, $desc);
        $crate::__private::paste! {
          #[doc = "Update "]
          #[doc = $desc]
          #[doc = "field"]
          #[inline]
          pub const fn [<with_ $name>](self, val: $enum) -> Self {
            let raw_val = unsafe { core::mem::transmute::<$enum, $val_type>(val) };
              Self($crate::ops::set_usize(self.0 as usize, $start, $end, raw_val as usize) as $val_type)
          }
        }
    };
}

#[macro_export]
macro_rules! ro_masked_field {
    ($name:ident, $mask:expr, $ty:ty, $desc:literal) => {
        #[doc = "Extract "]
        #[doc = $desc]
        #[doc = "field"]
        #[inline]
        pub const fn $name(&self) -> $ty {
            self.0 & $mask
        }
    };
}

#[macro_export]
macro_rules! rw_masked_field {
    ($name:ident, $mask:expr, $ty:ty, $desc:literal) => {
        $crate::ro_masked_field!($name, $mask, $ty, $desc);
        $crate::__private::paste! {
            #[doc = "Update "]
            #[doc = $desc]
            #[doc = "field"]
            #[inline]
            pub const fn [<with_ $name>](self, val: $ty) -> Self {
              Self(self.0 & !$mask | (val & $mask))
            }
        }
    };
}

#[macro_export]
macro_rules! ro_reg {
    ($name:ident, $val_type:ident, $ty:ty, $addr:expr, $doc:literal) => {
        #[doc = $doc]
        pub struct $name;
        impl $name {
            const ADDR: usize = $addr;
            const _ASSERT_ALIGNED: () = assert!(
                Self::ADDR.is_multiple_of(core::mem::align_of::<$ty>()),
                "Register address must be aligned to register type size"
            );

            #[inline]
            pub fn read(&self) -> $val_type {
                let () = Self::_ASSERT_ALIGNED;
                $val_type(unsafe { $crate::__private::raw_read::<$ty>(Self::ADDR) })
            }
        }
    };
}

#[macro_export]
macro_rules! rw_reg {
    ($name:ident, $val_type:ident, $ty:ty, $addr:expr, $doc:literal) => {
        #[doc = $doc]
        pub struct $name;
        impl $name {
            const ADDR: usize = $addr;
            const _ASSERT_ALIGNED: () = assert!(
                Self::ADDR.is_multiple_of(core::mem::align_of::<$ty>()),
                "Register address must be aligned to register type size"
            );

            #[inline]
            pub fn read(&self) -> $val_type {
                let () = Self::_ASSERT_ALIGNED;
                $val_type(unsafe { $crate::__private::raw_read::<$ty>(Self::ADDR) })
            }

            #[inline]
            pub fn write(&mut self, val: $val_type) {
                let () = Self::_ASSERT_ALIGNED;
                unsafe { $crate::__private::raw_write::<$ty>(Self::ADDR, val.0) }
            }
        }
    };
}

#[macro_export]
macro_rules! ro_block_reg {
    ($name:ident, $val_type:ident, $ty:ty, $addr_trait:path, $offset:expr, $doc:literal) => {
        #[doc = $doc]
        pub struct $name;
        impl $name {
            const OFFSET: usize = $offset;
            const _ASSERT_OFFSET_ALIGNED: () = assert!(
                Self::OFFSET.is_multiple_of(core::mem::align_of::<$ty>()),
                "Register offset must be aligned to register type size"
            );

            #[inline]
            pub fn read<A: $addr_trait>(&self, addr: &A) -> $val_type {
                let () = Self::_ASSERT_OFFSET_ALIGNED;
                $val_type(unsafe {
                    $crate::__private::raw_read::<$ty>(addr.base_address() + Self::OFFSET)
                })
            }
        }
    };
}

#[macro_export]
macro_rules! rw_block_reg {
    ($name:ident, $val_type:ident, $ty:ty, $addr_trait:path, $offset:expr, $doc:literal) => {
        #[doc = $doc]
        pub struct $name;
        impl $name {
            const OFFSET: usize = $offset;
            const _ASSERT_OFFSET_ALIGNED: () = assert!(
                Self::OFFSET.is_multiple_of(core::mem::align_of::<$ty>()),
                "Register offset must be aligned to register type size"
            );

            #[inline]
            pub fn read<A: $addr_trait>(&self, addr: &A) -> $val_type {
                let () = Self::_ASSERT_OFFSET_ALIGNED;
                $val_type(unsafe {
                    $crate::__private::raw_read::<$ty>(addr.base_address() + Self::OFFSET)
                })
            }

            #[inline]
            pub fn write<A: $addr_trait>(&mut self, addr: &A, val: $val_type) {
                let () = Self::_ASSERT_OFFSET_ALIGNED;
                unsafe {
                    $crate::__private::raw_write::<$ty>(addr.base_address() + Self::OFFSET, val.0)
                }
            }
        }
    };
}

pub mod ops {
    #[must_use]
    #[inline]
    pub const fn mask(start: usize, end: usize) -> usize {
        let length = end - start + 1;
        if length == pw_cast::cast!(usize::BITS => usize) {
            // Special case full mask to keep shifting logic below from overflowing.
            usize::MAX
        } else {
            ((1usize << length) - 1) << start
        }
    }

    #[must_use]
    #[inline]
    pub const fn mask_u32(start: u32, end: u32) -> u32 {
        let length = end - start + 1;
        if length == u32::BITS {
            // Special case full mask to keep shifting logic below from overflowing.
            u32::MAX
        } else {
            ((1u32 << length) - 1) << start
        }
    }

    #[must_use]
    #[inline]
    pub const fn get_bool(value: usize, bit: usize) -> bool {
        (value >> bit) & 0x1 == 0x1
    }

    #[must_use]
    #[inline]
    pub const fn set_bool(value: usize, bit: usize, field_value: bool) -> usize {
        value & !(1 << bit) | (pw_cast::cast!(field_value => usize) << bit)
    }

    #[must_use]
    #[inline]
    pub const fn get_u32(value: u32, start: u32, end: u32) -> u32 {
        (value & mask_u32(start, end)) >> start
    }

    #[must_use]
    #[inline]
    pub const fn set_u32(value: u32, start: u32, end: u32, field_value: u32) -> u32 {
        let mask = mask_u32(start, end);
        (value & !mask) | ((field_value << start) & mask)
    }

    #[must_use]
    #[inline]
    pub const fn get_usize(value: usize, start: usize, end: usize) -> usize {
        (value & mask(start, end)) >> start
    }

    #[must_use]
    #[inline]
    pub const fn set_usize(value: usize, start: usize, end: usize, field_value: usize) -> usize {
        let mask = mask(start, end);
        (value & !mask) | ((field_value << start) & mask)
    }
}

#[cfg(test)]
mod tests {
    use unittest::test;

    use super::*;

    #[test]
    fn mask_calculated_correctly() -> unittest::Result<()> {
        unittest::assert_eq!(ops::mask(8, 15), 0x0000_ff00);
        Ok(())
    }

    #[test]
    fn get_bool_extracts_correct_value() -> unittest::Result<()> {
        unittest::assert_false!(ops::get_bool(0x0000_0100, 7));
        unittest::assert_true!(ops::get_bool(0x0000_0100, 8));
        unittest::assert_false!(ops::get_bool(0x0000_0100, 9));
        Ok(())
    }

    #[test]
    fn set_bool_preserved_unmasked_value() -> unittest::Result<()> {
        unittest::assert_eq!(ops::set_bool(0xffff_ffff, 16, false), 0xfffe_ffff);
        Ok(())
    }

    #[test]
    fn get_u32_extracts_correct_value() -> unittest::Result<()> {
        unittest::assert_eq!(ops::get_u32(0x5555_aa55, 8, 15), 0xaa);
        Ok(())
    }

    #[test]
    fn set_u32_preserves_unmasked_value() -> unittest::Result<()> {
        unittest::assert_eq!(ops::set_u32(0x5555_5555, 8, 15, 0xaa), 0x5555_aa55);
        Ok(())
    }

    #[test]
    fn single_bit_get_u32_extracts_correct_value() -> unittest::Result<()> {
        unittest::assert_eq!(ops::get_u32(0x0001_0000, 16, 16), 0b1);
        Ok(())
    }

    #[test]
    fn single_bit_set_u32_sets_correct_value() -> unittest::Result<()> {
        unittest::assert_eq!(ops::set_u32(0xffff_ffff, 16, 16, 0b0), 0xfffe_ffff);
        Ok(())
    }

    #[test]
    fn get_usize_extracts_correct_value() -> unittest::Result<()> {
        #[cfg(target_pointer_width = "64")]
        unittest::assert_eq!(ops::get_usize(0x5555_aa55_5555_5555, 8 + 32, 15 + 32), 0xaa);
        #[cfg(target_pointer_width = "32")]
        unittest::assert_eq!(ops::get_usize(0x5555_aa55, 8, 15), 0xaa);
        Ok(())
    }

    #[test]
    fn set_usize_preserves_unmasked_value() -> unittest::Result<()> {
        #[cfg(target_pointer_width = "64")]
        unittest::assert_eq!(
            ops::set_usize(0x5555_5555_5555_5555, 8 + 32, 15 + 32, 0xaa),
            0x5555_aa55_5555_5555
        );
        #[cfg(target_pointer_width = "32")]
        unittest::assert_eq!(ops::set_usize(0x5555_5555, 8, 15, 0xaa), 0x5555_aa55);
        Ok(())
    }

    #[test]
    fn single_bit_get_usize_extracts_correct_value() -> unittest::Result<()> {
        #[cfg(target_pointer_width = "64")]
        unittest::assert_eq!(ops::get_usize(0x0001_0000_0000_0000, 48, 48), 0b1);
        #[cfg(target_pointer_width = "32")]
        unittest::assert_eq!(ops::get_usize(0x0001_0000, 16, 16), 0b1);
        Ok(())
    }

    #[test]
    fn single_bit_set_usize_sets_correct_value() -> unittest::Result<()> {
        #[cfg(target_pointer_width = "64")]
        unittest::assert_eq!(
            ops::set_usize(0xffff_ffff_ffff_ffff, 48, 48, 0b0),
            0xfffe_ffff_ffff_ffff
        );
        #[cfg(target_pointer_width = "32")]
        unittest::assert_eq!(ops::set_usize(0xffff_ffff, 16, 16, 0b0), 0xfffe_ffff);
        Ok(())
    }
}
