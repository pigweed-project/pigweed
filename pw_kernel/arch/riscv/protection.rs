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

use kernel_config::{KernelConfig, RiscVKernelConfigInterface as _};
use memory_config::{MemoryRegion, MemoryRegionType};

use crate::regs::pmp::*;

/// RISC-V memory configuration
///
/// Represents the full configuration of RISC-V memory configuration through
/// the PMP block.
#[derive(Clone)]
pub struct MemoryConfig {
    pmp_config: PmpConfig<{ KernelConfig::PMP_ENTRIES }>,
    regions: &'static [MemoryRegion],
}

impl MemoryConfig {
    /// Create a new `MemoryConfig` in a `const` context
    ///
    /// # Panics
    /// Will panic if the current target's PMP will does not have enough entries
    /// to represent the provided `regions`.
    #[must_use]
    pub const fn const_new(regions: &'static [MemoryRegion]) -> Self {
        match PmpConfig::new(regions) {
            Ok(cfg) => Self {
                pmp_config: cfg,
                regions,
            },
            Err(_) => panic!("Cannot create PMP memory config"),
        }
    }

    /// Write this memory configuration to the PMP registers.
    ///
    /// # Safety
    /// Caller must ensure that it is safe and sound to update the PMP with this
    /// memory config.
    pub unsafe fn write(&self) {
        unsafe {
            // We clear first so the following write can't create an
            // intermediate invalid state (Consider zeroing the lower
            // address register of a TOR region, which could create a
            // region from address 0 to whatever the top-of-range is that
            // may be inaccessible).
            self.pmp_config.clear();
            self.pmp_config.write();
        }
    }

    pub fn dump(&self) {
        self.pmp_config.dump();
    }

    pub fn dump_current(&self) {
        unsafe {
            let pmp_config = PmpConfig::<{ KernelConfig::PMP_ENTRIES }>::read();
            pmp_config.dump();
        }
    }
}

impl memory_config::MemoryConfig for MemoryConfig {
    const KERNEL_THREAD_MEMORY_CONFIG: Self = Self::const_new(KernelConfig::KERNEL_MEMORY_REGIONS);

    fn range_has_access(
        &self,
        access_type: MemoryRegionType,
        start_addr: usize,
        end_addr: usize,
    ) -> bool {
        let validation_region = MemoryRegion::new(access_type, start_addr, end_addr);
        MemoryRegion::regions_have_access(self.regions, &validation_region)
    }
}

#[allow(dead_code)]
pub unsafe extern "C" fn memory_config_write(memory_config: *const MemoryConfig) {
    unsafe {
        (*memory_config).write();
    }
}
