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

// The Rust test framework uses `set_output_capture()` to redirect stdio output.
// Since we need to capture stdout in this test, we must use this API as well.
#![cfg_attr(test, feature(internal_output_capture))]
mod backend_tests;

// Runs `action` while capturing `println!` output and returns the
// captured output.
#[cfg(test)]
fn run_with_capture<F: FnOnce()>(action: F) -> String {
    // Use statements here instead of at the module level to scope them to the
    // above #[cfg(test)]
    use std::sync::{Arc, Mutex};

    let output = Arc::new(Mutex::new(Vec::new()));
    let old_capture = std::io::set_output_capture(Some(output.clone()));

    action();

    std::io::set_output_capture(old_capture);
    let output_data = output.lock().unwrap();
    String::from_utf8((*output_data).to_vec()).unwrap()
}
