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

use anyhow::{Result, anyhow};
use clap::Parser;
use serde::{Deserialize, Serialize};
use system_generator::{ArchConfigInterface, Cli, SystemGenerator, system_config};

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type")]
#[serde(rename_all = "lowercase")]
pub enum ArchConfig {
    Armv8M(system_config::Armv8MConfig),
    RiscV(system_config::RiscVConfig),
}

// SystemGenerator supports Armv8m and RiscV by default.
// New architectures which are implemented out of tree can
// implement ArchConfigInterface with all required architectures.
impl ArchConfigInterface for ArchConfig {
    fn get_arch_crate_name(&self) -> &'static str {
        match self {
            ArchConfig::Armv8M(config) => config.get_arch_crate_name(),
            ArchConfig::RiscV(config) => config.get_arch_crate_name(),
        }
    }

    fn get_start_fn_address(&self, flash_start_address: u64) -> u64 {
        match self {
            ArchConfig::Armv8M(config) => config.get_start_fn_address(flash_start_address),
            ArchConfig::RiscV(config) => config.get_start_fn_address(flash_start_address),
        }
    }

    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()> {
        match self {
            ArchConfig::Armv8M(arch_config) => arch_config.calculate_and_validate_config(config),
            ArchConfig::RiscV(arch_config) => arch_config.calculate_and_validate_config(config),
        }
    }

    fn get_interrupt_table_link_section(&self) -> Option<String> {
        match self {
            ArchConfig::Armv8M(config) => config.get_interrupt_table_link_section(),
            ArchConfig::RiscV(config) => config.get_interrupt_table_link_section(),
        }
    }

    fn validate_mpu(&self, config: &system_config::BaseConfig) -> Result<()> {
        match self {
            ArchConfig::Armv8M(arch_config) => arch_config.validate_mpu(config),
            ArchConfig::RiscV(arch_config) => arch_config.validate_mpu(config),
        }
    }
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let config = system_generator::parse_config::<ArchConfig>(&cli)?;
    let mut system_generator = SystemGenerator::new(cli, config)?;
    system_generator.generate().map_err(|e| anyhow!("{e}"))
}
