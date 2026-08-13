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

//! Shared protocol definitions and messages for Pigweed Kernel debug mailboxes.

#![no_std]

use zerocopy::{Immutable, IntoBytes, KnownLayout, TryFromBytes};

/// Host commands sent via debug mailbox.
#[derive(
    Copy, Clone, Debug, Default, PartialEq, Eq, IntoBytes, Immutable, KnownLayout, TryFromBytes,
)]
#[repr(u32)]
#[non_exhaustive]
pub enum HostCommand {
    /// Initial or idle state.
    #[default]
    None = 0,
    /// Request target to exit.
    Exit = 0xdeadc0de,
}
