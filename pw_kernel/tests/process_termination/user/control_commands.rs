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

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
pub enum Command {
    ModifyState = 1,
    VerifyReset = 2,
    BlockInitiator = 3,
    AsyncTransact = 4,
    SleepAndExit = 5,
}

impl TryFrom<u8> for Command {
    type Error = pw_status::Error;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Command::ModifyState),
            2 => Ok(Command::VerifyReset),
            3 => Ok(Command::BlockInitiator),
            4 => Ok(Command::AsyncTransact),
            5 => Ok(Command::SleepAndExit),
            _ => Err(pw_status::Error::InvalidArgument),
        }
    }
}
