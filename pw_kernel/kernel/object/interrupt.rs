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

use pw_status::Result;
use pw_time_core::Instant;

use crate::Kernel;
use crate::object::{KernelObject, ObjectBase, Signals, WaitReturn};

/// Object for handling userspace interrupts.
pub struct InterruptObject<K: Kernel> {
    base: ObjectBase<K>,
    ack_irqs: fn(Signals),
}

impl<K: Kernel> InterruptObject<K> {
    #[must_use]
    pub const fn new(ack_irqs: fn(Signals)) -> Self {
        Self {
            base: ObjectBase::new(Signals::no_active()),
            ack_irqs,
        }
    }

    pub fn interrupt(&self, kernel: K, signal_mask: Signals) {
        self.base.signal(kernel, |current| current | signal_mask);
    }
}

impl<K: Kernel> KernelObject<K> for InterruptObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.base)
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<K::Clock>,
    ) -> Result<WaitReturn> {
        self.base.wait_until(kernel, signal_mask, deadline)
    }

    fn interrupt_ack(&self, kernel: K, signal_mask: Signals) -> Result<()> {
        // Clear the signaled interrupts.
        self.base.signal(kernel, |signals| signals - signal_mask);
        (self.ack_irqs)(signal_mask);
        Ok(())
    }
}
