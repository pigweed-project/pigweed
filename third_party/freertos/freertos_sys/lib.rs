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
#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

// Bazel includes the module as part of the source.
#[cfg(not(rust_bindgen_include))]
mod freertos_sys_bindgen;

// GN requires that the bingen source be included explicitly.
#[cfg(rust_bindgen_include)]
mod freertos_sys_bindgen {
    core::include!(env!("BINDGEN_RS_FILE"));
}

pub use freertos_sys_bindgen::*;
pub use freertos_sys_taskENTER_CRITICAL as taskENTER_CRITICAL;
pub use freertos_sys_taskENTER_CRITICAL_FROM_ISR as taskENTER_CRITICAL_FROM_ISR;
pub use freertos_sys_taskEXIT_CRITICAL as taskEXIT_CRITICAL;
pub use freertos_sys_taskEXIT_CRITICAL_FROM_ISR as taskEXIT_CRITICAL_FROM_ISR;
pub use freertos_sys_taskYIELD as taskYIELD;
