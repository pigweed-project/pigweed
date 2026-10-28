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

use core::ptr;

use kernel::Kernel;
use kernel::interrupt_controller::{InterruptController, InterruptGuard, InterruptTableEntry};
use kernel_config::{PlicConfig, PlicConfigInterface};
use log_if::debug_if;
use pw_log::info;
use regs::{rw_block_reg, rw_int_field};

use crate::exceptions::RiscVInterruptController;
use crate::regs::Interrupt;
use crate::timer;

const LOG_INTERRUPTS: bool = false;

// TODO: Once the kernel supports more than one HART, we'll need to configure the number of contexts.
static CONTEXT_0: Context = Context { index: 0 };

struct Context {
    index: u16,
}

impl regs::BaseAddress for Context {
    fn base_address(&self) -> usize {
        // Only one context is currently supported.
        pw_assert::debug_assert!(self.index == 0);
        PlicConfig::PLIC_BASE_ADDRESS
    }
}

// Marker type to ensure only the PLIC can be passed to these block registers.
pub trait PlicBaseAddress: regs::BaseAddress {}
impl PlicBaseAddress for Context {}

// As per section 3 https://github.com/riscv/riscv-plic-spec/blob/1.0.0/riscv-plic-1.0.0.pdf
// all registers are u32, not usize.

#[doc = "Interrupt Priorities Registers"]
struct Ipr {}
impl Ipr {
    const REG_OFFSET: usize = 0;

    fn offset<A: PlicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PlicBaseAddress>(&mut self, addr: &A, irq: u32) -> IprValue {
        let ptr: *const IprValue = ptr::with_exposed_provenance::<IprValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    #[allow(dead_code)]
    fn write<A: PlicBaseAddress>(&mut self, addr: &A, irq: u32, val: IprValue) {
        let ptr = ptr::with_exposed_provenance_mut::<IprValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[allow(dead_code)]
struct IprValue(u32);
impl IprValue {
    rw_int_field!(u32, priority, 0, 31, u32, "Priority");
}

#[doc = "Interrupt Enables Registers"]
struct Ier {}
impl Ier {
    const REG_OFFSET: usize = 0x2000;

    fn offset<A: PlicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (4 * (irq as usize / 32))
    }

    #[allow(dead_code)]
    fn read<A: PlicBaseAddress>(&mut self, addr: &A, irq: u32) -> IerValue {
        let ptr = ptr::with_exposed_provenance::<IerValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    #[allow(dead_code)]
    fn write<A: PlicBaseAddress>(&mut self, addr: &A, irq: u32, val: IerValue) {
        let ptr = ptr::with_exposed_provenance_mut::<IerValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

struct IerValue(u32);
impl IerValue {
    rw_int_field!(u32, sources, 0, 31, u32, "sources");
}

rw_block_reg!(
    Icr,
    IcrValue,
    u32,
    PlicBaseAddress,
    0x200004,
    "Interrupt Claim Registers"
);

struct IcrValue(pub u32);
impl IcrValue {
    rw_int_field!(u32, irq, 0, 31, u32, "Irq");
}

unsafe extern "Rust" {
    static PW_KERNEL_INTERRUPT_TABLE: &'static [InterruptTableEntry<Plic>];
}

pub struct Plic {}

impl Plic {
    pub const fn new() -> Self {
        Self {}
    }
}

impl RiscVInterruptController for Plic {
    fn handle_interrupt(interrupt: Interrupt, from_userspace: bool) {
        match interrupt {
            Interrupt::MachineTimer => timer::mtimer_tick(from_userspace),
            Interrupt::MachineExternal => Self::external_interrupt(from_userspace),
            // The PLIC only routes machine external interrupts; hart-local
            // lines and other standard interrupt codes are not supported.
            Interrupt::Local(_) | Interrupt::Unknown(_) => {
                pw_assert::panic!(
                    "Unhandled interrupt: interrupt_number={:#010x}",
                    interrupt.raw_cause() as usize
                );
            }
        }
    }
}

impl Plic {
    fn external_interrupt(from_userspace: bool) {
        // Claim the interrupt
        let icr = Icr;
        let irq = icr.read(&CONTEXT_0).irq();

        if irq == 0 {
            return;
        }

        debug_if!(LOG_INTERRUPTS, "Interrupt {}", irq as u32);

        // This lookup is not behind a lock as the table is static.  A locking scheme
        // will be required if we ever make the interrupt table dynamic.
        let Some(handler) = unsafe { PW_KERNEL_INTERRUPT_TABLE }
            .get(irq as usize)
            .copied()
            .flatten()
        else {
            pw_assert::panic!("Unhandled interrupt: irq={}", irq as u32);
        };

        unsafe { handler(from_userspace) };

        // It is up to the interrupt handler to call userspace_interrupt_ack()
        // or kernel_interrupt_handler_exit() (kernel drivers) to release the claim.
    }
}

impl InterruptController for Plic {
    type InterruptHandler = unsafe extern "C" fn(bool);

    fn early_init(&self) {
        info!(
            "Initializing PLIC {:#x}",
            PlicConfig::PLIC_BASE_ADDRESS as usize
        );

        const GLOBAL_PRIORITY: u32 = 0;
        const IRQ_PRIORITY: u32 = 1;

        // Disable all interrupt sources at init and provide a default priority of 1 (lowest).
        // It is up to the kernel driver or interrupt object to enable the interrupts (and
        // optionally change the priority).
        // Start at 1, as interrupt source 0 is reserved.
        for irq in 1..PlicConfig::MAX_IRQS {
            Self::disable_interrupt(irq);
            set_interrupt_priority(irq, IRQ_PRIORITY);
        }

        debug_if!(
            LOG_INTERRUPTS,
            "Setting global priority to {}",
            GLOBAL_PRIORITY as u32
        );
        set_global_priority(GLOBAL_PRIORITY);

        unsafe {
            riscv::register::mie::set_mext();
        }
    }

    fn enable_interrupt(irq: u32) {
        debug_if!(LOG_INTERRUPTS, "Enable interrupt {}", irq as u32);
        set_interrupt_enable(irq, true);
    }

    fn disable_interrupt(irq: u32) {
        debug_if!(LOG_INTERRUPTS, "Disable interrupt {}", irq as u32);
        set_interrupt_enable(irq, false);
    }

    fn userspace_interrupt_ack(irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler ack",
            irq as u32
        );
        // Release the claim on the interrupt
        let mut icr = Icr;
        icr.write(&CONTEXT_0, IcrValue(irq));
    }

    fn userspace_interrupt_handler_enter<K: Kernel>(_kernel: K, irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler enter",
            irq as u32
        );
    }

    fn userspace_interrupt_handler_exit<K: Kernel>(
        _kernel: K,
        irq: u32,
        _guard: InterruptGuard<K>,
    ) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler exit",
            irq as u32
        );
    }

    fn kernel_interrupt_handler_enter<K: Kernel>(_kernel: K, irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler enter",
            irq as u32
        );
    }

    fn kernel_interrupt_handler_exit<K: Kernel>(_kernel: K, irq: u32, _guard: InterruptGuard<K>) {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler exit",
            irq as u32
        );
        // Release the claim on the interrupt
        let mut icr = Icr;
        icr.write(&CONTEXT_0, IcrValue(irq));
    }

    fn enable_interrupts() {
        debug_if!(LOG_INTERRUPTS, "Enable interrupts");
        unsafe {
            riscv::register::mstatus::set_mie();
        }
    }

    fn disable_interrupts() {
        debug_if!(LOG_INTERRUPTS, "Disable interrupts");
        unsafe {
            riscv::register::mstatus::clear_mie();
        }
    }

    fn interrupts_enabled() -> bool {
        let mie = riscv::register::mstatus::read().mie();
        debug_if!(
            LOG_INTERRUPTS,
            "Interrupts enabled: {}",
            u8::from(mie) as u8
        );
        mie
    }

    fn trigger_interrupt(_irq: u32) {
        pw_assert::panic!("trigger_interrupt not supported on the PLIC");
    }
}

fn set_interrupt_enable(irq: u32, enable: bool) {
    let mut ier: Ier = Ier {};
    let enabled_sources = ier.read(&CONTEXT_0, irq).0;
    let bitmask = 1 << (irq % 32);
    let new_enabled_sources = if enable {
        enabled_sources | bitmask
    } else {
        enabled_sources & !bitmask
    };

    ier.write(&CONTEXT_0, irq, IerValue(new_enabled_sources));
}

fn set_global_priority(priority: u32) {
    let mut ipr = Ipr {};
    ipr.write(&CONTEXT_0, 0, IprValue(priority));
}

fn set_interrupt_priority(irq: u32, priority: u32) {
    let mut ipr = Ipr {};
    ipr.write(&CONTEXT_0, irq, IprValue(priority));
}
