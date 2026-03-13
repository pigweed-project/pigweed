// Copyright 2024 The Pigweed Authors
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

mod backend_tests;

#[cfg(not(target_os = "macos"))]
#[cfg(test)]
fn flush_stdout() {
    // Safety: Test only.  Calling into a libc function w/o any dependency on
    // data from the Rust side.
    unsafe {
        unsafe extern "C" {
            static stdout: *mut libc::FILE;
        }
        libc::fflush(stdout);
    }
}

#[cfg(target_os = "macos")]
#[cfg(test)]
fn flush_stdout() {
    // Safety: Test only.  Calling into a libc function w/o any dependency on
    // data from the Rust side.
    unsafe {
        unsafe extern "C" {
            // MacOS uses a #define to declare stdout so we need to expand that
            // manually.
            static __stdoutp: *mut libc::FILE;
        }
        libc::fflush(__stdoutp);
    }
}

// Runs `action` while capturing stdout and returns the captured output.
#[cfg(test)]
fn run_with_capture<F: FnOnce()>(action: F) -> String {
    // Use statements here instead of at the module level to scope them to the
    // above #[cfg(test)]
    use std::fs::File;
    use std::io::{Read, stdout};
    use std::os::fd::AsRawFd;

    use nix::unistd::{dup, dup2, pipe};

    // Capture the output of printf by creating a pipe and replacing
    // `STDOUT_FILENO` with the write side of the pipe.  This only works on
    // POSIX platforms (i.e. not Windows).  Because the backend is written
    // against libc's printf, the behavior should be the same on POSIX and
    // Windows so there is little incremental benefit from testing on Windows
    // as well.

    // Grab a lock on stdout to prevent others (test framework and other tests)
    // from writing while we capture output.
    let stdout = stdout().lock();

    let stdout_fd = stdout.as_raw_fd();

    // Duplicate the current stdout so we can restore it after the test.  Keep
    // it as an owned fd,
    let old_stdout = dup(stdout_fd).unwrap();

    // Create a pipe that will let us read stdout.
    let (pipe_rx, pipe_tx) = pipe().unwrap();

    // Since `printf` buffers output, we need to call into libc to flush the
    // buffers before swapping stdout.
    flush_stdout();

    // Replace stdout with our pipe.
    dup2(pipe_tx.as_raw_fd(), stdout_fd).unwrap();

    action();

    // Flush buffers again before restoring stdout.
    flush_stdout();

    // Restore old stdout.
    dup2(old_stdout, stdout_fd).unwrap();

    // Drop the writer side of the pipe to close it so that read will see an
    // EOF.
    drop(pipe_tx);

    // Read the read side of the pipe into a `String`.
    let mut pipe_rx: File = pipe_rx.into();
    let mut output = String::new();
    pipe_rx.read_to_string(&mut output).unwrap();

    output
}
