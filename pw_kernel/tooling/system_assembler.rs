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

use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::BufWriter;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use anyhow::{Context, Result, anyhow, bail};
use clap::Parser;
use object::build::elf::{
    AttributeScope, AttributesSection, AttributesSubsection, AttributesSubsubsection, Builder,
    Section, SectionData, SectionId,
};
use object::build::{ByteString, Bytes, Id};
use object::{ReadRef, elf};

// Don't copy these sections, as the object writer won't allow multiple
// sections of SYMTAB and STRTAB type.  All other sections should be copied
// even if they're not loaded into memory, as symbols may reference
// these non-alloc sections.
static SKIPPED_APP_SECTIONS: LazyLock<HashSet<&[u8]>> = LazyLock::new(|| {
    [&b".symtab"[..], &b".shstrtab"[..], &b".strtab"[..]]
        .into_iter()
        .collect()
});

// These are prefixes for sections that will be collected from all the apps and the kernel into
// merged system-image sections of the same name. For example, within the combined elf, there can
// only be one tokenized section that must be called `.pw_tokenizer.entries` as this is what the
// host-side tooling expects. The annotation sections are designed to be appended, so for each
// section we encounter in an elf, we just append the bytes to the end of the first section we
// encounter of that kind. This means that the individual symbols from app elfs are erased since we
// don't update the SYMTAB with them.
static MERGED_SECTION_PREFIXES: [&[u8]; 2] = [b".pw_tokenizer.", b".pw_kernel.annotations."];

#[derive(Debug, Parser)]
struct Args {
    #[arg(long, required(true))]
    kernel: PathBuf,
    #[arg(long("app"))]
    apps: Vec<PathBuf>,
    #[arg(long, required(true))]
    output: PathBuf,
}

struct SystemImage<'data> {
    builder: Builder<'data>,
    merged_sections: HashMap<String, SectionId>,
}

impl<'data> SystemImage<'data> {
    pub fn new<R: ReadRef<'data>>(kernel_bytes: R) -> Result<Self> {
        let builder = Builder::read(kernel_bytes)
            .map_err(|e| anyhow!("Failed to parse kernel image: {e}"))?;

        let mut instance = Self {
            builder,
            merged_sections: HashMap::new(),
        };

        instance.init_merged_sections()?;
        Ok(instance)
    }

    fn write(self, writer: &mut BufWriter<File>) -> Result<()> {
        let mut buffer = object::write::StreamingBuffer::new(writer);

        self.builder
            .write(&mut buffer)
            .map_err(|e| anyhow!("Failed to write system image: {e}"))
    }

    fn add_app_image<'a, R: ReadRef<'a>>(&mut self, app_bytes: R, app_name: &String) -> Result<()> {
        let app_builder =
            Builder::read(app_bytes).map_err(|e| anyhow!("Failed to parse app image: {e}"))?;

        let mut section_map = HashMap::new();
        self.add_app_sections(&app_builder, app_name, &mut section_map)
            .map_err(|e| anyhow!("Failed adding app sections: {e}"))?;
        self.add_app_segments(&app_builder, &section_map)
            .map_err(|e| anyhow!("Failed adding app segments: {e}"))?;
        self.add_app_symbols(&app_builder, app_name, &section_map)
            .map_err(|e| anyhow!("Failed adding app symbols: {e}"))
    }

    fn add_app_sections(
        &mut self,
        app: &Builder,
        app_name: &String,
        section_map: &mut HashMap<usize, SectionId>,
    ) -> Result<()> {
        let mut sections_for_fixup = Vec::new();
        for section in &app.sections {
            let should_merge = Self::should_merge_section(section)?;
            let mut add_merge_section = false;
            if should_merge {
                add_merge_section = self.update_merged_section(section, section_map);
                // Don't add this section, if a merged section already exists
                if !add_merge_section {
                    continue;
                }
            }

            // TODO: davidroth - This isn't a problem right now as we don't support
            // debugging of merged self files.  Revisit this once we add debugging
            // support.  Possibly we could move the symbols & strings into merged
            // sections as we do with the tokenized section.
            if SKIPPED_APP_SECTIONS.contains(&section.name.as_slice()) {
                // println!("Skipping section '{}'", section.name);
                continue;
            }

            let new_section = self.builder.sections.add();
            section_map.insert(section.id().index(), new_section.id());

            if add_merge_section {
                self.merged_sections
                    .insert(section.name.to_string(), new_section.id());
                // Don't rename merged section if we're adding one.
                new_section.name = ByteString::from(section.name.to_vec());
            } else {
                let name = format!("{}.{}", section.name, app_name);
                new_section.name = name.into_bytes().into();
            }

            if section.sh_link_section.is_some() || section.sh_info_section.is_some() {
                sections_for_fixup.push(new_section.id());
            }

            Self::copy_section(section_map, section, new_section)?;

            // println!("Added app section '{:?}'", new_section);
        }

        // Now that all the sections have been added, go back and update any
        // references to SectionIds.
        for section_id in sections_for_fixup {
            let section = self.builder.sections.get_mut(section_id);
            if let Some(id) = section.sh_link_section {
                section.sh_link_section = Self::get_mapped_section_id(section_map, id)?;
            }
            if let Some(id) = section.sh_info_section {
                section.sh_info_section = Self::get_mapped_section_id(section_map, id)?;
            }
        }

        Ok(())
    }

    fn copy_section(
        section_map: &mut HashMap<usize, SectionId>,
        src: &Section,
        dst: &mut Section<'data>,
    ) -> Result<()> {
        dst.sh_type = src.sh_type;
        dst.sh_flags = src.sh_flags;
        // Copy sh_addr and sh_offset.  They will be updated if they're
        // added to a segment.
        dst.sh_addr = src.sh_addr;
        dst.sh_offset = src.sh_offset;
        dst.sh_size = src.sh_size;
        // OK to copy the original SectiondId's. They will be re-mapped
        // after all the sections have been added.
        dst.sh_link_section = src.sh_link_section;
        dst.sh_info_section = src.sh_info_section;
        dst.sh_addralign = src.sh_addralign;
        dst.sh_entsize = src.sh_entsize;
        dst.data = match &src.data {
            SectionData::Data(data) => SectionData::Data(Bytes::from(data.to_vec())),
            SectionData::UninitializedData(data) => SectionData::UninitializedData(*data),
            SectionData::Attributes(data) => {
                Self::copy_section_attributes(section_map, data).unwrap()
            }
            SectionData::SectionString => SectionData::SectionString,
            SectionData::Symbol => SectionData::Symbol,
            SectionData::SymbolSectionIndex => SectionData::SymbolSectionIndex,
            SectionData::String => SectionData::String,
            SectionData::DynamicSymbol => SectionData::DynamicSymbol,
            SectionData::DynamicString => SectionData::DynamicString,
            SectionData::Hash => SectionData::Hash,
            SectionData::GnuHash => SectionData::GnuHash,
            SectionData::GnuVersym => SectionData::GnuVersym,
            SectionData::GnuVerdef => SectionData::GnuVerdef,
            SectionData::GnuVerneed => SectionData::GnuVerneed,
            _ => unreachable!("Unsupported section data type: {:?}", src.data),
        };

        Ok(())
    }

    // Copy the section attributes which may GNU or other vendor-specific attributes. Need to
    // deep copy as the object crate does not provide a Copy and if an attribute tag points
    // to a section id, it need to be remapped to the new section id in the merged elf.
    fn copy_section_attributes(
        section_map: &mut HashMap<usize, SectionId>,
        data: &AttributesSection,
    ) -> Result<SectionData<'data>, ()> {
        let mut attributes_section = AttributesSection::new();
        for subsection in &data.subsections {
            let mut attributes_subsection =
                AttributesSubsection::new(ByteString::from(subsection.vendor.to_vec()));
            for subsubsection in &subsection.subsubsections {
                let scope = match &subsubsection.scope {
                    AttributeScope::File => AttributeScope::File,
                    AttributeScope::Section(section_tag) => {
                        let mut tag_sections = Vec::new();
                        // Remap the section ids to the new ids in the merged elf.
                        for section_id in section_tag {
                            let mapped_id = Self::get_mapped_section_id(section_map, *section_id);
                            tag_sections.push(mapped_id.unwrap().expect("Section attribute copy"));
                        }
                        AttributeScope::Section(tag_sections)
                    }
                    AttributeScope::Symbol(symbol_tag) => {
                        AttributeScope::Symbol(symbol_tag.to_vec())
                    }
                };

                let attributes_subsubsection = AttributesSubsubsection {
                    scope,
                    data: Bytes::from(subsubsection.data.to_vec()),
                };
                attributes_subsection
                    .subsubsections
                    .push(attributes_subsubsection);
            }
            attributes_section.subsections.push(attributes_subsection);
        }
        Ok(SectionData::Attributes(attributes_section))
    }

    fn add_app_segments(
        &mut self,
        app: &Builder,
        section_map: &HashMap<usize, SectionId>,
    ) -> Result<()> {
        for segment in &app.segments {
            if !segment.is_load() {
                // println!("Skipping non-load segment {:?}", segment.id());
                continue;
            }
            let new_segment = self
                .builder
                .segments
                .add_load_segment(segment.p_flags, segment.p_align);
            // Make sure to preserve the addresses where the original
            // segment is loaded, as add_load_segment() will change
            // them.
            // TODO: davidroth - investigate submitting an upstream
            // path to make this more robust and not require fixup.
            new_segment.p_paddr = segment.p_paddr;
            new_segment.p_vaddr = segment.p_vaddr;
            for section_id in &segment.sections {
                // append_section() recalculates sh_addr from the segment's p_vaddr, which
                // can differ from the section's actual load address when PMSAv7 alignment
                // padding creates a gap between segment start and section start. Save and
                // restore sh_addr to preserve the linker's intended memory layout.
                let original_section = app.sections.get(*section_id);
                let original_addr = original_section.sh_addr;

                let mapped_section_id = Self::get_mapped_section_id(section_map, *section_id)?;
                let section = self.builder.sections.get_mut(mapped_section_id.unwrap());
                new_segment.append_section(section);

                // Restore sh_addr to the value saved before append_section().
                section.sh_addr = original_addr;
            }
            // println!("Added segment {:?}", new_segment.id());
        }

        Ok(())
    }

    fn add_app_symbols(
        &mut self,
        app: &Builder,
        app_name: &String,
        section_map: &HashMap<usize, SectionId>,
    ) -> Result<()> {
        for symbol in &app.symbols {
            // println!("Adding app symbol: {:?}", symbol);
            let new_symbol = self.builder.symbols.add();
            if symbol.st_bind() == elf::STB_GLOBAL {
                let new_name = format!("{}_{}", symbol.name, app_name);
                new_symbol.name = new_name.into_bytes().into();
            }

            // Recalculate symbol addresses to stay consistent with the section addresses
            // preserved in add_app_segments(). Section-associated symbols are adjusted via
            // their section-relative offset. Absolute/linker-defined symbols (e.g. __sdata)
            // are adjusted if their value falls within a relocated section's address range.

            if let Some(old_section_id) = symbol.section {
                let new_section_id = Self::get_mapped_section_id(section_map, old_section_id)?;
                new_symbol.section = new_section_id;

                // If symbol is in a section, adjust its address for the section's new location.
                // The symbol's value is an absolute address, not a section-relative offset.
                // We need to convert: old_addr -> offset_in_section -> new_addr
                if let Some(new_id) = new_section_id {
                    let old_section = app.sections.get(old_section_id);
                    let new_section = self.builder.sections.get(new_id);

                    // Calculate offset within the section.
                    // Example: symbol at 0x40420, section at 0x40420 -> offset = 0
                    let offset_in_section = symbol.st_value.wrapping_sub(old_section.sh_addr);

                    // Set symbol address to new section base + offset.
                    // With our section address preservation fix, old and new section addresses
                    // should match, so this effectively preserves the original symbol address.
                    new_symbol.st_value = new_section.sh_addr.wrapping_add(offset_in_section);
                } else {
                    // No section mapping (section was filtered out), preserve original value.
                    new_symbol.st_value = symbol.st_value;
                }
            } else {
                // Symbol not explicitly associated with a section (e.g., absolute symbols
                // or linker-defined symbols like __sdata, __edata, pw_boot_stack_*).
                //
                // If the symbol's value falls within a relocated section's address range,
                // adjust it using the same section-relative offset logic.
                // Otherwise, leave it unchanged.
                let mut new_value = symbol.st_value;

                for old_section in &app.sections {
                    // Only consider allocatable sections that are actually mapped into
                    // the loadable image. Non-allocatable sections (debug info, etc.)
                    // don't contribute to the memory layout.
                    if !old_section.is_alloc() {
                        continue;
                    }

                    let size = old_section.sh_size;
                    if size == 0 {
                        continue;
                    }

                    let start = old_section.sh_addr;
                    let end = start.wrapping_add(size);
                    let addr = symbol.st_value;

                    // Check if symbol's value falls within this section's range [start, end)
                    if addr < start || addr >= end {
                        continue;
                    }

                    // This absolute symbol's value falls within this section.
                    // Treat it as if it were section-relative and apply the same
                    // relocation that we apply to section-based symbols.
                    let old_section_id = old_section.id();
                    let new_section_id = Self::get_mapped_section_id(section_map, old_section_id)?;

                    if let Some(new_id) = new_section_id {
                        let new_section = self.builder.sections.get(new_id);
                        let offset_in_section = addr.wrapping_sub(start);
                        new_value = new_section.sh_addr.wrapping_add(offset_in_section);
                    }

                    // An absolute symbol should belong to at most one allocatable section
                    // range, so we can stop once we've found and adjusted it.
                    break;
                }

                new_symbol.st_value = new_value;
            }

            new_symbol.st_info = symbol.st_info;
            new_symbol.st_other = symbol.st_other;
            new_symbol.st_shndx = symbol.st_shndx;
            new_symbol.st_size = symbol.st_size;
            new_symbol.version = symbol.version;
            new_symbol.version_hidden = symbol.version_hidden;
        }
        Ok(())
    }

    fn get_mapped_section_id(
        section_map: &HashMap<usize, SectionId>,
        id: SectionId,
    ) -> Result<Option<SectionId>> {
        Self::get_mapped_section_id_from_index(section_map, id.index())
    }

    fn get_mapped_section_id_from_index(
        section_map: &HashMap<usize, SectionId>,
        id: usize,
    ) -> Result<Option<SectionId>> {
        match section_map.get(&id) {
            Some(mapped_id) => {
                // println!("Mapped SectionId {:?} to {:?}", id, mapped_id);
                Ok(Some(*mapped_id))
            }
            None => bail!("No mapping for {:?}", id),
        }
    }

    fn init_merged_sections(&mut self) -> Result<()> {
        for section in &mut self.builder.sections {
            if Self::should_merge_section(section)? {
                self.merged_sections
                    .insert(section.name.to_string(), section.id());
            }
        }
        Ok(())
    }

    fn update_merged_section(
        &mut self,
        section: &Section,
        section_map: &mut HashMap<usize, SectionId>,
    ) -> bool {
        if let Some(&target_section_id) = self.merged_sections.get(&section.name.to_string()) {
            // There is already a section with this name, so append
            // this section data to the existing one.
            let target_section = self.builder.sections.get_mut(target_section_id);
            target_section.sh_size += section.sh_size;
            target_section.data = match &target_section.data {
                SectionData::Data(data) => {
                    let mut combined_data = data.to_vec();
                    match &section.data {
                        SectionData::Data(new_data) => combined_data.extend(&new_data.to_vec()),
                        _ => unreachable!("Incorrect data type"),
                    };
                    SectionData::Data(Bytes::from(combined_data))
                }
                _ => unreachable!("Incorrect data type"),
            };
            section_map.insert(section.id().index(), target_section_id);
            false
        } else {
            // No existing section with this name in the system image, so use
            // this one.
            true
        }
    }

    fn should_merge_section(section: &Section) -> Result<bool> {
        let is_match = MERGED_SECTION_PREFIXES
            .iter()
            .any(|name| section.name.starts_with(name));

        if is_match && section.is_alloc() {
            bail!(
                "Section '{}' matches merge section prefixes but is allocatable (is_alloc=true), check your linker script.",
                String::from_utf8_lossy(&section.name)
            );
        }

        Ok(is_match)
    }
}

fn get_app_name(path: &Path, index: usize) -> Result<String> {
    let filename = path
        .file_stem()
        .context("Invalid path: No filename found")?
        .to_str()
        .context("Invalid path: Filename is not valid UTF-8")
        .map(|s| s.to_owned())?;

    // Ensure the app name is a valid elf symbol.
    // Replace any invalid characters with `_`.
    // There is no concern over name collisions, as
    // we also add a unique index suffix
    let mut valid_name = String::new();
    let chars = filename.chars();
    for char in chars {
        if char.is_alphanumeric() || char == '_' {
            valid_name.push(char);
        } else {
            valid_name.push('_');
        }
    }
    valid_name.push_str(format!("_{index}").as_str());

    Ok(valid_name)
}

fn assemble(args: Args) -> Result<()> {
    // println!("Adding kernel image: {}", args.kernel.display());
    let kernel_bytes =
        fs::read(&args.kernel).map_err(|e| anyhow!("Failed to read kernel image: {e}"))?;

    let mut system_image: SystemImage<'_> = SystemImage::new(&*kernel_bytes)?;

    for (index, app) in args.apps.iter().enumerate() {
        // println!("Adding app image: {}", app.display());
        let app_bytes = fs::read(app).map_err(|e| anyhow!("Failed to read app image: {e}"))?;

        let app_name = get_app_name(app, index)?;
        system_image.add_app_image(&*app_bytes, &app_name)?;
    }

    // println!("Writing system image: {}", args.output.display());
    let mut open_options = fs::OpenOptions::new();
    open_options.write(true).create(true).truncate(true);
    let system_file = open_options
        .open(args.output)
        .map_err(|e| anyhow!("Failed to create system image: {e}"))?;
    let mut writer = BufWriter::new(system_file);
    system_image.write(&mut writer)
}

fn main() -> Result<()> {
    let args = Args::parse();
    assemble(args).map_err(|e| anyhow!("{e}"))
}
