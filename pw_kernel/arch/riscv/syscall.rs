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

use kernel::SyscallArgs;
use kernel::syscall::handle_syscall;
use pw_cast::CastInto as _;
use pw_status::{Error, Result};

use crate::exceptions::TrapFrame;

pub struct RiscVSyscallArgs<'a> {
    frame: &'a TrapFrame,
    cur_index: usize,
}

impl<'a> RiscVSyscallArgs<'a> {
    fn new(frame: &'a TrapFrame) -> Self {
        Self {
            frame,
            cur_index: 0,
        }
    }
}

impl<'a> SyscallArgs<'a> for RiscVSyscallArgs<'a> {
    fn next_usize(&mut self) -> Result<usize> {
        let value = self.frame.a(self.cur_index)?;
        self.cur_index += 1;
        Ok(value)
    }

    #[inline(always)]
    fn next_u64(&mut self) -> Result<u64> {
        // Note: This follows the PSABI calling convention[1] which differs from
        // the outdated (but still returned in search results) ABI from the ISA[2].
        // The PSABI is what both gcc and llvm based toolchains implement.
        //
        // 1: https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/main/riscv-cc.adoc#integer-calling-convention
        // 2: https://riscv.org/wp-content/uploads/2024/12/riscv-calling.pdf
        let low: u64 = self.next_usize()?.cast_into();
        let high: u64 = self.next_usize()?.cast_into();

        Ok(low | high << 32)
    }
}

// Pulls arguments out of the trap frame and calls the arch-independent syscall
// handler.
pub fn handle_ecall(frame: &mut TrapFrame) {
    // Explicitly truncate the syscall ID to 16 bits per syscall ABI.
    #[expect(clippy::cast_possible_truncation)]
    let id = frame.t0 as u16;
    let args = RiscVSyscallArgs::new(frame);
    let ret_val = handle_syscall(super::Arch, id, args);
    frame.a0 = ret_val.value[0];
    frame.a1 = ret_val.value[1];

    // ECALL exceptions do not "retire the instruction" requiring the advancing
    // of the PC past the ECALL instruction.  ECALLs are encoded as 4 byte
    // instructions.
    //
    // Use a wrapping add, as section 1.4 of the RISC-V unprivileged spec states:
    // "...memory address computations done by the hardware ignore overflow
    // and instead wrap around modulo 2^XLEN"
    frame.epc = frame.epc.wrapping_add(4);
}

impl TrapFrame {
    pub fn a(&self, index: usize) -> Result<usize> {
        if index > 7 {
            return Err(Error::InvalidArgument);
        }
        // Pointer math is used here instead of a match statement as it results
        // in significantly smaller code.
        //
        // SAFETY: Index is range checked above and the a* fields are in consecutive
        // positions in the `TrapFrame` struct.
        let usize0 = &raw const self.a0;
        Ok(unsafe { *usize0.byte_add(index * 4) })
    }
}
