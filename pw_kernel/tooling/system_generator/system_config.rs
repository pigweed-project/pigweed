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

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SystemConfig<A: ArchConfigInterface> {
    pub arch: A,
    #[serde(flatten)]
    pub base: BaseConfig,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BaseConfig {
    pub kernel: KernelConfig,
    #[serde(default)]
    pub apps: Vec<AppConfig>,
    #[serde(skip_deserializing)]
    pub arch_crate_name: &'static str,
    #[serde(skip_deserializing)]
    pub userspace: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Armv8MConfig {
    #[serde(flatten)]
    pub nvic: Armv8MNvicConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Armv8MNvicConfig {
    pub vector_table_start_address: u64,
    pub vector_table_size_bytes: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RiscVConfig;

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct KernelConfig {
    pub flash_start_address: u64,
    pub flash_size_bytes: u64,
    pub ram_start_address: u64,
    pub ram_size_bytes: u64,
    pub interrupt_table: Option<InterruptTableConfig>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct InterruptTableConfig {
    pub table: HashMap<String, String>,
    #[serde(skip_deserializing)]
    pub table_size: usize,
    #[serde(skip_deserializing)]
    pub ordered_table: BTreeMap<u32, String>,
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
    pub ram_size_bytes: u64,
    pub process: ProcessConfig,
    #[serde(default)]
    constants: Vec<ConstConfig>,
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

    #[serde(default)]
    pub memory_mappings: Vec<MemoryMapping>,

    #[serde(default)]
    pub objects: Vec<ObjectConfig>,
    pub threads: Vec<ThreadConfig>,
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
    WaitGroup(WaitGroupConfig),
}

impl ObjectConfig {
    #[must_use]
    pub fn name(&self) -> &str {
        match self {
            ObjectConfig::ChannelInitiator(c) => &c.name,
            ObjectConfig::ChannelHandler(c) => &c.name,
            ObjectConfig::Interrupt(c) => &c.name,
            ObjectConfig::WaitGroup(c) => &c.name,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ChannelInitiatorConfig {
    pub name: String,
    pub handler_app: String,
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

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ThreadConfig {
    pub name: String,
    pub stack_size_bytes: u64,
    pub priority: Option<String>,
}

impl<A: ArchConfigInterface> SystemConfig<A> {
    fn handler_exists(&self, app_name: &str, object_name: &str) -> bool {
        let Some(app) = self.base.apps.iter().find(|a| a.name == app_name) else {
            return false;
        };

        let Some(object) = app.process.objects.iter().find(|o| o.name() == object_name) else {
            return false;
        };

        matches!(object, ObjectConfig::ChannelHandler(_))
    }

    fn check_unique_names<'a, I>(items: I, context: &str) -> Result<()>
    where
        I: Iterator<Item = &'a str>,
    {
        let mut names = HashSet::new();
        for name in items {
            if !names.insert(name) {
                return Err(anyhow!("Duplicate name \"{}\" found in {}", name, context));
            }
        }
        Ok(())
    }

    pub fn calculate_and_validate(&mut self) -> Result<()> {
        // Before generic calculations and validations are done, let the Arch
        // specific interface do its own validation and fixups.
        self.arch.calculate_and_validate_config(&mut self.base)?;

        Self::check_unique_names(self.base.apps.iter().map(|a| a.name.as_str()), "apps")?;

        for app_config in &self.base.apps {
            Self::check_unique_names(
                app_config
                    .process
                    .memory_mappings
                    .iter()
                    .map(|m| m.name.as_str()),
                &format!("memory mappings for app {}", app_config.name),
            )?;
            Self::check_unique_names(
                app_config.process.objects.iter().map(|o| o.name()),
                &format!("objects for app {}", app_config.name),
            )?;
            Self::check_unique_names(
                app_config.process.threads.iter().map(|t| t.name.as_str()),
                &format!("threads for app {}", app_config.name),
            )?;

            for object in &app_config.process.objects {
                if let ObjectConfig::ChannelInitiator(initiator) = object {
                    let handler_app = &initiator.handler_app;
                    let handler_object_name = &initiator.handler_object_name;
                    // Check to make sure that channel objects are properly linked.
                    if !self.handler_exists(handler_app, handler_object_name) {
                        return Err(anyhow!(
                            "Channel initiator \"{app_name}:{initiator_name}\" references non-existent handler \"{handler_app}\":{handler_object_name}",
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
        Ok(())
    }
}
