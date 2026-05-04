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
use crate::scheduler::SchedulerState;
use crate::scheduler::thread::{Process, ProcessRef};
use crate::sync::spinlock::{SpinLock, SpinLockGuard};

#[derive(Clone)]
pub struct MainThread<K: Kernel> {
    pub thread: foreign_box::ForeignRc<K::AtomicUsize, super::thread::ThreadObject<K>>,
    pub initial_pc: usize,
    pub initial_sp: usize,
    pub args: (usize, usize, usize),
}

enum ProcessState<K: Kernel> {
    Stopped(ForeignBox<Process<K>>),
    Running(ProcessRef<K>),
    Empty,
}

impl<K: Kernel> ProcessState<K> {
    fn take_process(&mut self) -> Option<ForeignBox<Process<K>>> {
        match core::mem::replace(self, ProcessState::Empty) {
            ProcessState::Stopped(process) => Some(process),
            state => {
                *self = state;
                None
            }
        }
    }
}

struct State<K: Kernel> {
    process_state: ProcessState<K>,
    main_thread: Option<MainThread<K>>,
}

pub struct ProcessObject<K: Kernel> {
    base: ObjectBase<K>,
    state: SpinLock<K, State<K>>,
}

impl<K: Kernel> ProcessObject<K> {
    #[must_use]
    pub const fn new(process: ForeignBox<Process<K>>, main_thread: Option<MainThread<K>>) -> Self {
        Self {
            base: ObjectBase::new(Signals::no_active()),
            state: SpinLock::new(State {
                process_state: ProcessState::Stopped(process),
                main_thread,
            }),
        }
    }

    #[must_use]
    pub const fn new_empty() -> Self {
        Self {
            base: ObjectBase::new(Signals::no_active()),
            state: SpinLock::new(State {
                process_state: ProcessState::Empty,
                main_thread: None,
            }),
        }
    }

    pub fn set_process(
        &self,
        kernel: K,
        process: ForeignBox<Process<K>>,
        main_thread: Option<MainThread<K>>,
    ) -> Result<()> {
        let mut state = self.state.lock(kernel);
        match state.process_state {
            ProcessState::Empty => {
                state.process_state = ProcessState::Stopped(process);
                state.main_thread = main_thread;
                Ok(())
            }
            _ => Err(Error::AlreadyExists),
        }
    }

    pub fn start(&self, kernel: K) -> Result<()> {
        let mut state = self.state.lock(kernel);

        let Some(mut process) = state.process_state.take_process() else {
            return Err(Error::AlreadyExists);
        };

        // SAFETY: `ProcessObject` is a kernel object that is always managed
        // within a `ForeignRcState` (e.g., when wrapped in `ForeignRc` or
        // `ForeignBox` for the object table). Thus, the `&self` reference
        // points to storage contained inside a `ForeignRcState`.
        let self_rc = unsafe { foreign_box::ForeignRcState::create_ref_from_inner(self) };
        process.object = Some(self_rc.clone());

        let process_ref = crate::scheduler::add_process(kernel, process);
        state.process_state = ProcessState::Running(process_ref.clone());

        if let Some(main_thread) = &state.main_thread {
            main_thread.thread.start(
                kernel,
                process_ref,
                main_thread.initial_pc,
                main_thread.initial_sp,
                main_thread.args,
            )?;
        }

        Ok(())
    }

    pub fn terminate(&self, kernel: K) -> Result<()> {
        let process_ref = {
            let state = self.state.lock(kernel);
            match &state.process_state {
                ProcessState::Running(process_ref) => process_ref.clone(),
                ProcessState::Stopped(_) => return Err(Error::FailedPrecondition),
                ProcessState::Empty => return Err(Error::Internal),
            }
        };
        process_ref.terminate(kernel, ExitStatus::TerminatedBySyscall)
    }

    pub fn join(&self, kernel: K) -> Result<ExitStatus> {
        let process_ref = {
            let mut state = self.state.lock(kernel);
            match core::mem::replace(&mut state.process_state, ProcessState::Empty) {
                ProcessState::Running(p) => p,
                old_state => {
                    state.process_state = old_state;
                    return Err(Error::FailedPrecondition);
                }
            }
        };

        use crate::scheduler::ProcessTryJoinResult;
        match process_ref.try_join(kernel) {
            ProcessTryJoinResult::Joined(process, status) => {
                #[cfg(feature = "user_space")]
                process.reset_objects(kernel)?;
                let mut state = self.state.lock(kernel);
                state.process_state = ProcessState::Stopped(process);
                Ok(status)
            }
            ProcessTryJoinResult::Wait(process_ref) => {
                let mut state = self.state.lock(kernel);
                state.process_state = ProcessState::Running(process_ref);
                Err(Error::Unavailable)
            }
            ProcessTryJoinResult::Err {
                error,
                process: process_ref,
            } => {
                let mut state = self.state.lock(kernel);
                state.process_state = ProcessState::Running(process_ref);
                Err(error)
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

impl<K: Kernel> KernelObject<K> for ProcessObject<K> {
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

    fn process_start(&self, kernel: K) -> Result<()> {
        self.start(kernel)
    }

    fn process_terminate(&self, kernel: K) -> Result<()> {
        self.terminate(kernel)
    }

    fn process_join(&self, kernel: K) -> Result<ExitStatus> {
        self.join(kernel)
    }
}
