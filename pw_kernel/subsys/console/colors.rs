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

use pw_bytes::concat_static_strs;
use pw_log_backend_api::LogLevel;

pub const RESET: &str = "\x1b[0m";
pub const BOLD: &str = "\x1b[1m";
pub const DIM: &str = "\x1b[2m";

pub const BLACK: &str = "\x1b[30m";
pub const RED: &str = "\x1b[31m";
pub const GREEN: &str = "\x1b[32m";
pub const YELLOW: &str = "\x1b[33m";
pub const BLUE: &str = "\x1b[34m";
pub const MAGENTA: &str = "\x1b[35m";
pub const CYAN: &str = "\x1b[36m";
pub const WHITE: &str = "\x1b[37m";

#[must_use]
pub const fn log_level_tag(level: LogLevel) -> &'static str {
    // TODO(b/480216450): Using concat_static_strs! inside match level block
    // directly crashes Clippy. Fix the Clippy crash upstream before changing
    // the pattern back.
    if cfg!(feature = "color") {
        const DBG: &str = concat_static_strs!(DIM, WHITE, "DBG", RESET);
        const INF: &str = concat_static_strs!(DIM, CYAN, "INF", RESET);
        const WRN: &str = concat_static_strs!(DIM, YELLOW, "WRN", RESET);
        const ERR: &str = concat_static_strs!(DIM, RED, "ERR", RESET);
        const CRT: &str = concat_static_strs!(DIM, RED, "CRT", RESET);
        const FTL: &str = concat_static_strs!(DIM, RED, "FTL", RESET);
        match level {
            LogLevel::Debug => DBG,
            LogLevel::Info => INF,
            LogLevel::Warn => WRN,
            LogLevel::Error => ERR,
            LogLevel::Critical => CRT,
            LogLevel::Fatal => FTL,
        }
    } else {
        match level {
            LogLevel::Debug => "DBG",
            LogLevel::Info => "INF",
            LogLevel::Warn => "WRN",
            LogLevel::Error => "ERR",
            LogLevel::Critical => "CRT",
            LogLevel::Fatal => "FTL",
        }
    }
}
