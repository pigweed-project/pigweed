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

use std::collections::HashSet;

use anyhow::{Result, anyhow};

use crate::ArchConfigInterface;
use crate::system_config::{ConstConfig, SystemConfig};

pub trait ManifestValidator<A: ArchConfigInterface> {
    fn validate(&self, config: &SystemConfig<A>) -> Result<()>;
}

fn check_ident(name: &str, context: &str) -> Result<()> {
    if syn::parse_str::<syn::Ident>(name).is_err() {
        return Err(anyhow!(
            "Invalid Rust identifier `{}` found in {}",
            name,
            context
        ));
    }
    if name.to_lowercase() != name {
        return Err(anyhow!(
            "Invalid identifier `{}` in {}: must be all lowercase.",
            name,
            context
        ));
    }
    Ok(())
}

struct IdentifierValidatorContext<'a> {
    seen: HashSet<&'a str>,
}

impl<'a> IdentifierValidatorContext<'a> {
    fn new() -> Self {
        Self {
            seen: HashSet::new(),
        }
    }

    fn check(&mut self, name: &'a str, context: &str) -> Result<()> {
        check_ident(name, context)?;
        if !self.seen.insert(name) {
            return Err(anyhow!("Duplicate {}", context));
        }
        Ok(())
    }
}

pub struct IdentifierValidator;

impl<A: ArchConfigInterface> ManifestValidator<A> for IdentifierValidator {
    fn validate(&self, config: &SystemConfig<A>) -> Result<()> {
        let mut app_names = IdentifierValidatorContext::new();
        let mut process_names = IdentifierValidatorContext::new();
        for app in &config.base.apps {
            app_names.check(app.name.as_str(), &format!("app `{}`", app.name))?;

            let mut constant_names = IdentifierValidatorContext::new();
            for constant in &app.constants {
                let const_name = match constant {
                    ConstConfig::Bool { name, .. } => name,
                    ConstConfig::Char { name, .. } => name,
                    ConstConfig::U8 { name, .. } => name,
                    ConstConfig::U16 { name, .. } => name,
                    ConstConfig::U32 { name, .. } => name,
                    ConstConfig::U64 { name, .. } => name,
                    ConstConfig::U128 { name, .. } => name,
                    ConstConfig::Usize { name, .. } => name,
                    ConstConfig::I8 { name, .. } => name,
                    ConstConfig::I16 { name, .. } => name,
                    ConstConfig::I32 { name, .. } => name,
                    ConstConfig::I64 { name, .. } => name,
                    ConstConfig::I128 { name, .. } => name,
                    ConstConfig::Isize { name, .. } => name,
                    ConstConfig::String { name, .. } => name,
                };
                let context = format!("constant `{}` in app `{}`", const_name, app.name);
                constant_names.check(const_name.as_str(), &context)?;
            }

            for process in &app.processes {
                let context = format!("process `{}` in app `{}`", process.name, app.name);
                process_names.check(process.name.as_str(), &context)?;

                let mut memory_mapping_names = IdentifierValidatorContext::new();
                for mapping in &process.memory_mappings {
                    let context = format!(
                        "memory mapping `{}` in process `{}`",
                        mapping.name, process.name
                    );
                    memory_mapping_names.check(mapping.name.as_str(), &context)?;
                }

                let mut object_names = IdentifierValidatorContext::new();
                for object in &process.objects {
                    let context =
                        format!("object `{}` in process `{}`", object.name(), process.name);
                    object_names.check(object.name(), &context)?;
                }

                let thread_count = &process.threads().count();
                if *thread_count < 1 {
                    return Err(anyhow!(
                        "Process `{}` has no threads. At least one thread must be declared.",
                        process.name,
                    ));
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::system_config::{AppConfig, BaseConfig, KernelConfig, ProcessConfig};

    struct MockArch;
    impl crate::ArchConfigInterface for MockArch {
        fn get_arch_crate_name(&self) -> &'static str {
            "mock"
        }
        fn get_start_fn_address(&self, _flash: u64) -> u64 {
            0
        }
        fn calculate_and_validate_config(&mut self, _config: &mut BaseConfig) -> Result<()> {
            Ok(())
        }
        fn get_interrupt_table_link_section(&self) -> Option<String> {
            None
        }
        fn bare_interrupt_table_entries(&self) -> bool {
            false
        }
    }

    fn make_mock_config() -> SystemConfig<MockArch> {
        SystemConfig {
            arch: MockArch,
            base: BaseConfig {
                kernel: KernelConfig {
                    flash_start_address: 0,
                    flash_size_bytes: 0,
                    ram_start_address: 0,
                    ram_size_bytes: 0,
                    interrupt_table: None,
                    mpu_validation: crate::mpu_validation::MpuValidationMode::Permissive,
                },
                apps: vec![],
                arch_crate_name: "mock",
                userspace: true,
                bare_interrupt_table_entries: false,
            },
        }
    }

    #[test]
    fn test_identifier_validator_rust_ident() {
        let mut config = make_mock_config();
        config.base.apps.push(AppConfig {
            name: "valid_app".to_string(),
            flash_size_bytes: 0,
            ram_size_bytes: 0,
            processes: vec![ProcessConfig {
                name: "valid_process".to_string(),
                ram_size_bytes: 0,
                ram_start_address: 0,
                memory_mappings: vec![crate::system_config::MemoryMapping {
                    name: "valid_mapping".to_string(),
                    ty: crate::system_config::MemoryMappingType::ReadOnlyExecutable,
                    start_address: 0,
                    size_bytes: 0,
                }],
                objects: vec![crate::system_config::ObjectConfig::Thread(
                    crate::system_config::ThreadObjectConfig {
                        name: "valid_thread_obj".to_string(),
                        kernel_stack_size_bytes: None,
                        priority: None,
                        stack_size_expression: "".to_string(),
                    },
                )],
                main_thread_name: None,
            }],
            constants: vec![ConstConfig::U32 {
                name: "valid_const".to_string(),
                value: 0,
            }],
            flash_start_address: 0,
            ram_start_address: 0,
            start_fn_address: 0,
            initial_sp: 0,
        });

        let validator = IdentifierValidator;
        validator.validate(&config).unwrap();

        // Invalid app name (syntax)
        config.base.apps[0].name = "invalid-name".to_string();
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].name = "valid_app".to_string();

        // Invalid app name (must be lowercase)
        config.base.apps[0].name = "InvalidApp".to_string();
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].name = "valid_app".to_string();

        // Invalid mapping name
        config.base.apps[0].processes[0].memory_mappings[0].name = "InvalidMapping".to_string();
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].processes[0].memory_mappings[0].name = "valid_mapping".to_string();

        // Invalid constant name
        config.base.apps[0].constants[0] = ConstConfig::U32 {
            name: "let".to_string(),
            value: 0,
        };
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].constants[0] = ConstConfig::U32 {
            name: "valid_const".to_string(),
            value: 0,
        };
    }

    #[test]
    fn test_identifier_validator_uniqueness() {
        let mut config = make_mock_config();
        let app1 = AppConfig {
            name: "app1".to_string(),
            flash_size_bytes: 0,
            ram_size_bytes: 0,
            processes: vec![ProcessConfig {
                name: "proc1".to_string(),
                ram_size_bytes: 0,
                ram_start_address: 0,
                memory_mappings: vec![crate::system_config::MemoryMapping {
                    name: "mapping1".to_string(),
                    ty: crate::system_config::MemoryMappingType::ReadOnlyExecutable,
                    start_address: 0,
                    size_bytes: 0,
                }],
                objects: vec![crate::system_config::ObjectConfig::Thread(
                    crate::system_config::ThreadObjectConfig {
                        name: "thread1".to_string(),
                        kernel_stack_size_bytes: None,
                        priority: None,
                        stack_size_expression: "".to_string(),
                    },
                )],
                main_thread_name: None,
            }],
            constants: vec![ConstConfig::U32 {
                name: "const1".to_string(),
                value: 0,
            }],
            flash_start_address: 0,
            ram_start_address: 0,
            start_fn_address: 0,
            initial_sp: 0,
        };
        config.base.apps.push(app1.clone());

        let validator = IdentifierValidator;
        validator.validate(&config).unwrap();

        // Duplicate app name
        config.base.apps.push(app1.clone());
        assert!(validator.validate(&config).is_err());

        // Resolve duplicate app name but leave duplicate process name
        config.base.apps[1].name = "app2".to_string();
        assert!(validator.validate(&config).is_err());

        // Resolve duplicate process name
        config.base.apps[1].processes[0].name = "proc2".to_string();
        validator.validate(&config).unwrap();

        // Duplicate memory mapping name
        let m = config.base.apps[0].processes[0].memory_mappings[0].clone();
        config.base.apps[0].processes[0].memory_mappings.push(m);
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].processes[0].memory_mappings.pop();

        // Duplicate constant
        let c = config.base.apps[0].constants[0].clone();
        config.base.apps[0].constants.push(c);
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].constants.pop();

        // Duplicate object name
        let o = config.base.apps[0].processes[0].objects[0].clone();
        config.base.apps[0].processes[0].objects.push(o);
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].processes[0].objects.pop();

        // No thread objects should cause an error.
        let t = config.base.apps[0].processes[0].objects[0].clone();
        config.base.apps[0].processes[0].objects.pop();
        assert!(validator.validate(&config).is_err());
        config.base.apps[0].processes[0].objects.push(t);
    }
}
