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

/// Exit status of a process or thread.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(C, usize)]
#[non_exhaustive]
pub enum ExitStatus {
    //
    // 0 is left out as a niche for Option<ExitStatus>.
    //
    /// The process or thread exited successfully with the given status code.
    Success(u32) = 1,
    /// The process or thread was terminated due to an unhandled exception.
    UnhandledException(usize) = 2,
    /// The process or thread was forcibly terminated by another entity via a syscall.
    TerminatedBySyscall = 3,
    /// The process or thread was directly terminated by the kernel.
    TerminatedByKernel = 4,
    /// The thread was terminated because its containing process was terminated.
    ProcessTerminated = 5,
    //
    // Discriminant values must be within the range of positive isize values.
    //
}

// Enforce that ExitStatus can cleanly fit into the two register sized
// SysCallReturnValue.
const _: () = assert!(core::mem::size_of::<ExitStatus>() == 2 * core::mem::size_of::<usize>());
