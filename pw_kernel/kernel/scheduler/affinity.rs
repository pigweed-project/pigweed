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

use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_status::{Error, Result};

/// Logical identifier for a CPU core in a system with up to `MAX_CORES` cores.
#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GenericCoreId<const MAX_CORES: usize>(usize);

/// Logical identifier for a CPU core.
pub type CoreId = GenericCoreId<{ KernelConfig::MAX_CORES }>;

impl<const MAX_CORES: usize> GenericCoreId<MAX_CORES> {
    const _ASSERT_VALID_MAX_CORES: () = assert!(MAX_CORES >= 1);

    /// The primary (bootstrap) core identifier (`CoreId(0)`).
    pub const ZERO: Self = {
        let () = Self::_ASSERT_VALID_MAX_CORES;
        Self(0)
    };

    /// Constructs a [`GenericCoreId`] for the given logical core index.
    ///
    /// # Panics
    ///
    /// Debug-asserts that `id < MAX_CORES`.
    #[must_use]
    #[inline]
    pub fn new(id: usize) -> Self {
        let () = Self::_ASSERT_VALID_MAX_CORES;
        pw_assert::debug_assert!(id < MAX_CORES);
        Self(id)
    }

    /// Returns the zero-based logical core index.
    #[must_use]
    #[inline]
    pub const fn as_usize(self) -> usize {
        self.0
    }
}

/// Thread CPU affinity mask for a system with up to `MAX_CORES` cores.
///
/// Represents the set of CPU cores on which a thread is permitted to execute.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct GenericAffinityMask<const MAX_CORES: usize>(u32);

/// Thread CPU affinity mask.
pub type AffinityMask = GenericAffinityMask<{ KernelConfig::MAX_CORES }>;

impl<const MAX_CORES: usize> GenericAffinityMask<MAX_CORES> {
    const BITS: usize = pw_cast::cast!(u32::BITS => usize);
    const _ASSERT_VALID_MAX_CORES: () = {
        assert!(MAX_CORES >= 1);
        assert!(MAX_CORES <= Self::BITS);
    };

    /// Constructs an affinity mask allowing the thread to run on any configured core.
    #[must_use]
    #[inline]
    pub const fn all() -> Self {
        let () = Self::_ASSERT_VALID_MAX_CORES;
        Self(u32::MAX >> (Self::BITS - MAX_CORES))
    }

    /// Constructs an affinity mask pinning the thread exclusively to a single core.
    #[must_use]
    #[inline]
    pub const fn pinned_to_core(core: GenericCoreId<MAX_CORES>) -> Self {
        let () = Self::_ASSERT_VALID_MAX_CORES;
        Self(1 << core.as_usize())
    }

    /// Constructs an affinity mask from a raw bitmask.
    ///
    /// Returns `Err(Error::InvalidArgument)` if `mask` is zero or sets any bits
    /// outside the configured cores (`>= 1 << MAX_CORES`).
    #[inline]
    pub const fn from_raw(mask: u32) -> Result<Self> {
        // A valid affinity mask must include at least one core (`mask != 0`)
        // and must not set any bits outside the configured cores (`Self::all()`,
        // which is `0b1` on uniprocessor builds where `MAX_CORES == 1`).
        if mask != 0 && (mask & !Self::all().0) == 0 {
            Ok(Self(mask))
        } else {
            Err(Error::InvalidArgument)
        }
    }

    /// Returns `true` if the thread is permitted to execute on `core`.
    #[must_use]
    #[inline]
    pub const fn contains(&self, core: GenericCoreId<MAX_CORES>) -> bool {
        (self.0 & (1 << core.as_usize())) != 0
    }

    /// Returns the lowest-numbered core permitted by this affinity mask.
    #[must_use]
    #[inline]
    pub const fn lowest_core(&self) -> GenericCoreId<MAX_CORES> {
        GenericCoreId(pw_cast::cast!(self.0.trailing_zeros() => usize))
    }
}

#[cfg(test)]
mod tests {
    use unittest::test;

    use super::*;

    /// Verifies `CoreId` and `AffinityMask` behavior on a single-core (`MAX_CORES = 1`) system.
    #[test]
    fn uniprocessor_core_id_and_affinity_mask() -> unittest::Result<()> {
        type UpCoreId = GenericCoreId<1>;
        type UpMask = GenericAffinityMask<1>;

        unittest::assert_eq!(UpCoreId::ZERO.as_usize(), 0);
        unittest::assert_eq!(UpCoreId::new(0), UpCoreId::ZERO);

        let all = UpMask::all();
        unittest::assert_true!(all.contains(UpCoreId::ZERO));
        unittest::assert_eq!(all.lowest_core(), UpCoreId::ZERO);
        unittest::assert_eq!(UpMask::pinned_to_core(UpCoreId::ZERO), all);

        // Only `0b1` is a valid raw mask when `MAX_CORES == 1`.
        unittest::assert_eq!(UpMask::from_raw(0), Err(Error::InvalidArgument));
        unittest::assert_eq!(UpMask::from_raw(0b1), Ok(all));
        unittest::assert_eq!(UpMask::from_raw(0b10), Err(Error::InvalidArgument));
        unittest::assert_eq!(UpMask::from_raw(0b11), Err(Error::InvalidArgument));
        unittest::assert_eq!(UpMask::from_raw(u32::MAX), Err(Error::InvalidArgument));
        Ok(())
    }

    /// Verifies single-core pinning, multi-core subsets, and bounds checks for `MAX_CORES = 4`.
    #[test]
    fn quad_core_affinity_mask_subsets_and_bounds() -> unittest::Result<()> {
        type QuadCoreId = GenericCoreId<4>;
        type QuadMask = GenericAffinityMask<4>;

        let all = QuadMask::all();
        unittest::assert_eq!(all.lowest_core(), QuadCoreId::ZERO);
        unittest::assert_eq!(QuadMask::from_raw(0b1111), Ok(all));

        // Each core's pinned mask contains only that core.
        for i in 0..4 {
            let core = QuadCoreId::new(i);
            unittest::assert_eq!(core.as_usize(), i);
            unittest::assert_true!(all.contains(core));

            let single = QuadMask::pinned_to_core(core);
            unittest::assert_true!(single.contains(core));
            unittest::assert_eq!(single.lowest_core(), core);
            for other in 0..4 {
                if other != i {
                    unittest::assert_false!(single.contains(QuadCoreId::new(other)));
                }
            }
        }

        // Multi-core subset (cores 1 and 3): lowest core is 1.
        let subset = QuadMask::from_raw(0b1010).unwrap();
        unittest::assert_false!(subset.contains(QuadCoreId::new(0)));
        unittest::assert_true!(subset.contains(QuadCoreId::new(1)));
        unittest::assert_false!(subset.contains(QuadCoreId::new(2)));
        unittest::assert_true!(subset.contains(QuadCoreId::new(3)));
        unittest::assert_eq!(subset.lowest_core(), QuadCoreId::new(1));

        // Empty and out-of-bounds bitmasks (`>= 1 << 4`) must be rejected.
        unittest::assert_eq!(QuadMask::from_raw(0), Err(Error::InvalidArgument));
        unittest::assert_eq!(QuadMask::from_raw(1 << 4), Err(Error::InvalidArgument));
        unittest::assert_eq!(
            QuadMask::from_raw(0b1111 | (1 << 4)),
            Err(Error::InvalidArgument)
        );
        unittest::assert_eq!(QuadMask::from_raw(u32::MAX), Err(Error::InvalidArgument));
        Ok(())
    }

    /// Verifies that `MAX_CORES = 32` handles the full 32-bit mask without shift overflow.
    #[test]
    fn max_32_cores_full_bitmask_boundary() -> unittest::Result<()> {
        type MaxCoreId = GenericCoreId<32>;
        type MaxMask = GenericAffinityMask<32>;

        let all = MaxMask::all();
        unittest::assert_eq!(MaxMask::from_raw(u32::MAX), Ok(all));
        unittest::assert_eq!(all.lowest_core(), MaxCoreId::ZERO);
        unittest::assert_eq!(MaxMask::from_raw(0), Err(Error::InvalidArgument));

        // Pinning to the highest core (`CoreId(31)`) sets the MSB.
        let high_core = MaxCoreId::new(31);
        let high_mask = MaxMask::pinned_to_core(high_core);
        unittest::assert_eq!(MaxMask::from_raw(1u32 << 31), Ok(high_mask));
        unittest::assert_true!(high_mask.contains(high_core));
        unittest::assert_false!(high_mask.contains(MaxCoreId::ZERO));
        unittest::assert_eq!(high_mask.lowest_core(), high_core);
        Ok(())
    }
}
