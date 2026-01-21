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
use std::path::Path;

use anyhow::{Context, Result, anyhow};
use object::{Endian, File, Object, ObjectSection};

pub struct StackInfo {
    pub name: String,
    pub stack_addr: u64,
    pub stack_size: u64,
}

pub struct ThreadInfo {
    pub name: String,
    pub id: u64,
    pub parent_id: u64,
}

pub struct ProcessInfo {
    pub name: String,
    pub id: u64,
}

pub struct ImageInfo {
    pub stacks: Vec<StackInfo>,
    pub threads: Vec<ThreadInfo>,
    pub processes: Vec<ProcessInfo>,
    pub endian: object::Endianness,
}

impl ImageInfo {
    pub fn new(path: &Path) -> Result<Self> {
        let bin_data = fs::read(path).context("Failed to read ELF file")?;
        let obj_file = object::File::parse(&*bin_data).context("Failed to parse ELF file")?;

        let endian = obj_file.endianness();
        let stacks = Self::extract_stacks(&obj_file)?;
        let threads = Self::extract_threads(&obj_file)?;
        let processes = Self::extract_processes(&obj_file)?;

        Ok(ImageInfo {
            stacks,
            threads,
            endian,
            processes,
        })
    }

    fn get_section_data<'a>(obj_file: &File<'a>, section_name: &str) -> Result<&'a [u8]> {
        let section = obj_file
            .section_by_name(section_name)
            .context("Failed to find .pw_kernel.annotations.stack section")?;

        let data = section.data().context("Failed to read section data")?;
        Ok(data)
    }

    fn extract_stacks(obj_file: &File<'_>) -> Result<Vec<StackInfo>> {
        Self::extract_entries(obj_file, ".pw_kernel.annotations.stack", 4, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read stack name")?;
            Ok(StackInfo {
                name,
                stack_addr: fields[2],
                stack_size: fields[3],
            })
        })
    }

    fn extract_threads(obj_file: &File<'_>) -> Result<Vec<ThreadInfo>> {
        Self::extract_entries(obj_file, ".pw_kernel.annotations.thread", 4, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read thread name")?;
            Ok(ThreadInfo {
                name,
                id: fields[2],
                parent_id: fields[3],
            })
        })
    }

    fn extract_processes(obj_file: &File<'_>) -> Result<Vec<ProcessInfo>> {
        Self::extract_entries(obj_file, ".pw_kernel.annotations.process", 3, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read process name")?;
            Ok(ProcessInfo {
                name,
                id: fields[2],
            })
        })
    }

    fn extract_entries<T, F>(
        obj_file: &File<'_>,
        section_name: &str,
        num_fields: usize,
        mapper: F,
    ) -> Result<Vec<T>>
    where
        F: Fn(&[u64]) -> Result<T>,
    {
        let data = Self::get_section_data(obj_file, section_name)?;
        let is_64 = obj_file.is_64();
        let field_size = if is_64 { 8 } else { 4 };
        let entry_size = num_fields * field_size;

        if data.len() % entry_size != 0 {
            return Err(anyhow!(
                "{} section size is not a multiple of {}",
                section_name,
                entry_size
            ));
        }

        let endian = obj_file.endianness();
        let mut entries = Vec::new();

        for entry_data in data.chunks(entry_size) {
            let mut fields = Vec::with_capacity(num_fields);
            for i in 0..num_fields {
                fields.push(Self::extract_usize_field(endian, entry_data, i, is_64)?);
            }
            entries.push(mapper(&fields)?);
        }

        Ok(entries)
    }

    fn extract_usize_field(
        endian: object::Endianness,
        chunk: &[u8],
        index: usize,
        is_64: bool,
    ) -> Result<u64> {
        if is_64 {
            let start = index * 8;
            let bytes = chunk
                .get(start..start + 8)
                .context("Chunk too small for 64-bit field")?;
            Ok(endian.read_u64_bytes(bytes.try_into()?))
        } else {
            let start = index * 4;
            let bytes = chunk
                .get(start..start + 4)
                .context("Chunk too small for 32-bit field")?;
            Ok(u64::from(endian.read_u32_bytes(bytes.try_into()?)))
        }
    }

    fn read_data<'a>(obj_file: &'a object::File, addr: u64, len: u64) -> Result<&'a [u8]> {
        // Find the section containing the address
        for section in obj_file.sections() {
            let section_addr = section.address();
            let section_size = section.size();
            if addr >= section_addr && addr + len <= section_addr + section_size {
                let data = section.data()?;
                let offset = usize::try_from(addr - section_addr).context("Offset too large")?;
                return Ok(&data[offset..offset + usize::try_from(len).context("Len too large")?]);
            }
        }

        Err(anyhow!("Could not find address {:#x} in any section", addr))
    }

    fn read_string(obj_file: &object::File, addr: u64, len: u64) -> Result<String> {
        let bytes = Self::read_data(obj_file, addr, len)?;
        Ok(String::from_utf8_lossy(bytes).into_owned())
    }
}
