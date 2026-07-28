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

use crate::{
    PROCESS_SECTION_NAME, STACK_SECTION_NAME, THREAD_SECTION_NAME, TRACE_BUFFER_SECTION_NAME,
};

#[derive(Debug, Clone)]
pub struct StackInfo {
    pub name: String,
    pub stack_addr: u64,
    pub stack_size: u64,
}

#[derive(Debug, Clone)]
pub struct ThreadInfo {
    pub name: String,
    pub id: u64,
    pub parent_id: u64,
}

#[derive(Debug, Clone)]
pub struct ProcessInfo {
    pub name: String,
    pub id: u64,
}

#[derive(Debug, Clone)]
pub struct TraceBufferInfo {
    pub name: String,
    pub addr: u64,
    pub size: u64,
}

#[derive(Debug, Clone)]
pub struct ImageInfo {
    pub stacks: Vec<StackInfo>,
    pub threads: Vec<ThreadInfo>,
    pub processes: Vec<ProcessInfo>,
    pub trace_buffers: Vec<TraceBufferInfo>,
    pub endian: object::Endianness,
}

impl ImageInfo {
    /// Create `ImageInfo` by loading and parsing an ELF binary file at `path`.
    pub fn new(path: &Path) -> Result<Self> {
        Self::from_path(path)
    }

    /// Create `ImageInfo` by loading and parsing an ELF binary file at `path`.
    pub fn from_path(path: impl AsRef<Path>) -> Result<Self> {
        let bin_data = fs::read(path)?;
        Self::from_bytes(&bin_data)
    }

    /// Create `ImageInfo` by parsing ELF binary data bytes.
    pub fn from_bytes(bin_data: &[u8]) -> Result<Self> {
        let obj_file = object::File::parse(bin_data)?;
        Self::from_object(&obj_file)
    }

    /// Create `ImageInfo` from an already parsed [`object::File`].
    pub fn from_object(obj_file: &File<'_>) -> Result<Self> {
        let endian = obj_file.endianness();
        let stacks = Self::extract_stacks(obj_file)?
            .with_context(|| format!("Failed to find section {STACK_SECTION_NAME}"))?;
        let threads = Self::extract_threads(obj_file)?
            .with_context(|| format!("Failed to find section {THREAD_SECTION_NAME}"))?;
        let processes = Self::extract_processes(obj_file)?
            .with_context(|| format!("Failed to find section {PROCESS_SECTION_NAME}"))?;
        let trace_buffers = Self::extract_trace_buffers(obj_file)?.unwrap_or_default();

        Ok(ImageInfo {
            stacks,
            threads,
            endian,
            processes,
            trace_buffers,
        })
    }

    fn get_section_data<'a>(obj_file: &File<'a>, section_name: &str) -> Result<Option<&'a [u8]>> {
        let Some(section) = obj_file.section_by_name(section_name) else {
            return Ok(None);
        };

        let data = section.data().context("Failed to read section data")?;
        Ok(Some(data))
    }

    fn extract_stacks(obj_file: &File<'_>) -> Result<Option<Vec<StackInfo>>> {
        Self::extract_entries(obj_file, STACK_SECTION_NAME, 4, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read stack name")?;
            Ok(StackInfo {
                name,
                stack_addr: fields[2],
                stack_size: fields[3],
            })
        })
    }

    fn extract_threads(obj_file: &File<'_>) -> Result<Option<Vec<ThreadInfo>>> {
        Self::extract_entries(obj_file, THREAD_SECTION_NAME, 4, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read thread name")?;
            Ok(ThreadInfo {
                name,
                id: fields[2],
                parent_id: fields[3],
            })
        })
    }

    fn extract_processes(obj_file: &File<'_>) -> Result<Option<Vec<ProcessInfo>>> {
        Self::extract_entries(obj_file, PROCESS_SECTION_NAME, 3, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read process name")?;
            Ok(ProcessInfo {
                name,
                id: fields[2],
            })
        })
    }

    fn extract_trace_buffers(obj_file: &File<'_>) -> Result<Option<Vec<TraceBufferInfo>>> {
        Self::extract_entries(obj_file, TRACE_BUFFER_SECTION_NAME, 4, |fields| {
            let name = Self::read_string(obj_file, fields[0], fields[1])
                .context("Failed to read trace buffer name")?;
            Ok(TraceBufferInfo {
                name,
                addr: fields[2],
                size: fields[3],
            })
        })
    }

    /// Extracts entries from a named linker section in an ELF object file.
    ///
    /// Returns `Ok(Some(entries))` if the section is present in the object file,
    /// `Ok(None)` if the section is not found, or `Err` if reading or parsing
    /// the section data fails.
    fn extract_entries<T, F>(
        obj_file: &File<'_>,
        section_name: &str,
        num_fields: usize,
        mapper: F,
    ) -> Result<Option<Vec<T>>>
    where
        F: Fn(&[u64]) -> Result<T>,
    {
        let Some(data) = Self::get_section_data(obj_file, section_name)? else {
            return Ok(None);
        };
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

        Ok(Some(entries))
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::STACK_SECTION_NAME;

    #[test]
    fn invalid_elf_bytes_returns_error() {
        let invalid_bytes = b"not an elf file";
        let res = ImageInfo::from_bytes(invalid_bytes);
        res.unwrap_err();
    }

    #[test]
    fn non_existent_file_returns_error() {
        let res = ImageInfo::from_path(Path::new("non_existent_file_path.elf"));
        res.unwrap_err();
    }

    #[test]
    fn missing_section_yields_none() {
        let obj = object::write::Object::new(
            object::BinaryFormat::Elf,
            object::Architecture::X86_64,
            object::Endianness::Little,
        );
        let data = obj.write().unwrap();
        let obj_file = object::File::parse(&*data).unwrap();
        let res: Result<Option<Vec<StackInfo>>> =
            ImageInfo::extract_entries(&obj_file, ".non_existent_section", 4, |_| {
                unreachable!();
            });
        assert!(res.unwrap().is_none());
    }

    #[test]
    fn missing_required_sections_returns_error() {
        let obj = object::write::Object::new(
            object::BinaryFormat::Elf,
            object::Architecture::X86_64,
            object::Endianness::Little,
        );
        let data = obj.write().unwrap();
        let res = ImageInfo::from_bytes(&data);
        res.unwrap_err();
    }

    #[test]
    fn missing_optional_sections_returns_empty_vec() {
        let mut obj = object::write::Object::new(
            object::BinaryFormat::Elf,
            object::Architecture::X86_64,
            object::Endianness::Little,
        );
        let _ = obj.add_section(
            Vec::new(),
            STACK_SECTION_NAME.as_bytes().to_vec(),
            object::SectionKind::ReadOnlyData,
        );
        let _ = obj.add_section(
            Vec::new(),
            THREAD_SECTION_NAME.as_bytes().to_vec(),
            object::SectionKind::ReadOnlyData,
        );
        let _ = obj.add_section(
            Vec::new(),
            PROCESS_SECTION_NAME.as_bytes().to_vec(),
            object::SectionKind::ReadOnlyData,
        );

        let data = obj.write().unwrap();
        let image_info = ImageInfo::from_bytes(&data).unwrap();
        assert!(image_info.trace_buffers.is_empty());
        assert!(image_info.stacks.is_empty());
        assert!(image_info.threads.is_empty());
        assert!(image_info.processes.is_empty());
    }
}
