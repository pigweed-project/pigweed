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

use kernel_config::{ClintTimerConfigInterface, KernelConfig, RiscVKernelConfigInterface};

use super::mtime::{MTime, MTimeCmp};

type ClintConfig = <KernelConfig as RiscVKernelConfigInterface>::Timer;

pub type ClintMTime = MTime<{ ClintConfig::MTIME_REGISTER }>;
pub type ClintMTimeCmp = MTimeCmp<{ ClintConfig::MTIMECMP_REGISTER }>;
