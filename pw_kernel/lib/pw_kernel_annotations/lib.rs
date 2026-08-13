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

pub const STACK_SECTION_NAME: &str = ".pw_kernel.annotations.stack";
pub const THREAD_SECTION_NAME: &str = ".pw_kernel.annotations.thread";
pub const PROCESS_SECTION_NAME: &str = ".pw_kernel.annotations.process";
pub const TRACE_BUFFER_SECTION_NAME: &str = ".pw_kernel.annotations.trace_buffer";
pub const TOKENIZER_SECTION_NAME: &str = ".pw_tokenizer.entries";
pub const DEBUG_MAILBOX_SECTION_NAME: &str = ".pw_kernel.annotations.debug_mailbox";

pub mod image_info;

pub use image_info::{
    DebugMailboxInfo, ImageInfo, ProcessInfo, StackInfo, ThreadInfo, TraceBufferInfo,
};
