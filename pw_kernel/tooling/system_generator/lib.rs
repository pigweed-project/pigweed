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
pub mod validation;

use mpu_validation::MpuValidationMode;
use mpu_validation::pmsav7::validate_pmsav7_layout;
use system_config::ObjectConfig::Interrupt;
use system_config::{
    InterruptTableConfig, MemoryMapping, MemoryMappingType, ObjectConfig, ProcessObjectConfig,
    SystemConfig,
};

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
    RenderAppTemplate(AppArgs),
}

#[derive(Args, Debug)]
#[group(required = true, multiple = false)]
pub struct AppArgs {
    #[arg(long)]
    pub app_name: Option<String>,

    #[arg(long)]
    pub process_name: Option<String>,
}

enum AppType {
    SingleProcess { app_name: String },
    MultiProcess { process_name: String },
}

impl From<&AppArgs> for AppType {
    fn from(args: &AppArgs) -> AppType {
        if let Some(ref app_name) = args.app_name {
            return AppType::SingleProcess {
                app_name: app_name.clone(),
            };
        }
        if let Some(ref process_name) = args.process_name {
            return AppType::MultiProcess {
                process_name: process_name.clone(),
            };
        }
        unreachable!("AppArgs group requires either app_name or process_name");
    }
}

/// Context defining the specific app and process being passed to the app template for rendering.
#[derive(Serialize)]
pub struct AppRenderContext<'a, A: ArchConfigInterface + Serialize> {
    pub arch: &'a A,
    pub app: &'a system_config::AppConfig,
    pub process: &'a system_config::ProcessConfig,
    pub is_multi_process_app: bool,
}

pub trait ArchConfigInterface {
    fn get_arch_crate_name(&self) -> &'static str;
    fn get_start_fn_address(&self, flash_start_address: u64) -> u64;
    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()>;
    fn get_interrupt_table_link_section(&self) -> Option<String>;

    /// Returns true if the interrupt table entries should have no arguments.
    ///
    /// Vectored interrupt controllers (like NVIC on ARM) require the entries to
    /// have no arguments because the hardware jumps directly to the handler.
    /// Non-vectored controllers (or architectures where all interrupts go to a
    /// single handler like RISC-V) need to capture the `from_userspace` flag
    /// earlier in the call chain and pass it to the specific interrupt handler.
    fn bare_interrupt_table_entries(&self) -> bool;

    /// Validate memory layout for MPU compatibility.
    fn validate_mpu(&self, _config: &system_config::BaseConfig) -> Result<()> {
        Ok(()) // Default: no MPU validation
    }
    /// Get required alignment for a process RAM region.
    fn get_ram_alignment(&self, _size: u64) -> u64 {
        32
    }
    /// Get minimum RAM alignment.
    fn get_minimum_ram_alignment(&self) -> u64 {
        32
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

/// Map the kernel's flash region as `ReadOnlyExecutable` into every app's
/// address space. This is required so that `svc_return` remains executable
/// after the privilege drop on Cortex-M.
///
/// TODO: https://pwbug.dev/465500606 - Isolate `svc_return` into its own
/// section to allow mapping only that instead of the whole kernel flash.
fn cortex_m_add_kernel_code_mapping(config: &mut system_config::BaseConfig) {
    for app in &mut config.apps {
        for process in &mut app.processes {
            process.memory_mappings.insert(
                0,
                MemoryMapping {
                    name: "kernel_code".to_string(),
                    ty: MemoryMappingType::ReadOnlyExecutable,
                    start_address: config.kernel.flash_start_address,
                    size_bytes: config.kernel.flash_size_bytes,
                },
            );
        }
    }
}

impl ArchConfigInterface for system_config::Armv8MConfig {
    fn get_arch_crate_name(&self) -> &'static str {
        "arch_arm_cortex_m"
    }

    fn get_start_fn_address(&self, flash_start_address: u64) -> u64 {
        // +1 to set the Thumb mode bit in the reset vector.
        flash_start_address + 1
    }

    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()> {
        cortex_m_add_kernel_code_mapping(config);

        // PMSAv8 requires all region base addresses and sizes to be aligned to
        // 32 bytes (the minimum MPU region granularity).
        for app in &config.apps {
            for process in &app.processes {
                for mapping in &process.memory_mappings {
                    if mapping.start_address % 32 != 0 {
                        return Err(anyhow!(
                            "Unaligned memory mapping: process {} in application {}'s memory mapping {}'s start address ({:#10x}) must be aligned to 32 bytes",
                            process.name,
                            app.name,
                            mapping.name,
                            mapping.start_address,
                        ));
                    }
                    if mapping.size_bytes % 32 != 0 {
                        return Err(anyhow!(
                            "Unaligned memory mapping: process {} in application {}'s memory mapping {}'s size ({}) must be aligned to 32 bytes",
                            process.name,
                            app.name,
                            mapping.name,
                            mapping.size_bytes,
                        ));
                    }
                }
            }
        }

        Ok(())
    }

    fn get_interrupt_table_link_section(&self) -> Option<String> {
        Some(".vector_table.interrupts".to_string())
    }

    fn bare_interrupt_table_entries(&self) -> bool {
        true
    }
}

impl ArchConfigInterface for system_config::Armv7MConfig {
    fn get_arch_crate_name(&self) -> &'static str {
        "arch_arm_cortex_m"
    }

    fn get_start_fn_address(&self, flash_start_address: u64) -> u64 {
        // +1 to set the Thumb mode bit in the reset vector.
        flash_start_address + 1
    }

    fn calculate_and_validate_config(
        &mut self,
        config: &mut system_config::BaseConfig,
    ) -> Result<()> {
        cortex_m_add_kernel_code_mapping(config);

        // Validate power-of-2 size for Armv7-M
        for app in &config.apps {
            for process in &app.processes {
                if !process.ram_size_bytes.is_power_of_two() {
                    return Err(anyhow!(
                        "Process {} RAM size ({}) must be a power of 2 for Armv7-M",
                        process.name,
                        process.ram_size_bytes
                    ));
                }
                if process.ram_size_bytes < 32 {
                    return Err(anyhow!(
                        "Process {} RAM size ({}) must be at least 32 bytes for Armv7-M",
                        process.name,
                        process.ram_size_bytes
                    ));
                }
            }
        }

        Ok(())
    }

    fn get_ram_alignment(&self, size: u64) -> u64 {
        size
    }

    fn get_interrupt_table_link_section(&self) -> Option<String> {
        Some(".vector_table.interrupts".to_string())
    }

    fn bare_interrupt_table_entries(&self) -> bool {
        true
    }

    fn validate_mpu(&self, config: &system_config::BaseConfig) -> Result<()> {
        let kernel = &config.kernel;

        // Build app list: (name, flash_start, flash_end, ram_start, ram_end)
        let apps: Vec<_> = config
            .apps
            .iter()
            .map(|app| {
                (
                    app.name.clone(),
                    app.flash_start_address,
                    app.flash_start_address + app.flash_size_bytes,
                    app.ram_start_address,
                    app.ram_start_address + app.ram_size_bytes,
                )
            })
            .collect();

        let issues = validate_pmsav7_layout(
            kernel.flash_start_address,
            kernel.flash_start_address + kernel.flash_size_bytes,
            kernel.ram_start_address,
            kernel.ram_start_address + kernel.ram_size_bytes,
            &apps,
        );

        if issues.is_empty() {
            return Ok(());
        }

        // Report issues based on validation mode
        let mode = kernel.mpu_validation;
        let has_errors = issues.iter().any(|i| i.is_error);

        for issue in &issues {
            match mode {
                MpuValidationMode::Strict => {
                    eprintln!("error: {}", issue);
                }
                MpuValidationMode::Warn => {
                    if issue.is_error {
                        eprintln!("warning: {}", issue);
                    } else {
                        eprintln!("info: {}", issue);
                    }
                }
                MpuValidationMode::Permissive => {
                    // Silent - no output in permissive mode
                }
            }
        }

        if mode == MpuValidationMode::Strict && has_errors {
            return Err(anyhow!(
                "PMSAv7 MPU validation failed. Use mpu_validation: \"warn\" to continue anyway."
            ));
        }

        Ok(())
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

    fn bare_interrupt_table_entries(&self) -> bool {
        false
    }
}

pub struct SystemGenerator<'a, A: ArchConfigInterface> {
    cli: Cli,
    config: system_config::SystemConfig<A>,
    env: Environment<'a>,
    validators: Vec<Box<dyn crate::validation::ManifestValidator<A>>>,
}

impl<'a, A: ArchConfigInterface + Serialize> SystemGenerator<'a, A> {
    pub fn new(cli: Cli, config: system_config::SystemConfig<A>) -> Result<Self> {
        let mut instance = Self {
            cli,
            config,
            env: Environment::new(),
            validators: vec![Box::new(crate::validation::IdentifierValidator)],
        };
        instance.env.set_lstrip_blocks(true);
        instance.env.set_trim_blocks(true);

        // Run manifest validation first before rendering any templates.
        for validator in &instance.validators {
            validator.validate(&instance.config)?;
        }

        instance.env.add_filter("hex", hex);
        instance.env.add_filter("to_lower_ident", to_lower_ident);
        instance.env.add_filter("to_upper_ident", to_upper_ident);
        for (name, path) in instance.cli.common_args.templates.clone() {
            let template = fs::read_to_string(path)?;
            instance.env.add_template_owned(name, template)?;
        }

        instance.populate_addresses();
        instance.populate_process_and_thread_objects()?;
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
            Command::RenderAppTemplate(args) => self.render_app_template(args)?,
        };

        let mut file = File::create(&self.cli.common_args.output)?;
        file.write_all(out_str.as_bytes())
            .context("Failed to write output")
    }

    fn render_system(&self) -> Result<String> {
        let template = self.env.get_template("system")?;
        template
            .render(&self.config)
            .context("Could not render system template")
    }

    fn render_app_template(&self, args: &AppArgs) -> Result<String> {
        let template = self.env.get_template("app")?;
        let is_multi_process_app = args.process_name.is_some();

        let (app, process) = match AppType::from(args) {
            AppType::MultiProcess { process_name } => self
                .config
                .base
                .get_process_and_app(&process_name)
                .ok_or_else(|| {
                    anyhow!(
                        "Unable to find process `{}` in system manifest",
                        process_name
                    )
                })?,
            AppType::SingleProcess { app_name } => {
                let a = self.config.base.get_app(&app_name).ok_or_else(|| {
                    anyhow!("Unable to find app `{}` in system manifest", app_name)
                })?;
                (a, &a.processes[0])
            }
        };

        let context = AppRenderContext {
            arch: &self.config.arch,
            app,
            process,
            is_multi_process_app,
        };

        template
            .render(&context)
            .context("Could not render app template")
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
        next_ram_start_address = Self::align(
            next_ram_start_address,
            self.config.arch.get_minimum_ram_alignment(),
        );

        self.config.base.arch_crate_name = self.config.arch.get_arch_crate_name();
        self.config.base.bare_interrupt_table_entries =
            self.config.arch.bare_interrupt_table_entries();

        for app in self.config.base.apps.iter_mut() {
            app.flash_start_address = next_flash_start_address;
            next_flash_start_address = Self::align(
                app.flash_start_address + app.flash_size_bytes,
                FLASH_ALIGNMENT,
            );

            app.ram_start_address = next_ram_start_address;

            // Calculate app RAM size as sum of process RAM sizes.
            app.ram_size_bytes = app.processes.iter().map(|p| p.ram_size_bytes).sum();

            let mut current_process_ram_start = app.ram_start_address;
            for process in &mut app.processes {
                let alignment = self.config.arch.get_ram_alignment(process.ram_size_bytes);
                process.ram_start_address = Self::align(current_process_ram_start, alignment);
                current_process_ram_start = process.ram_start_address + process.ram_size_bytes;
            }

            next_ram_start_address = Self::align(
                app.ram_start_address + app.ram_size_bytes,
                self.config.arch.get_minimum_ram_alignment(),
            );

            app.start_fn_address = self
                .config
                .arch
                .get_start_fn_address(app.flash_start_address);

            app.initial_sp = app.ram_start_address + app.ram_size_bytes;
        }
    }

    fn resolve_linked_process(
        apps: &[system_config::AppConfig],
        linked_process: &str,
    ) -> Option<String> {
        for app in apps {
            if let Some(proc) = app.processes.iter().find(|pr| pr.name == linked_process) {
                return Some(proc.name.clone());
            }
        }
        None
    }

    fn populate_process_objects(
        process: &mut system_config::ProcessConfig,
        app_name: &str,
        apps: &[system_config::AppConfig],
    ) -> Result<()> {
        let process_name = &process.name;
        // Verify user-declared process objects
        for object in &mut process.objects {
            if let ObjectConfig::Process(p) = object {
                if p.name == "process" {
                    return Err(anyhow::anyhow!(
                        "Process '{}' in App '{}' manually requested a process object named 'process'. This is reserved for the process's own handle.",
                        process_name,
                        app_name
                    ));
                }

                if let Some(resolved_process) =
                    Self::resolve_linked_process(apps, &p.linked_process)
                {
                    p.process_object_name =
                        std::format!("object_{}_process", resolved_process.to_lowercase());
                } else {
                    return Err(anyhow::anyhow!(
                        "Process '{}' in App '{}' requested handle to process object '{}' linked to '{}' but no such process found in system config",
                        process_name,
                        app_name,
                        p.name,
                        p.linked_process
                    ));
                }
            }
        }

        // Add Process object for this process's own process. A fixed name is
        // used so that generic code can be written to access a process's own
        // process object.
        process
            .objects
            .push(ObjectConfig::Process(ProcessObjectConfig {
                name: "process".to_string(),
                linked_process: process.name.clone(),
                process_object_name: std::format!("object_{}_process", process.name.to_lowercase()),
            }));

        Ok(())
    }

    fn populate_thread_objects(process: &mut system_config::ProcessConfig) {
        // Save the main_thread_name for template usage.
        process.main_thread_name = Some(process.get_main_thread().name.clone());
    }

    fn populate_process_and_thread_objects(&mut self) -> Result<()> {
        let apps = self.config.base.apps.clone();

        for app in self.config.base.apps.iter_mut() {
            let app_name = &app.name;
            for process in &mut app.processes {
                Self::populate_process_objects(process, app_name, &apps)?;
                Self::populate_thread_objects(process);
            }
        }
        Ok(())
    }

    fn populate_memory_mappings(&mut self) {
        for app in self.config.base.apps.iter_mut() {
            for process in &mut app.processes {
                process.memory_mappings.insert(
                    0,
                    MemoryMapping {
                        name: "flash".to_string(),
                        ty: MemoryMappingType::ReadOnlyExecutable,
                        start_address: app.flash_start_address,
                        size_bytes: app.flash_size_bytes,
                    },
                );
                process.memory_mappings.insert(
                    1,
                    MemoryMapping {
                        name: "ram".to_string(),
                        ty: MemoryMappingType::ReadWriteData,
                        start_address: process.ram_start_address,
                        size_bytes: process.ram_size_bytes,
                    },
                );
            }
        }
    }

    fn populate_interrupt_table(&mut self) -> Result<()> {
        // Add any interrupts handled by interrupt objects.
        for app in &mut self.config.base.apps {
            let app_name = &app.name;
            for process in &mut app.processes {
                for object in &mut process.objects {
                    let object_name = object.name().to_string();
                    let &mut Interrupt(ref mut interrupt_config) = object else {
                        continue;
                    };

                    // Allow defining interrupt objects without requiring an interrupt_table
                    // to also be defined in the config.
                    if self.config.base.kernel.interrupt_table.is_none() {
                        self.config.base.kernel.interrupt_table =
                            Some(InterruptTableConfig::default());
                    }

                    let interrupt_table = self.config.base.kernel.interrupt_table.as_mut().unwrap();

                    interrupt_config.object_ref_name =
                        std::format!("INTERRUPT_OBJECT_{}_{object_name}", process.name)
                            .to_uppercase();

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

                        if interrupt_table.table.contains_key(&irq.to_string()) {
                            return Err(anyhow!(
                                "IRQ {}={} in app {} object {} already handled.",
                                irq_name,
                                irq,
                                app_name,
                                object_name
                            ));
                        }

                        let handler_name = std::format!(
                            "interrupt_handler_{}_{object_name}_{irq_name}",
                            process.name
                        )
                        .to_lowercase();

                        interrupt_table
                            .combined_ordered_table
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
        }

        // If there's no interrupt_table defined by the user and no interrupt objects,
        // there is no need to generate an interrupt table.
        if self.config.base.kernel.interrupt_table.is_none() {
            return Ok(());
        }

        let interrupt_table = self.config.base.kernel.interrupt_table.as_mut().unwrap();

        // Add any kernel interrupt handlers defined in the config to the ordered list.
        for irq_str in interrupt_table.table.keys() {
            let irq = irq_str.parse::<u32>().unwrap();
            interrupt_table
                .combined_ordered_table
                // Use the safe wrapper handler to keep the table elements safe.
                .insert(irq, std::format!("interrupt_handler_{irq_str}"));
        }

        // Calculate the size of the interrupt table, which is the highest handled IRQ + 1
        interrupt_table.table_size = interrupt_table
            .combined_ordered_table
            .keys()
            .max()
            .map(|max_irq| max_irq + 1)
            .unwrap_or(0) as usize;

        interrupt_table.link_section = self.config.arch.get_interrupt_table_link_section();

        Ok(())
    }
}

//
// Custom filters
//

#[must_use]
pub fn hex(_state: &State, value: usize) -> String {
    format!("{value:#x}")
}

#[must_use]
pub fn to_lower_ident(_state: &State, value: String) -> String {
    value.to_lowercase()
}

#[must_use]
pub fn to_upper_ident(_state: &State, value: String) -> String {
    value.to_uppercase()
}
