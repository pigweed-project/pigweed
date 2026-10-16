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

#[cfg(test)]
mod tests {
    // TODO: davidroth - This won't work with out of tree architecture.
    // We need to re-visit integration testing to resolve this issue and
    // also not require each target to opt into integration tests.

    #[cfg(feature = "arch_arm_cortex_m")]
    use arch_arm_cortex_m::Arch;
    #[cfg(feature = "arch_riscv")]
    use arch_riscv::Arch;
    use kernel::sync::spinlock::{BareSpinLock, InterruptsOnDrop, SpinLock};
    use unittest::test;

    type ConcreteBareSpinLock = <Arch as kernel::Arch>::BareSpinLock;

    #[test]
    fn bare_try_lock_returns_correct_value() -> unittest::Result<()> {
        let lock = ConcreteBareSpinLock::new();

        {
            let _sentinel = lock.lock();
            unittest::assert_true!(lock.try_lock().is_none());
        }

        unittest::assert_true!(lock.try_lock().is_some());

        Ok(())
    }

    #[test]
    fn try_lock_returns_correct_value() -> unittest::Result<()> {
        let lock = SpinLock::<Arch, _>::new(false);

        {
            let mut guard = lock.lock(Arch);
            *guard = true;
            unittest::assert_true!(lock.try_lock(Arch).is_none());
        }

        let guard = lock.lock(Arch);
        unittest::assert_true!(*guard);

        Ok(())
    }

    #[test]
    fn adopt_locked_adopts_and_releases_lock() -> unittest::Result<()> {
        use kernel::interrupt_controller::InterruptController as _;

        let interrupts_were_enabled =
            <Arch as kernel::Arch>::InterruptController::interrupts_enabled();
        let lock = SpinLock::<Arch, _>::new(false);

        // 1. Adopt with `InterruptsOnDrop::KeepDisabled`: dropping the guard
        // unlocks `lock` and decrements `preempt_disable_count`, but keeps
        // interrupts disabled.
        core::mem::forget(lock.lock(Arch));
        let try_lock_while_held = lock.try_lock(Arch).is_none();
        {
            // SAFETY: `lock` is held via the forgotten guard above, and
            // interrupts and preemption are disabled.
            let mut guard = unsafe { lock.adopt_locked(Arch, InterruptsOnDrop::KeepDisabled) };
            *guard = true;
        }
        let interrupts_after_drop_false =
            <Arch as kernel::Arch>::InterruptController::interrupts_enabled();

        // 2. Re-lock while interrupts are disabled, then adopt and restore the
        // initial interrupt state on drop.
        core::mem::forget(lock.lock(Arch));
        let value_in_second_adopt = {
            let interrupts_on_drop = if interrupts_were_enabled {
                InterruptsOnDrop::Restore
            } else {
                InterruptsOnDrop::KeepDisabled
            };
            // SAFETY: `lock` is held via the forgotten guard above, and
            // interrupts and preemption are disabled.
            let guard = unsafe { lock.adopt_locked(Arch, interrupts_on_drop) };
            *guard
        };

        unittest::assert_true!(try_lock_while_held);
        unittest::assert_false!(interrupts_after_drop_false);
        unittest::assert_true!(value_in_second_adopt);
        unittest::assert_eq!(
            <Arch as kernel::Arch>::InterruptController::interrupts_enabled(),
            interrupts_were_enabled
        );
        let guard = lock.try_lock(Arch);
        unittest::assert_true!(guard.is_some());
        unittest::assert_true!(*guard.unwrap());

        Ok(())
    }
}
