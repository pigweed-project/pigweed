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

//! MPU-aware memory layout validation for the system generator.
//!
//! This module provides common types for MPU validation. Architecture-specific
//! validation is implemented via the [`ArchConfigInterface::validate_mpu`] trait
//! method in each architecture's config module.
//!
//! # Adding a new architecture validator
//!
//! 1. Create a validation module (e.g., `mpu_validation/pmsav8.rs`) with:
//!    - Region calculation functions for the architecture's MPU constraints
//!    - A `validate_<arch>_layout()` function returning `Vec<MpuIssue>`
//!    - Unit tests covering relevant scenarios (alignment, overlap, etc.)
//!
//! 2. Add `pub mod <arch>;` to this file (mod.rs) to expose the module
//!
//! 3. Implement `ArchConfigInterface::validate_mpu()` for your architecture's
//!    config type in `lib.rs`, calling your validation function
//!
//! # Current implementations
//!
//! - `pmsav7` - ARMv7-M PMSAv7 (power-of-2 regions with subregion disable)
//!
//! [`ArchConfigInterface::validate_mpu`]: crate::ArchConfigInterface::validate_mpu

use std::fmt;

use serde::{Deserialize, Serialize};

/// MPU validation severity level.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum MpuValidationMode {
    /// Fail the build if any MPU compatibility issues are detected.
    Strict,
    /// Emit warnings for MPU compatibility issues but continue.
    #[default]
    Warn,
    /// Silent - no output for MPU issues.
    Permissive,
}

/// A detected MPU compatibility issue.
#[derive(Clone, Debug)]
pub struct MpuIssue {
    /// True for errors (overlap), false for warnings (bloat)
    pub is_error: bool,
    /// Human-readable description
    pub message: String,
    /// Name of the affected region
    pub region_name: String,
    /// Suggested fix, if available
    pub suggestion: Option<String>,
}

impl fmt::Display for MpuIssue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)?;
        if let Some(suggestion) = &self.suggestion {
            write!(f, "\n  suggestion: {}", suggestion)?;
        }
        Ok(())
    }
}

/// A memory region for validation purposes.
#[derive(Clone, Debug)]
pub struct MemoryRegion {
    pub name: String,
    pub start: u64,
    pub end: u64,
    pub is_kernel: bool,
    pub is_executable: bool,
}

impl MemoryRegion {
    #[must_use]
    pub fn size(&self) -> u64 {
        self.end - self.start
    }
}
