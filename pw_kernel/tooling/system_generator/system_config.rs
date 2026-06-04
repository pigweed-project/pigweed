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

use std::collections::{BTreeMap, HashMap, HashSet};

use anyhow::{Result, anyhow};
use serde::{Deserialize, Serialize};

use crate::ArchConfigInterface;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SystemConfig<A: ArchConfigInterface> {
    pub arch: A,
    #[serde(flatten)]
    pub base: BaseConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BaseConfig {
    pub kernel: KernelConfig,
    #[serde(default)]
    pub apps: Vec<AppConfig>,
    #[serde(skip_deserializing)]
    pub arch_crate_name: &'static str,
    #[serde(skip_deserializing)]
    pub userspace: bool,
    #[serde(skip_deserializing)]
    pub bare_interrupt_table_entries: bool,
}

impl BaseConfig {
    #[must_use]
    pub fn get_app(&self, app_name: &str) -> Option<&AppConfig> {
        self.apps.iter().find(|a| a.name == app_name)
    }

    #[must_use]
    pub fn get_process_and_app(&self, process_name: &str) -> Option<(&AppConfig, &ProcessConfig)> {
        for app in &self.apps {
            if let Some(p) = app.processes.iter().find(|pr| pr.name == process_name) {
                return Some((app, p));
            }
        }
        None
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NvicConfig {
    pub vector_table_start_address: u64,
    pub vector_table_size_bytes: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Armv8MConfig {
    #[serde(flatten)]
    pub nvic: NvicConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Armv7MConfig {
    #[serde(flatten)]
    pub nvic: NvicConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RiscVConfig;

use crate::mpu_validation::MpuValidationMode;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct KernelConfig {
    pub flash_start_address: u64,
    pub flash_size_bytes: u64,
    pub ram_start_address: u64,
    pub ram_size_bytes: u64,
    pub interrupt_table: Option<InterruptTableConfig>,
    /// MPU validation mode for memory layout compatibility checking.
    /// - "strict": Fail build on any MPU issues
    /// - "warn": Emit warnings but continue (default)
    /// - "permissive": Silent
    #[serde(default)]
    pub mpu_validation: MpuValidationMode,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct InterruptTableConfig {
    /// User defined table of interrupt handlers, mapping
    /// IRQ to interrupt handler.
    pub table: BTreeMap<String, String>,
    #[serde(skip_deserializing)]
    pub table_size: usize,
    /// Complete table of all interrupt handlers in the system
    /// composed of both user defined handlers from `table` and
    /// code generated handlers.
    #[serde(skip_deserializing)]
    pub combined_ordered_table: BTreeMap<u32, String>,
    #[serde(skip_deserializing)]
    pub link_section: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ConstConfig {
    Bool { name: String, value: bool },
    Char { name: String, value: char },
    U8 { name: String, value: u8 },
    U16 { name: String, value: u16 },
    U32 { name: String, value: u32 },
    U64 { name: String, value: u64 },
    U128 { name: String, value: u128 },
    Usize { name: String, value: usize },
    I8 { name: String, value: i8 },
    I16 { name: String, value: i16 },
    I32 { name: String, value: i32 },
    I64 { name: String, value: i64 },
    I128 { name: String, value: i128 },
    Isize { name: String, value: isize },
    String { name: String, value: String },
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct AppConfig {
    pub name: String,
    pub flash_size_bytes: u64,
    #[serde(skip_deserializing)]
    pub ram_size_bytes: u64,
    pub processes: Vec<ProcessConfig>,
    #[serde(default)]
    pub constants: Vec<ConstConfig>,
    // The following fields are calculated, not defined by a user.
    // TODO: davidroth - if this becomes too un-wieldy, we should
    // split the config schema from the template structs.
    #[serde(skip_deserializing)]
    pub flash_start_address: u64,
    #[serde(skip_deserializing)]
    pub ram_start_address: u64,
    #[serde(skip_deserializing)]
    pub start_fn_address: u64,
    #[serde(skip_deserializing)]
    pub initial_sp: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProcessConfig {
    pub name: String,
    pub ram_size_bytes: u64,
    #[serde(skip_deserializing)]
    pub ram_start_address: u64,
    #[serde(skip_deserializing)]
    pub main_thread_name: Option<String>,

    #[serde(default)]
    pub memory_mappings: Vec<MemoryMapping>,

    #[serde(default)]
    pub objects: Vec<ObjectConfig>,
}

impl ProcessConfig {
    pub fn threads(&self) -> impl Iterator<Item = &ThreadObjectConfig> {
        self.objects.iter().filter_map(|object| match object {
            ObjectConfig::Thread(thread) => Some(thread),
            _ => None,
        })
    }

    pub fn threads_mut(&mut self) -> impl Iterator<Item = &mut ThreadObjectConfig> {
        self.objects.iter_mut().filter_map(|object| match object {
            &mut ObjectConfig::Thread(ref mut thread) => Some(thread),
            _ => None,
        })
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum MemoryMappingType {
    Device,
    ReadOnlyExecutable,
    ReadWriteData,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct MemoryMapping {
    pub name: String,
    #[serde(rename = "type")]
    pub ty: MemoryMappingType,
    pub start_address: u64,
    pub size_bytes: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type")]
#[serde(rename_all = "snake_case")]
pub enum ObjectConfig {
    ChannelInitiator(ChannelInitiatorConfig),
    ChannelHandler(ChannelHandlerConfig),
    Interrupt(InterruptConfig),
    Process(ProcessObjectConfig),
    Thread(ThreadObjectConfig),
    WaitGroup(WaitGroupConfig),
}

impl ObjectConfig {
    #[must_use]
    pub fn name(&self) -> &str {
        match self {
            ObjectConfig::ChannelInitiator(c) => &c.name,
            ObjectConfig::ChannelHandler(c) => &c.name,
            ObjectConfig::Interrupt(c) => &c.name,
            ObjectConfig::Process(c) => &c.name,
            ObjectConfig::Thread(c) => &c.name,
            ObjectConfig::WaitGroup(c) => &c.name,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ProcessObjectConfig {
    pub name: String,
    pub linked_process: String,
    #[serde(skip_deserializing)]
    #[serde(default)]
    pub process_object_name: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ThreadObjectConfig {
    pub name: String,
    pub kernel_stack_size_bytes: Option<u64>,
    pub priority: Option<String>,
    #[serde(skip_deserializing)]
    pub stack_size_expression: String,
    /// Boolean flag to tag this thread as the main thread for the
    /// parent process. If only one thread is defined this can be
    /// omitted.
    #[serde(default)]
    pub main_thread: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ChannelInitiatorConfig {
    pub name: String,
    pub handler_process: String,
    pub handler_object_name: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ChannelHandlerConfig {
    pub name: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct IrqConfig {
    pub name: String,
    pub number: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct InterruptConfig {
    pub name: String,
    pub irqs: Vec<IrqConfig>,
    #[serde(skip_deserializing)]
    pub handlers: HashMap<u32, String>,
    #[serde(skip_deserializing)]
    pub object_ref_name: String,
    #[serde(skip_deserializing)]
    pub interrupt_signal_map: HashMap<String, String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct WaitGroupConfig {
    pub name: String,
}

impl<A: ArchConfigInterface> SystemConfig<A> {
    fn handler_exists(&self, process_name: &str, object_name: &str) -> bool {
        for app in &self.base.apps {
            for process in &app.processes {
                if process.name == process_name
                    && let Some(object) = process.objects.iter().find(|o| o.name() == object_name)
                    && matches!(object, ObjectConfig::ChannelHandler(_))
                {
                    return true;
                }
            }
        }
        false
    }

    fn check_unique_names<'a, I>(items: I, context: &str) -> Result<()>
    where
        I: Iterator<Item = &'a str>,
    {
        let mut names = HashSet::new();
        for name in items {
            if !names.insert(name) {
                return Err(anyhow!("Duplicate name `{}` found in {}", name, context));
            }
        }
        Ok(())
    }

    pub fn calculate_and_validate(&mut self) -> Result<()> {
        // Before generic calculations and validations are done, let the Arch
        // specific interface do its own validation and fixups.
        self.arch.calculate_and_validate_config(&mut self.base)?;

        for app_config in &mut self.base.apps {
            for process in &mut app_config.processes {
                let alignment = self.arch.get_ram_alignment(process.ram_size_bytes);
                if process.ram_size_bytes % alignment != 0 {
                    return Err(anyhow!(
                        "RAM size ({}) for process {} must be aligned to {} bytes",
                        process.ram_size_bytes,
                        process.name,
                        alignment
                    ));
                }

                for thread in &mut process.threads_mut() {
                    if thread.priority.is_none() {
                        thread.priority = Some("DEFAULT_PRIORITY".to_string());
                    }

                    thread.stack_size_expression = match thread.kernel_stack_size_bytes {
                        None => {
                            "kernel::__private::kernel_config::KernelConfig::KERNEL_STACK_SIZE_BYTES"
                                .to_string()
                        }
                        Some(size) => format!("{:#x}", size),
                    };
                }
            }
        }

        Self::check_unique_names(self.base.apps.iter().map(|a| a.name.as_str()), "apps")?;

        for app_config in &self.base.apps {
            for process in &app_config.processes {
                Self::check_unique_names(
                    process.memory_mappings.iter().map(|m| m.name.as_str()),
                    &format!(
                        "memory mappings for process {} in app {}",
                        process.name, app_config.name
                    ),
                )?;
                Self::check_unique_names(
                    process.objects.iter().map(|o| o.name()),
                    &format!(
                        "objects for process {} in app {}",
                        process.name, app_config.name
                    ),
                )?;

                for object in &process.objects {
                    if let ObjectConfig::ChannelInitiator(initiator) = object {
                        let handler_process = &initiator.handler_process;
                        let handler_object_name = &initiator.handler_object_name;
                        // Check to make sure that channel objects are properly linked.
                        if !self.handler_exists(handler_process, handler_object_name) {
                            return Err(anyhow!(
                                "Channel initiator `{app_name}:{initiator_name}` references non-existent handler `{handler_process}`:{handler_object_name}",
                                app_name = app_config.name,
                                initiator_name = initiator.name,
                            ));
                        }
                    } else if let ObjectConfig::Interrupt(interrupt_config) = object {
                        Self::check_unique_names(
                            interrupt_config.irqs.iter().map(|i| i.name.as_str()),
                            &format!("irqs for interrupt object {}", interrupt_config.name),
                        )?;
                    }
                }
            }
        }

        Ok(())
    }
}
