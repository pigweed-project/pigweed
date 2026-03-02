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

use std::fs;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;

use anyhow::{Context, Result, anyhow};
use clap::{Args, Parser, Subcommand};
use minijinja::{Environment, State};
use serde::Serialize;
use serde::de::DeserializeOwned;

pub mod mpu_validation;
pub mod system_config;

use system_config::ObjectConfig::Interrupt;
use system_config::{InterruptTableConfig, MemoryMapping, MemoryMappingType, SystemConfig};

#[derive(Debug, Parser)]
pub struct Cli {
    #[command(flatten)]
    common_args: CommonArgs,
    #[command(subcommand)]
    command: Command,
}

#[derive(Args, Debug)]
pub struct CommonArgs {
    #[arg(long, required(true))]
    config: PathBuf,
    #[arg(long, required(true))]
    output: PathBuf,
    #[arg(
        long("template"),
        value_name = "NAME=PATH",
        value_parser = parse_template,
        action = clap::ArgAction::Append
    )]
    templates: Vec<(String, PathBuf)>,
    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    userspace: bool,
}

fn parse_template(s: &str) -> Result<(String, PathBuf), String> {
    s.split_once('=')
        .map(|(name, path)| (name.to_string(), path.into()))
        .ok_or_else(|| format!("invalid template format: '{s}'. Expected NAME=PATH."))
}

#[derive(Subcommand, Debug)]
enum Command {
    RenderTargetTemplate,
    RenderAppTemplate(AppLinkerScriptArgs),
}

#[derive(Args, Debug)]
pub struct AppLinkerScriptArgs {
    #[arg(long, required = true)]
    pub app_name: String,
}

pub trait ArchConfigInterface {
    fn get_arch_crate_name(&self) -> &'static str;
    fn get_start_fn_address(&self, flash_start_address: u64) -> u64;
    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()>;
    fn get_interrupt_table_link_section(&self) -> Option<String>;
    /// Validate memory layout for MPU compatibility.
    fn validate_mpu(&self, _config: &system_config::BaseConfig) -> Result<()> {
        Ok(()) // Default: no MPU validation
    }
}

pub fn parse_config<A: ArchConfigInterface + DeserializeOwned>(
    cli: &Cli,
) -> Result<system_config::SystemConfig<A>> {
    let json5_str =
        fs::read_to_string(&cli.common_args.config).context("Failed to read config file")?;
    let config: SystemConfig<A> =
        serde_json5::from_str(&json5_str).context("Failed to parse config file")?;
    Ok(config)
}

const FLASH_ALIGNMENT: u64 = 4;
const RAM_ALIGNMENT: u64 = 8;

impl ArchConfigInterface for system_config::Armv8MConfig {
    fn get_arch_crate_name(&self) -> &'static str {
        "arch_arm_cortex_m"
    }

    fn get_start_fn_address(&self, flash_start_address: u64) -> u64 {
        // On Armv8M, the +1 is to denote thumb mode.
        flash_start_address + 1
    }

    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()> {
        for app in &mut config.apps {
            // Add a ReadOnlyExecutable mapping for the kernel's code into userspace
            // to allow the cortex_m's `svc_return` to drop privaldge and still
            // be executable.
            //
            // TODO: https://pwbug.dev/465500606 - Isolate `svc_return` into its own section
            // to allow selectively mapping it into userspace instead of the whole kernel.
            app.process.memory_mappings.insert(
                0,
                MemoryMapping {
                    name: "kernel_code".to_string(),
                    ty: MemoryMappingType::ReadOnlyExecutable,
                    start_address: config.kernel.flash_start_address,
                    size_bytes: config.kernel.flash_size_bytes,
                },
            );
        }

        for app in &config.apps {
            for mapping in &app.process.memory_mappings {
                if mapping.start_address % 32 != 0 {
                    return Err(anyhow!(
                        "Unaligned memory mapping: application {}'s memory mapping {}'s start address ({:#10x}) must be aligned to 32 bytes",
                        app.name,
                        mapping.name,
                        mapping.start_address,
                    ));
                }
                if mapping.size_bytes % 32 != 0 {
                    return Err(anyhow!(
                        "Unaligned memory mapping: application {}'s memory mapping {}'s size ({}) must be aligned to 32 bytes",
                        app.name,
                        mapping.name,
                        mapping.size_bytes,
                    ));
                }
            }
        }
        Ok(())
    }

    fn get_interrupt_table_link_section(&self) -> Option<String> {
        Some(".vector_table.interrupts".to_string())
    }
}

impl ArchConfigInterface for system_config::RiscVConfig {
    fn get_arch_crate_name(&self) -> &'static str {
        "arch_riscv"
    }

    fn get_start_fn_address(&self, flash_start_address: u64) -> u64 {
        flash_start_address
    }

    fn calculate_and_validate_config(
        &mut self,
        _config: &mut system_config::BaseConfig,
    ) -> Result<()> {
        Ok(())
    }

    fn get_interrupt_table_link_section(&self) -> Option<String> {
        None
    }
}

pub struct SystemGenerator<'a, A: ArchConfigInterface> {
    cli: Cli,
    config: system_config::SystemConfig<A>,
    env: Environment<'a>,
}

impl<'a, A: ArchConfigInterface + Serialize> SystemGenerator<'a, A> {
    pub fn new(cli: Cli, config: system_config::SystemConfig<A>) -> Result<Self> {
        let mut instance = Self {
            cli,
            config,
            env: Environment::new(),
        };

        instance.env.add_filter("hex", hex);
        for (name, path) in instance.cli.common_args.templates.clone() {
            let template = fs::read_to_string(path)?;
            instance.env.add_template_owned(name, template)?;
        }

        instance.populate_addresses();
        // This must be called after populate_addresses.
        instance.populate_memory_mappings();
        instance.populate_interrupt_table()?;

        instance.config.base.userspace = instance.cli.common_args.userspace;

        // Calculate and validate config after the populations above.
        instance.config.calculate_and_validate()?;

        // Run architecture-specific MPU compatibility validation.
        instance.config.arch.validate_mpu(&instance.config.base)?;

        Ok(instance)
    }

    pub fn generate(&mut self) -> Result<()> {
        let out_str = match &self.cli.command {
            Command::RenderTargetTemplate => self.render_system()?,
            Command::RenderAppTemplate(args) => self.render_app_linker_script(&args.app_name)?,
        };

        let mut file = File::create(&self.cli.common_args.output)?;
        file.write_all(out_str.as_bytes())
            .context("Failed to write output")
    }

    fn render_system(&self) -> Result<String> {
        let template = self.env.get_template("system")?;
        match template.render(&self.config) {
            Ok(str) => Ok(str),
            Err(e) => Err(anyhow!(e)),
        }
    }

    fn render_app_linker_script(&self, app_name: &String) -> Result<String> {
        let template = self.env.get_template("app")?;
        let app = self
            .config
            .base
            .apps
            .iter()
            .find(|a| a.name == *app_name)
            .ok_or_else(|| anyhow!("Unable to find app \"{app_name}\" in system manifest"))?;
        match template.render(app) {
            Ok(str) => Ok(str),
            Err(e) => Err(anyhow!(e)),
        }
    }

    #[must_use]
    fn align(value: u64, alignment: u64) -> u64 {
        debug_assert!(alignment.is_power_of_two());
        (value + alignment - 1) & !(alignment - 1)
    }

    fn populate_addresses(&mut self) {
        // Stack the apps after the kernel in flash and ram.
        // TODO: davidroth - remove the requirement of setting the size of
        // flash, and instead calculate it based on code size.
        let mut next_flash_start_address =
            self.config.base.kernel.flash_start_address + self.config.base.kernel.flash_size_bytes;
        next_flash_start_address = Self::align(next_flash_start_address, FLASH_ALIGNMENT);
        let mut next_ram_start_address =
            self.config.base.kernel.ram_start_address + self.config.base.kernel.ram_size_bytes;
        next_ram_start_address = Self::align(next_ram_start_address, RAM_ALIGNMENT);

        self.config.base.arch_crate_name = self.config.arch.get_arch_crate_name();

        for app in self.config.base.apps.iter_mut() {
            app.flash_start_address = next_flash_start_address;
            next_flash_start_address = Self::align(
                app.flash_start_address + app.flash_size_bytes,
                FLASH_ALIGNMENT,
            );

            app.ram_start_address = next_ram_start_address;
            next_ram_start_address =
                Self::align(app.ram_start_address + app.ram_size_bytes, RAM_ALIGNMENT);

            app.start_fn_address = self
                .config
                .arch
                .get_start_fn_address(app.flash_start_address);

            app.initial_sp = app.ram_start_address + app.ram_size_bytes;
        }
    }

    fn populate_memory_mappings(&mut self) {
        for app in self.config.base.apps.iter_mut() {
            app.process.memory_mappings.insert(
                0,
                MemoryMapping {
                    name: "flash".to_string(),
                    ty: MemoryMappingType::ReadOnlyExecutable,
                    start_address: app.flash_start_address,
                    size_bytes: app.flash_size_bytes,
                },
            );
            app.process.memory_mappings.insert(
                1,
                MemoryMapping {
                    name: "ram".to_string(),
                    ty: MemoryMappingType::ReadWriteData,
                    start_address: app.ram_start_address,
                    size_bytes: app.ram_size_bytes,
                },
            );
        }
    }

    fn populate_interrupt_table(&mut self) -> Result<()> {
        // Add any interrupts handled by interrupt objects.
        for app in &mut self.config.base.apps {
            let app_name = &app.name;
            for object in &mut app.process.objects {
                let object_name = object.name().to_string();
                let &mut Interrupt(ref mut interrupt_config) = object else {
                    continue;
                };

                // Allow defining interrupt objects without requiring an interrupt_table
                // to also be defined in the config.
                if self.config.base.kernel.interrupt_table.is_none() {
                    self.config.base.kernel.interrupt_table = Some(InterruptTableConfig::default());
                }

                let interrupt_table = self.config.base.kernel.interrupt_table.as_mut().unwrap();

                interrupt_config.object_ref_name =
                    std::format!("INTERRUPT_OBJECT_{app_name}_{object_name}").to_uppercase();

                if interrupt_config.irqs.len() > 16 {
                    return Err(anyhow!(
                        "Interrupt object {} in app {} has more than 16 interrupts",
                        object_name,
                        app_name,
                    ));
                }

                for (index, irq_config) in interrupt_config.irqs.iter().enumerate() {
                    let irq_name = &irq_config.name;
                    let irq = irq_config.number;

                    if interrupt_table.table.contains_key(&*irq.to_string()) {
                        return Err(anyhow!(
                            "IRQ {}={} in app {} object {} already handled.",
                            irq_name,
                            irq,
                            app_name,
                            object_name
                        ));
                    }

                    let handler_name =
                        std::format!("interrupt_handler_{app_name}_{object_name}_{irq_name}")
                            .to_lowercase();

                    interrupt_table
                        .ordered_table
                        .insert(irq, handler_name.clone());

                    interrupt_config.handlers.insert(irq, handler_name.clone());

                    interrupt_config.interrupt_signal_map.insert(
                        irq_name.to_string(),
                        std::format!(
                            "Signals::INTERRUPT_{}",
                            (b'A' + u8::try_from(index).unwrap()) as char
                        ),
                    );
                }
            }
        }

        // If there's no interrupt_table defined by the user and no interrupt objects,
        // there is no need to generate an interrupt table.
        if self.config.base.kernel.interrupt_table.is_none() {
            return Ok(());
        }

        let interrupt_table = self.config.base.kernel.interrupt_table.as_mut().unwrap();

        // Add any kernel interrupt handlers defined in the config to the ordered list.
        for irq in interrupt_table.table.keys() {
            interrupt_table
                .ordered_table
                // Use the safe wrapper handler to keep the table elements safe.
                .insert(
                    irq.parse::<u32>().unwrap(),
                    std::format!("interrupt_handler_{irq}"),
                );
        }

        // Calculate the size of the interrupt table, which is the highest handled IRQ + 1
        interrupt_table.table_size = interrupt_table
            .ordered_table
            .keys()
            .max()
            .map(|max_irq| max_irq + 1)
            .unwrap_or(0) as usize;

        interrupt_table.link_section = self.config.arch.get_interrupt_table_link_section();

        Ok(())
    }
}

// Custom filters
#[must_use]
pub fn hex(_state: &State, value: usize) -> String {
    format!("{value:#x}")
}
