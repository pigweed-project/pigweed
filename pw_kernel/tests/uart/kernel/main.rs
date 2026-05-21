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
#![no_std]

use kernel::Kernel;
use pw_status::Result;

// The test assumes that any UART implementing this trait is backed by
// a buffer.
// It also assumes that read() will not block when there is no data.
// If the UART buffer is full, write() will fail until it's read from.
pub trait TestUart {
    fn enable_loopback();
    fn read() -> Result<Option<u8>>;
    fn write(byte: u8) -> Result<()>;
}

pub fn main<K: Kernel, U: TestUart>(_kernel: K) -> Result<()> {
    test_logger::start("Kernel UART Test");

    // enable lo to support writing and then reading back the result.
    U::enable_loopback();

    pw_assert::assert!(
        U::read().is_ok_and(|val| val.is_none()),
        "UART buffer initially empty"
    );

    U::write(7)?;
    if let Ok(Some(value)) = U::read() {
        pw_assert::eq!(value as u8, 7 as u8);
    } else {
        pw_assert::panic!("U::read() returned no value");
    }
    pw_assert::assert!(
        U::read().is_ok_and(|val| val.is_none()),
        "Buffer empty after read"
    );

    for i in 0..3 {
        U::write(i)?;
    }

    for i in 0..3u8 {
        if let Ok(Some(value)) = U::read() {
            pw_assert::eq!(value as u8, i as u8);
        } else {
            pw_assert::panic!("U::read() returned no value");
        }
    }
    pw_assert::assert!(
        U::read().is_ok_and(|val| val.is_none()),
        "Buffer empty multiple reads"
    );

    test_logger::passed("Kernel UART Test");
    Ok(())
}
