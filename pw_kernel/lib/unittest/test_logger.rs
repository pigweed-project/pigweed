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

pub use pw_log;

/// Logs the start of a test case.
pub fn start(name: &str) {
    pw_log::info!("🔄 [{}] RUNNING", name as &str);
}

/// Logs that a test case passed.
pub fn passed(name: &str) {
    pw_log::info!("✅ [{}] PASSED", name as &str);
}

/// Logs that a test case failed.
pub fn failed(name: &str) {
    pw_log::error!("❌ [{}] FAILED", name as &str);
}

#[macro_export]
macro_rules! info {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::info!("├─ " PW_FMT_CONCAT $format_string, $($args),*)
    };
}

#[macro_export]
macro_rules! error {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::error!("❌ " PW_FMT_CONCAT $format_string, $($args),*)
    };
}

#[macro_export]
macro_rules! step_start {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::info!("├─ 🔄 " PW_FMT_CONCAT $format_string, $($args),*)
    };
}

#[macro_export]
macro_rules! step_info {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::info!("│  ├─ " PW_FMT_CONCAT $format_string, $($args),*)
    };
}

#[macro_export]
macro_rules! step_passed {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::info!("│  ✅ " PW_FMT_CONCAT $format_string, $($args),*)
    };
}

#[macro_export]
macro_rules! step_failed {
    ($format_string:literal $(, $args:expr)* $(,)?) => {
        $crate::pw_log::error!("│  ❌ " PW_FMT_CONCAT $format_string, $($args),*)
    };
}
