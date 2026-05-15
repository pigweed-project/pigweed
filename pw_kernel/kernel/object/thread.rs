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

use foreign_box::ForeignBox;
use pw_status::{Error, Result};
use syscall_defs::ExitStatus;

use crate::Kernel;
use crate::object::{KernelObject, ObjectBase, Signals};
use crate::scheduler::thread::{ProcessRef, Thread, ThreadRef};
use crate::scheduler::{SchedulerState, TryJoinResult};
use crate::sync::spinlock::{SpinLock, SpinLockGuard};

enum State<K: Kernel> {
    Stopped(ForeignBox<Thread<K>>),
    Running(ThreadRef<K>),
    Empty,
}

impl<K: Kernel> State<K> {
    fn take_thread(&mut self) -> Option<ForeignBox<Thread<K>>> {
        match core::mem::replace(self, State::Empty) {
            State::Stopped(thread) => Some(thread),
            state => {
                *self = state;
                None
            }
        }
    }
}

pub struct ThreadObject<K: Kernel> {
    base: ObjectBase<K>,
    state: SpinLock<K, State<K>>,
}

impl<K: Kernel> ThreadObject<K> {
    #[must_use]
    pub const fn new(thread: ForeignBox<Thread<K>>) -> Self {
        Self {
            base: ObjectBase::new(Signals::no_active()),
            state: SpinLock::new(State::Stopped(thread)),
        }
    }

    pub const fn new_empty() -> Self {
        Self {
            base: ObjectBase::new(Signals::no_active()),
            state: SpinLock::new(State::Empty),
        }
    }

    pub fn set_thread(&self, kernel: K, thread: ForeignBox<Thread<K>>) -> Result<()> {
        let mut state = self.state.lock(kernel);
        match *state {
            State::Empty => {
                *state = State::Stopped(thread);
                Ok(())
            }
            _ => Err(Error::AlreadyExists),
        }
    }

    pub fn start(
        &self,
        kernel: K,
        process_ref: ProcessRef<K>,
        initial_pc: usize,
        initial_sp: usize,
        args: (usize, usize, usize),
    ) -> Result<()> {
        let mut state = self.state.lock(kernel);
        let Some(mut thread) = state.take_thread() else {
            return Err(Error::AlreadyExists);
        };

        // SAFETY: `ThreadObject` is a kernel object that is always managed
        // within a `ForeignRcState` (e.g., when wrapped in `ForeignRc` or
        // `ForeignBox` for the object table). Thus, the `&self` reference
        // points to storage contained inside a `ForeignRcState`.
        let self_rc = unsafe { foreign_box::ForeignRcState::create_ref_from_inner(self) };
        thread.object = Some(self_rc.clone());

        unsafe {
            thread.initialize_non_priv_thread(kernel, process_ref, initial_pc, initial_sp, args)?;
        }
        let thread_ref = crate::scheduler::start_thread(kernel, thread);
        *state = State::Running(thread_ref);
        Ok(())
    }

    pub fn terminate(&self, kernel: K) -> Result<()> {
        let mut thread_ref = {
            let state = self.state.lock(kernel);
            match &*state {
                State::Running(thread_ref) => thread_ref.clone(),
                State::Stopped(_) => return Err(Error::FailedPrecondition),
                State::Empty => return Err(Error::Internal),
            }
        };
        // Call terminate without holding the state lock
        thread_ref.terminate(kernel, ExitStatus::TerminatedBySyscall)
    }

    pub fn join_locked<'a>(
        &self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
    ) -> (SpinLockGuard<'a, K, SchedulerState<K>>, Result<ExitStatus>) {
        let thread_ref = {
            let mut state = self.state.lock(kernel);
            match core::mem::replace(&mut *state, State::Empty) {
                State::Running(t) => t,
                old_state => {
                    *state = old_state;
                    return (sched, Err(Error::FailedPrecondition));
                }
            }
        };

        let res = thread_ref.try_join_locked(kernel, sched);

        match res {
            (sched, crate::scheduler::TryJoinResult::Joined(thread, status)) => {
                let mut state = self.state.lock(kernel);
                *state = State::Stopped(thread);
                (sched, Ok(status))
            }
            (sched, crate::scheduler::TryJoinResult::Wait(thread_ref)) => {
                let mut state = self.state.lock(kernel);
                *state = State::Running(thread_ref);
                (sched, Err(Error::Unavailable))
            }
            (
                sched,
                TryJoinResult::Err {
                    error,
                    thread: thread_ref,
                },
            ) => {
                let mut state = self.state.lock(kernel);
                *state = State::Running(thread_ref);
                (sched, Err(error))
            }
        }
    }

    pub(crate) fn signal_locked<'a, F: Fn(Signals) -> Signals>(
        &self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        update_fn: F,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        self.base.signal_locked(kernel, sched, update_fn)
    }
}

impl<K: Kernel> KernelObject<K> for ThreadObject<K> {
    fn base(&self) -> Option<&ObjectBase<K>> {
        Some(&self.base)
    }

    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: time::Instant<K::Clock>,
    ) -> Result<crate::object::WaitReturn> {
        self.base.wait_until(kernel, signal_mask, deadline)
    }

    fn thread_start(&self, kernel: K, initial_pc: usize, initial_sp: usize) -> Result<()> {
        let process = kernel.get_scheduler().lock(kernel).current_process_ref();
        self.start(kernel, process.clone(), initial_pc, initial_sp, (0, 0, 0))
    }

    fn task_terminate(&self, kernel: K) -> Result<()> {
        self.terminate(kernel)
    }

    fn task_join(&self, kernel: K) -> Result<ExitStatus> {
        let sched = kernel.get_scheduler().lock(kernel);
        let (_, res) = self.join_locked(kernel, sched);
        res
    }
}
