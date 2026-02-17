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

use core::ptr;

use kernel::interrupt_controller::{InterruptController, InterruptTableEntry};
use kernel::scheduler::PreemptDisableGuard;
use kernel::{Kernel, interrupt_controller};
use kernel_config::{VeerPicConfig, VeerPicConfigInterface};
use log_if::debug_if;
use pw_log::info;
use regs::{rw_block_reg, rw_bool_field, rw_int_field};

use crate::regs::veer::{MeiCPCT, MeiCPCTVal, MeiHAP, MeiPT, MeiPTVal};

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
        VeerPicConfig::PIC_BASE_ADDRESS
    }
}

// Marker type to ensure only the PIC can be passed to these block registers.
pub trait PicBaseAddress: regs::BaseAddress {}
impl PicBaseAddress for Context {}

// As per section 6 https://github.com/chipsalliance/Cores-VeeR-EH1/blob/main/docs/RISC-V_VeeR_EH1_PRM.pdf
// all registers are documented as 32-bit registers.

#[doc = "Machine External Interrupt Priority Level Registers"]
struct MeiPL {}
impl MeiPL {
    const REG_OFFSET: usize = 0;

    fn offset<A: PicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PicBaseAddress>(&mut self, addr: &A, irq: u32) -> MeiPLValue {
        let ptr: *const MeiPLValue =
            ptr::with_exposed_provenance::<MeiPLValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    fn write<A: PicBaseAddress>(&mut self, addr: &A, irq: u32, val: MeiPLValue) {
        let ptr = ptr::with_exposed_provenance_mut::<MeiPLValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[allow(dead_code)]
struct MeiPLValue(u32);
impl MeiPLValue {
    rw_int_field!(u32, priority, 0, 31, u32, "Priority");
}

// TODO(cfrantz): meip is currently unused.
// Remove if we determine this register is unneeded in a complete PIC implementation.
#[doc = "Machine External Interrupt Pending Registers"]
struct MeiP {}
impl MeiP {
    const REG_OFFSET: usize = 0x1000;

    fn offset<A: PicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PicBaseAddress>(&mut self, addr: &A, irq: u32) -> MeiPValue {
        let ptr: *const MeiPValue =
            ptr::with_exposed_provenance::<MeiPValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    #[allow(dead_code)]
    fn write<A: PicBaseAddress>(&mut self, addr: &A, irq: u32, val: MeiPValue) {
        let ptr = ptr::with_exposed_provenance_mut::<MeiPValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[allow(dead_code)]
struct MeiPValue(u32);
impl MeiPValue {
    rw_bool_field!(u32, pending, 0, "interrupt pending");
}

#[doc = "Machine External Interrupt Enables Registers"]
struct MeiE {}
impl MeiE {
    const REG_OFFSET: usize = 0x2000;

    fn offset<A: PicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PicBaseAddress>(&mut self, addr: &A, irq: u32) -> MeiEValue {
        let ptr = ptr::with_exposed_provenance::<MeiEValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    fn write<A: PicBaseAddress>(&mut self, addr: &A, irq: u32, val: MeiEValue) {
        let ptr = ptr::with_exposed_provenance_mut::<MeiEValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[allow(dead_code)]
struct MeiEValue(u32);
impl MeiEValue {
    rw_bool_field!(u32, enable, 0, "interrupt enable");
}

// TODO(cfrantz): mpicfg is currently unused.
// Remove if we determine this register is unneeded in a complete PIC implementation.
rw_block_reg!(
    MpiCfg,
    MpiCfgValue,
    u32,
    PicBaseAddress,
    0x3000,
    "Machine Priority Interrupt Config"
);

#[allow(dead_code)]
struct MpiCfgValue(pub u32);
impl MpiCfgValue {
    rw_int_field!(u32, prioord, 0, 3, u32, "global priority");
}

// TODO(cfrantz): meigwctrl is currently unused.
// Remove if we determine this register is unneeded in a complete PIC implementation.
#[doc = "Machine External Interrupt Gateway Control"]
struct MeiGwCtrl {}
impl MeiGwCtrl {
    const REG_OFFSET: usize = 0x4000;

    fn offset<A: PicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PicBaseAddress>(&mut self, addr: &A, irq: u32) -> MeiGwCtrlValue {
        let ptr = ptr::with_exposed_provenance::<MeiGwCtrlValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    #[allow(dead_code)]
    fn write<A: PicBaseAddress>(&mut self, addr: &A, irq: u32, val: MeiGwCtrlValue) {
        let ptr = ptr::with_exposed_provenance_mut::<MeiGwCtrlValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[allow(dead_code)]
struct MeiGwCtrlValue(u32);
impl MeiGwCtrlValue {
    rw_bool_field!(u32, polarity_low, 1, "active low interrupt");
    rw_bool_field!(u32, type_edge, 0, "edge triggered interrupt");
}

#[doc = "Machine External Interrupt Gateway Clear"]
struct MeiGwClr {}
impl MeiGwClr {
    const REG_OFFSET: usize = 0x5000;

    fn offset<A: PicBaseAddress>(&self, addr: &A, irq: u32) -> usize {
        addr.base_address() + Self::REG_OFFSET + (irq as usize * 4)
    }

    #[allow(dead_code)]
    fn read<A: PicBaseAddress>(&mut self, addr: &A, irq: u32) -> MeiGwClrValue {
        let ptr = ptr::with_exposed_provenance::<MeiGwClrValue>(self.offset(addr, irq));
        unsafe { ptr.read_volatile() }
    }

    fn write<A: PicBaseAddress>(&mut self, addr: &A, irq: u32, val: MeiGwClrValue) {
        let ptr = ptr::with_exposed_provenance_mut::<MeiGwClrValue>(self.offset(addr, irq));
        unsafe { ptr.write_volatile(val) }
    }
}

#[derive(Default)]
#[allow(dead_code)]
struct MeiGwClrValue(pub u32);

unsafe extern "Rust" {
    static PW_KERNEL_INTERRUPT_TABLE: &'static [InterruptTableEntry];
}

pub fn interrupt() {
    // Claim the interrupt.  The VeeR document leads me to believe that I
    // need to write to MEICPCT to capture the interrupt value into meihap.
    MeiCPCT::write(MeiCPCTVal::default());
    let meihap = MeiHAP::read();
    let irq = meihap.claimid() as u32;

    if irq == 0 {
        return;
    }

    // Disable the source and clear the claim on the interrupt.
    set_interrupt_enable(irq, false);
    let mut clr = MeiGwClr {};
    clr.write(&CONTEXT_0, irq, MeiGwClrValue::default());

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

    unsafe { handler() };

    // It is up to the interrupt handler to call userspace_interrupt_ack()
    // or kernel_interrupt_handler_exit() (kernel drivers) to release the claim.
}

pub struct VeerPic {}

impl VeerPic {
    pub const fn new() -> Self {
        Self {}
    }
}

impl InterruptController for VeerPic {
    fn early_init(&self) {
        info!(
            "Initializing PIC {:#x}",
            VeerPicConfig::PIC_BASE_ADDRESS as usize
        );

        const IRQ_PRIORITY: u32 = 1;

        // Disable all interrupt sources at init and provide a default priority of 1 (lowest).
        // It is up to the kernel driver or interrupt object to enable the interrupts (and
        // optionally change the priority).
        // Start at 1, as interrupt source 0 is reserved.
        for irq in 1..VeerPicConfig::MAX_IRQS {
            Self::disable_interrupt(irq);
            set_interrupt_priority(irq, IRQ_PRIORITY);
        }

        const GLOBAL_PRIORITY: u32 = 0;
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
        // TODO: do we need to release the claim via MeiGwClr?
        set_interrupt_enable(irq, true);
    }

    fn userspace_interrupt_handler_enter<K: Kernel>(kernel: K, irq: u32) -> PreemptDisableGuard<K> {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler enter",
            irq as u32
        );
        // For the PIC, the interrupt has already been claimed in the handler, so there
        // is no need to mask the interrupt here.
        PreemptDisableGuard::new(kernel)
    }

    fn userspace_interrupt_handler_exit<K: Kernel>(
        kernel: K,
        irq: u32,
        preempt_guard: PreemptDisableGuard<K>,
    ) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler exit",
            irq as u32
        );
        interrupt_controller::handler_done(kernel, preempt_guard);
    }

    fn kernel_interrupt_handler_enter<K: Kernel>(kernel: K, irq: u32) -> PreemptDisableGuard<K> {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler enter",
            irq as u32
        );
        PreemptDisableGuard::new(kernel)
    }

    fn kernel_interrupt_handler_exit<K: Kernel>(
        kernel: K,
        irq: u32,
        preempt_guard: PreemptDisableGuard<K>,
    ) {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler exit",
            irq as u32
        );
        // Release the claim on the interrupt
        // TODO: do we need to release the claim via MeiGwClr?
        set_interrupt_enable(irq, true);
        interrupt_controller::handler_done(kernel, preempt_guard);
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
        pw_assert::panic!("trigger_interrupt not supported on the PIC");
    }
}

fn set_interrupt_enable(irq: u32, enable: bool) {
    let mut ier: MeiE = MeiE {};
    ier.write(&CONTEXT_0, irq, MeiEValue(enable.into()));
}

fn set_global_priority(priority: u32) {
    MeiPT::write(MeiPTVal(priority as usize));
}

fn set_interrupt_priority(irq: u32, priority: u32) {
    let mut ipr = MeiPL {};
    ipr.write(&CONTEXT_0, irq, MeiPLValue(priority));
}
