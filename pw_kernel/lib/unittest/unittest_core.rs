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

pub use pw_bytes;
pub use test_logger;

// SAFETY: Mutation of `TEST_LIST` is only permitted to occur in `add_test`.
static mut TEST_LIST: Option<&'static Test> = None;

fn iter_tests() -> impl Iterator<Item = &'static Test> {
    // SAFETY: The only mutation of `TEST_LIST` occurs in `add_test`, and
    // callers of `add_test` promise that calls don't overlap with any other
    // calls to methods or functions exposed by this crate.
    let mut next = unsafe { TEST_LIST };
    core::iter::from_fn(move || match next {
        None => None,
        Some(test) => {
            next = test.next;
            Some(test)
        }
    })
}

/// # Safety
///
/// The caller must ensure that all no call to `add_test` overlaps with any
/// other call to `add_test` or any call to any other methods or functions
/// exposed by this crate.
pub unsafe fn add_test(test: &'static mut Test) {
    // SAFETY: The caller has promised that we don't overlap with any other code
    // which accesses `TEST_LIST`. We don't permit `&mut TEST_LIST` to outlive
    // this function, so `&mut TEST_LIST` will not co-exist with any other
    // accesses to `TEST_LIST`.
    #[allow(static_mut_refs)]
    let head = unsafe { &mut TEST_LIST };
    if let Some(head) = head {
        test.next = Some(head);
    }
    *head = Some(test);
}

#[derive(Eq, PartialEq)]
pub enum TestsResult {
    AllPassed,
    SomeFailed,
}

// We use macros for `run_bare_metal_tests` and `run_all_tests` so that we don't
// have to take a dependency on `pw_log`, and can instead rely on those macros'
// callers taking dependencies on `pw_log`. Specifically, `declare_loggers!` is
// evaluated in the callers' context.
//
// This allows us to use this crate to test crates which are transitive
// dependencies of `pw_log`.

#[macro_export]
macro_rules! run_bare_metal_tests {
    () => {{
        let (log_start, log_error, log_pass) = $crate::declare_loggers!();
        $crate::run_bare_metal_tests(log_start, log_error, log_pass)
    }};
}

#[macro_export]
macro_rules! run_all_tests {
    () => {{
        let (log_start, log_error, log_pass) = $crate::declare_loggers!();
        $crate::run_all_tests(log_start, log_error, log_pass)
    }};
}

#[doc(hidden)]
#[macro_export]
macro_rules! declare_loggers {
    () => {
        (
            |test: &$crate::Test| $crate::test_logger::start(test.name),
            |test: &$crate::Test, e: $crate::TestError| {
                $crate::test_logger::failed(test.name);
                pw_log::error!("❌ ├─ {}:{}:", e.file as &str, e.line as u32);
                pw_log::error!("❌ └─ {}", e.message as &str);
            },
            |test: &$crate::Test| $crate::test_logger::passed(test.name),
        )
    };
}

#[doc(hidden)]
pub fn run_bare_metal_tests(
    log_start: impl Fn(&Test),
    log_error: impl Fn(&Test, TestError),
    log_pass: impl Fn(&Test),
) -> TestsResult {
    run_tests(TestSet::BareMetal, log_start, log_error, log_pass)
}

#[doc(hidden)]
pub fn run_all_tests(
    log_start: impl Fn(&Test),
    log_error: impl Fn(&Test, TestError),
    log_pass: impl Fn(&Test),
) -> TestsResult {
    let bare_metal_result = run_tests(TestSet::BareMetal, &log_start, &log_error, &log_pass);
    let kernel_result = run_tests(TestSet::Kernel, log_start, log_error, log_pass);

    use TestsResult::*;
    match (bare_metal_result, kernel_result) {
        (AllPassed, AllPassed) => AllPassed,
        _ => SomeFailed,
    }
}

fn run_tests(
    set: TestSet,
    log_start: impl Fn(&Test),
    log_error: impl Fn(&Test, TestError),
    log_pass: impl Fn(&Test),
) -> TestsResult {
    let mut result = TestsResult::AllPassed;
    iter_tests().for_each(|test| {
        if test.set != set {
            return;
        }

        log_start(test);
        if let Err(e) = (test.test_fn)() {
            log_error(test, e);
            result = TestsResult::SomeFailed;
        } else {
            log_pass(test);
        }
    });
    result
}

pub struct TestError {
    pub file: &'static str,
    pub line: u32,
    pub message: &'static str,
}

pub type Result<T> = core::result::Result<T, TestError>;

pub type TestFn = fn() -> Result<()>;

/// Which set of tests is this test a member of?
#[derive(PartialEq)]
pub enum TestSet {
    /// Bare metal tests run with or without a kernel.
    BareMetal,
    /// Kernel tests only run when a kernel is present and has been initialized.
    Kernel,
}

pub struct Test {
    pub name: &'static str,
    pub test_fn: TestFn,
    pub set: TestSet,
    pub next: Option<&'static Test>,
}

impl Test {
    pub const fn new(name: &'static str, test_fn: TestFn, set: TestSet) -> Self {
        Self {
            name,
            test_fn,
            set,
            next: None,
        }
    }
}

// We're marking these as send and sync so that we can declare statics with.
// them.  They're not actually Send and Sync because they contain linked list
// pointers but in practice tests are single threaded and these are never sent
// between threads.
//
// A better pattern here must be worked out with intrusive lists of static data
// (for statically declared threads for instance) so we'll revisit this later.
unsafe impl Send for Test {}
unsafe impl Sync for Test {}

#[macro_export]
macro_rules! assert_eq {
    ($a:expr, $b:expr) => {
        if $a != $b {
            return Err(unittest::TestError {
                file: file!(),
                line: line!(),
                message: unittest::pw_bytes::concat_static_strs!(
                    "assert_eq!(",
                    stringify!($a),
                    ", ",
                    stringify!($b),
                    ") failed"
                ),
            });
        }
    };
}

#[macro_export]
macro_rules! assert_ne {
    ($a:expr, $b:expr) => {
        if $a == $b {
            return Err(unittest::TestError {
                file: file!(),
                line: line!(),
                message: unittest::pw_bytes::concat_static_strs!(
                    "assert_ne!(",
                    stringify!($a),
                    ", ",
                    stringify!($b),
                    ") failed"
                ),
            });
        }
    };
}

#[macro_export]
macro_rules! assert_matches {
    ($a:expr, $($pat:pat_param)|+ ) => {
        match $a {
            $($pat)|+ => (),
            _ => return Err(unittest::TestError {
                file: file!(),
                line: line!(),
                message: unittest::pw_bytes::concat_static_strs!(
                    "assert_matches!(",
                    stringify!($a),
                    ", ",
                    stringify!($($pat)|+ ),
                    ") failed"
                ),
            })
        }
    };
}

#[macro_export]
macro_rules! assert_true {
    ($a:expr) => {
        if !$a {
            return Err(unittest::TestError {
                file: file!(),
                line: line!(),
                message: unittest::pw_bytes::concat_static_strs!(
                    "assert_true!(",
                    stringify!($a),
                    ") failed"
                ),
            });
        }
    };
}

#[macro_export]
macro_rules! assert_false {
    ($a:expr) => {
        if $a {
            return Err(unittest::TestError {
                file: file!(),
                line: line!(),
                message: unittest::pw_bytes::concat_static_strs!(
                    "assert_false!(",
                    stringify!($a),
                    ") failed"
                ),
            });
        }
    };
}

#[macro_export]
macro_rules! unwrap {
    ($a:expr) => {
        match $a {
            Ok(v) => v,
            _ => {
                return Err(unittest::TestError {
                    file: file!(),
                    line: line!(),
                    message: unittest::pw_bytes::concat_static_strs!(
                        "unwrap!(",
                        stringify!($a),
                        ") failed"
                    ),
                })
            }
        }
    };
}
