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
#![allow(clippy::print_stdout)]

use core::ops::Bound;
use std::collections::{BTreeMap, HashMap, btree_map, hash_map};
use std::rc::Rc;

use anyhow::anyhow;
use object::elf::{STB_GLOBAL, STB_LOCAL, STB_WEAK, STT_FUNC, SymbolBind};
use object::read::elf::{ElfFile32, Sym};
use pw_cast::CastFrom as _;

use super::elf_mem::ElfMem;
use super::{DecodedInstr, Instr, InstrA, InstrType, Reg};
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum Binding {
    Unknown = 0,
    Local = 1,
    GlobalWeak = 2,
    Global = 3,
}
impl Binding {
    #[must_use]
    pub fn from_st_bind(st_bind: SymbolBind) -> Self {
        match st_bind {
            STB_GLOBAL => Self::Global,
            STB_WEAK => Self::GlobalWeak,
            STB_LOCAL => Self::Local,
            _ => Self::Unknown,
        }
    }
}
/// List all the functions in an elf-file.
pub fn list_functions<'a>(
    elf: &'a ElfFile32<'a, object::LittleEndian>,
    elf_mem: &'a ElfMem,
) -> anyhow::Result<Vec<Function<'a>>> {
    const E: object::LittleEndian = object::LittleEndian;
    let t = elf.elf_symbol_table();
    let mut result = vec![];
    for sym in t.symbols() {
        let name = core::str::from_utf8(t.symbol_name(E, sym)?)?;
        if sym.st_type() != STT_FUNC || name.is_empty() {
            continue;
        }
        let Some(contents) = elf_mem.get(sym.st_value(E), sym.st_size(E)) else {
            return Err(anyhow!(
                "Cannot find function body for symbol {name:?} at addr 0x{:x}",
                sym.st_value(E)
            ));
        };
        result.push(Function {
            symbol_name: name,
            binding: Binding::from_st_bind(sym.st_bind()),
            body: Snippet {
                addr: sym.st_value(E),
                data: contents,
                instr_sizes: Rc::new(InstrSizes::new(contents)),
            },
        });
    }
    Ok(result)
}
/// A repository of functions and branches, giving easy access to instructions
/// by address or symbol, as well as looking up jumps to a particular address.
pub struct FuncRepo<'a> {
    by_symbol: HashMap<&'a str, Rc<Function<'a>>>,
    by_addr: BTreeMap<u32, Rc<Function<'a>>>,
    // Key is the jumpee, Value is a list of jumper addresses
    jumpers: HashMap<u32, Vec<u32>>,
}
impl<'a> FuncRepo<'a> {
    pub fn new(funcs: Vec<Function<'a>>) -> anyhow::Result<Self> {
        let mut by_symbol = HashMap::<&str, Rc<Function>>::new();
        let mut by_addr = BTreeMap::<u32, Rc<Function>>::new();
        let mut jumpers = HashMap::<u32, Vec<u32>>::new();
        for func in funcs {
            let func = Rc::new(func);
            match by_symbol.entry(func.symbol_name) {
                hash_map::Entry::Vacant(e) => {
                    e.insert(func.clone());
                }
                hash_map::Entry::Occupied(mut e) => {
                    if func.binding > e.get().binding {
                        e.insert(func.clone());
                    } else if func.binding == Binding::Global {
                        return Err(anyhow!(
                            "Duplicate global functions with symbol {:?}",
                            func.symbol_name
                        ));
                    }
                }
            }
            match by_addr.entry(func.start_addr()) {
                btree_map::Entry::Vacant(e) => {
                    e.insert(func.clone());
                }
                // Only replace if the new symbol has a stronger binding
                btree_map::Entry::Occupied(mut e) if func.binding > e.get().binding => {
                    // NOTE: sometimes multiple symbols will share the same
                    // implementation (for example, generic funcs), and we don't
                    // really care which one gets attribution
                    e.insert(func.clone());
                }
                _ => {}
            }
            let mut reg_const: Option<(Reg, u32)> = None;
            for instr in func.body.instructions() {
                let instr_d = instr.decode();
                if matches!(instr_d.ty(), InstrType::J(_) | InstrType::B(_)) {
                    jumpers
                        .entry(instr.absolute_imm())
                        .or_default()
                        .push(instr.addr);
                }
                if let (DecodedInstr::Jalr(i), Some((const_rd, const_val))) = (instr_d, reg_const)
                    && i.rd() == const_rd
                {
                    jumpers
                        .entry(const_val.wrapping_add_signed(i.imm()))
                        .or_default()
                        .push(instr.addr);
                }
                reg_const = match instr_d {
                    DecodedInstr::Auipc(i) => Some((i.rd(), instr.absolute_imm())),
                    _ => None,
                };
            }
        }
        Ok(Self {
            by_symbol,
            by_addr,
            jumpers,
        })
    }
    #[must_use]
    pub fn get_jumpers(&self, dest_addr: u32) -> &[u32] {
        self.jumpers
            .get(&dest_addr)
            .map(|v| v.as_slice())
            .unwrap_or(&[])
    }
    #[must_use]
    pub fn get_func_by_symbol(&self, name: &str) -> Option<&Function<'_>> {
        self.by_symbol.get(name).map(|v| &**v)
    }
    #[must_use]
    pub fn instructions_at_addr(&self, addr: u32) -> Option<(&Function<'_>, InstrIterator<'_>)> {
        let (_, func) = self
            .by_addr
            .range((Bound::Unbounded, Bound::Included(addr)))
            .next_back()?;
        if addr >= func.end_addr() {
            return None;
        }
        let instr_iter = func.body.instructions_at_addr(addr)?;
        Some((func, instr_iter))
    }
}
/// Represents a function in an elf file.
pub struct Function<'a> {
    pub symbol_name: &'a str,
    pub binding: Binding,
    pub body: Snippet<'a>,
}
impl Function<'_> {
    #[must_use]
    pub fn start_addr(&self) -> u32 {
        self.body.addr
    }
    #[must_use]
    pub fn end_addr(&self) -> u32 {
        self.body
            .addr
            .wrapping_add(self.body.data.len().try_into().unwrap())
    }
}
pub struct Snippet<'a> {
    data: &'a [u8],
    addr: u32,
    instr_sizes: Rc<InstrSizes>,
}
impl<'a> Snippet<'a> {
    #[allow(dead_code)]
    #[must_use]
    pub fn data(&self) -> &'a [u8] {
        self.data
    }
    #[must_use]
    pub fn instructions(&'a self) -> InstrIterator<'a> {
        InstrIterator {
            data: self.data,
            start_addr: self.addr,
            offset: 0,
            instr_sizes: &self.instr_sizes,
        }
    }
    #[must_use]
    pub fn instructions_at_addr(&'a self, addr: u32) -> Option<InstrIterator<'a>> {
        if !addr.is_multiple_of(2) || addr < self.addr {
            return None;
        }
        let Ok(offset) = usize::try_from(addr - self.addr) else {
            return None;
        };
        if offset >= self.data.len() {
            return None;
        }
        Some(InstrIterator {
            data: self.data,
            start_addr: self.addr,
            offset,
            instr_sizes: &self.instr_sizes,
        })
    }
}
/// A primitive bit vector.
struct BitVec {
    data: Vec<u64>,
    val: u64,
    bit_pos: usize,
}
impl BitVec {
    pub fn new() -> Self {
        Self {
            data: vec![],
            val: 0,
            bit_pos: 0,
        }
    }
    pub fn push(&mut self, val: bool) {
        self.val |= if val { 1 << self.bit_pos } else { 0 };
        self.bit_pos += 1;
        if self.bit_pos == 64 {
            self.data.push(self.val);
            self.bit_pos = 0;
            self.val = 0;
        }
    }
    pub fn get(&self, index: usize) -> Option<bool> {
        let word_index = index / 64;
        if let Some(word) = self.data.get(word_index) {
            return Some((word & (1 << (index % 64))) != 0);
        }
        if index < self.data.len() * 64 + self.bit_pos {
            return Some((self.val & (1 << (index % 64))) != 0);
        }
        None
    }
}
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum InstrSize {
    _2,
    _4,
}
impl From<InstrSize> for usize {
    fn from(value: InstrSize) -> Self {
        match value {
            InstrSize::_2 => 2,
            InstrSize::_4 => 4,
        }
    }
}
/// A data structure for keeping track of the size of the previous instruction.
/// Useful when iterating backwards through machine code.
pub struct InstrSizes {
    // True bits indicate that the previous instruction was 4-bytes
    bits: BitVec,
}
impl InstrSizes {
    #[must_use]
    pub fn new(mut data: &[u8]) -> Self {
        let mut bits = BitVec::new();
        bits.push(true);
        while !data.is_empty() {
            if data.len() >= 4 {
                let instr = Instr(u32::from_le_bytes(data[..4].try_into().unwrap()));
                if instr.is_compressed() {
                    data = &data[2..];
                    bits.push(false);
                } else {
                    data = &data[4..];
                    bits.push(true);
                    bits.push(true);
                }
                continue;
            }
            if data.len() >= 2 {
                let instr = Instr(u16::from_le_bytes(data[..2].try_into().unwrap()).into());
                if instr.is_compressed() {
                    bits.push(false);
                }
            }
            break;
        }
        Self { bits }
    }
    #[must_use]
    pub fn prev_instr_size(&self, offset: usize) -> Option<InstrSize> {
        match self.bits.get(offset / 2) {
            Some(true) => Some(InstrSize::_4),
            Some(false) => Some(InstrSize::_2),
            None => None,
        }
    }
}
/// An iterator for RiSC-V instructions.
pub struct InstrIterator<'a> {
    // virtual address of the start of this block of code.
    start_addr: u32,
    // slice containing the instruction bytes.
    data: &'a [u8],
    // offset to the current instruction from the start of `data``.
    offset: usize,
    // instruction-size metadata for all the instructions in `data`.
    instr_sizes: &'a InstrSizes,
}
impl InstrIterator<'_> {
    /// Returns the previous instruction and move the cursor back one instruction.
    pub fn prev(&mut self) -> Option<InstrA> {
        let Some(prev_instr_size) = self.instr_sizes.prev_instr_size(self.offset) else {
            println!(
                "Can't find prev instr at addr {:x} offset={}",
                self.offset + usize::cast_from(self.start_addr),
                self.offset
            );
            return None;
        };
        let prev_offset = self.offset.checked_sub(usize::from(prev_instr_size))?;
        self.offset = prev_offset;
        let addr = self.start_addr + u32::try_from(self.offset).unwrap();
        if self.data.len() - self.offset >= 4 {
            Some(InstrA::new(
                addr,
                u32::from_le_bytes(self.data[self.offset..][..4].try_into().unwrap()).into(),
            ))
        } else if self.data.len() - self.offset >= 2 {
            Some(InstrA::new(
                addr,
                u16::from_le_bytes(self.data[self.offset..][..2].try_into().unwrap()).into(),
            ))
        } else {
            None
        }
    }
}
impl Iterator for InstrIterator<'_> {
    type Item = InstrA;
    /// Returns the next instruction and move the cursor forward one instruction.
    fn next(&mut self) -> Option<Self::Item> {
        let addr = self.start_addr + u32::try_from(self.offset).unwrap();
        if self.data[self.offset..].len() >= 4 {
            let instr = InstrA::new(
                addr,
                u32::from_le_bytes(self.data[self.offset..][..4].try_into().unwrap()).into(),
            );
            if instr.is_compressed() {
                self.offset += 2;
            } else {
                self.offset += 4;
            }
            return Some(instr);
        }
        if self.data[self.offset..].len() >= 2 {
            let instr = InstrA::new(
                addr,
                u16::from_le_bytes(self.data[self.offset..][..2].try_into().unwrap()).into(),
            );
            self.offset += 2;
            return Some(instr);
        }
        None
    }
}
