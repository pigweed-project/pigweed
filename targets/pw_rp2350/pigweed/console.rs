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
#![no_std]

use embedded_io::Write;
use hal::fugit::RateExtU32;
use hal::gpio::bank0::{Gpio12, Gpio13};
use hal::gpio::{FunctionUart, Pin, PullDown};
use hal::pac::UART0;
use hal::uart::{self, DataBits, StopBits, UartConfig, UartPeripheral};
use hal::Clock;
use kernel::sync::spinlock::SpinLock;
use pw_status::{Error, Result};
use rp235x_hal as hal;

const XTAL_FREQ_HZ: u32 = 12_000_000u32;

pub type ConsoleUart = UartPeripheral<
    uart::Enabled,
    UART0,
    (
        Pin<Gpio12, FunctionUart, PullDown>,
        Pin<Gpio13, FunctionUart, PullDown>,
    ),
>;

static UART: SpinLock<arch_arm_cortex_m::Arch, Option<ConsoleUart>> = SpinLock::new(None);

#[unsafe(no_mangle)]
pub fn console_backend_write_all(buf: &[u8]) -> Result<()> {
    let mut uart = UART.lock(arch_arm_cortex_m::Arch);
    match &mut (*uart) {
        Some(uart) => uart.write_all(buf).map_err(|_| Error::Unknown),
        None => Ok(()),
    }
}

pub fn register_uart(val: ConsoleUart) {
    let mut uart = UART.lock(arch_arm_cortex_m::Arch);
    *uart = Some(val)
}

pub fn init() {
    let mut pac = hal::pac::Peripherals::take().unwrap();

    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);

    let clocks = hal::clocks::init_clocks_and_plls(
        XTAL_FREQ_HZ,
        pac.XOSC,
        pac.CLOCKS,
        pac.PLL_SYS,
        pac.PLL_USB,
        &mut pac.RESETS,
        &mut watchdog,
    )
    .unwrap();

    let sio = hal::Sio::new(pac.SIO);

    let pins = hal::gpio::Pins::new(
        pac.IO_BANK0,
        pac.PADS_BANK0,
        sio.gpio_bank0,
        &mut pac.RESETS,
    );

    let uart_pins = (pins.gpio12.into_function(), pins.gpio13.into_function());
    let uart = hal::uart::UartPeripheral::new(pac.UART0, uart_pins, &mut pac.RESETS)
        .enable(
            UartConfig::new(115200.Hz(), DataBits::Eight, None, StopBits::One),
            clocks.peripheral_clock.freq(),
        )
        .unwrap();

    register_uart(uart);
}
