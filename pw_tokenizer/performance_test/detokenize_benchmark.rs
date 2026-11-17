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

//! Standalone Rust detokenizer benchmark paired with `detokenize_benchmark.cc`.
//!
//! This benchmark uses the same token database and test cases as
//! `//pw_tokenizer:detokenize_perf_test` (`pw_tokenizer/detokenize_perf_test.cc`),
//! running 1,000-iteration batch-timed loops with `black_box` barriers so that
//! Rust and C++ execution times and binary sizes can be compared directly.

// Variable format specifiers on stable Rust use `as` casts to annotate type
// information.
#![allow(clippy::unnecessary_cast)]

use core::hint::black_box;
use std::time::Instant;

use pw_tokenizer::detokenize::Detokenizer;

// Number of iterations to run each individual benchmark case per session.
const ITERATIONS: u32 = 1000;

// Token database identical to `kDataWithArguments` in
// `pw_tokenizer/detokenize_perf_test.cc`.
const DATA_WITH_ARGUMENTS: &[u8] = b"TOKENS\0\0\
    \x09\x00\x00\x00\
    \0\0\0\0\
    \x00\x00\x00\x00----\
    \x0A\x0B\x0C\x0D----\
    \x0E\x0F\x00\x01----\
    \xAA\xAA\xAA\xAA----\
    \xBB\xBB\xBB\xBB----\
    \xCC\xCC\xCC\xCC----\
    \xDD\xDD\xDD\xDD----\
    \xEE\xEE\xEE\xEE----\
    \xFF\xFF\xFF\xFF----\
    \0\
    Use the %s, %s.\0\
    Now there are %d of %s!\0\
    %c!\0\
    %hhu!\0\
    %hu!\0\
    %u!\0\
    %lu!\0\
    %llu!\0";

struct BinaryTestCase {
    name: &'static str,
    data: &'static [u8],
    expected: &'static str,
}

const BINARY_CASES: &[BinaryTestCase] = &[
    BinaryTestCase {
        name: "Detokenize_NoMessage",
        data: b"\x01\x02\x03\x04\x05\x06",
        expected: "",
    },
    BinaryTestCase {
        name: "Detokenize_NoArgs",
        data: b"\x00\x00\x00\x00",
        expected: "",
    },
    BinaryTestCase {
        name: "Detokenize_OneArg",
        data: b"\xAA\xAA\xAA\xAA\xfc\x01",
        expected: "~!",
    },
    BinaryTestCase {
        name: "Detokenize_TwoArgs1",
        data: b"\x0E\x0F\x00\x01\x04\x04them",
        expected: "Now there are 2 of them!",
    },
    BinaryTestCase {
        name: "Detokenize_TwoArgs2",
        data: b"\x0E\x0F\x00\x01\x80\x01\x04them",
        expected: "Now there are 64 of them!",
    },
];

struct TextTestCase {
    name: &'static str,
    text: &'static str,
    expected: &'static str,
}

const TEXT_CASES: &[TextTestCase] = &[
    TextTestCase {
        name: "DetokenizeText_NoMessage",
        text: "Nothing!!",
        expected: "Nothing!!",
    },
    TextTestCase {
        name: "DetokenizeText_NoArgs",
        text: "$AAAAAA==",
        expected: "",
    },
    TextTestCase {
        name: "DetokenizeText_OneArg",
        text: "$qqqqqvwB",
        expected: "~!",
    },
    TextTestCase {
        name: "DetokenizeText_TwoArgs1",
        text: "$Dg8AAQQEdGhlbQ==",
        expected: "Now there are 2 of them!",
    },
    TextTestCase {
        name: "DetokenizeText_TwoArgs2",
        text: "$Dg8AAYABBHRoZW0=",
        expected: "Now there are 64 of them!",
    },
    TextTestCase {
        name: "DetokenizeText_TwoMessages",
        text: "What the $qqqqqvwB, $Dg8AAQQEdGhlbQ==",
        expected: "What the ~!, Now there are 2 of them!",
    },
];

fn run_perf_session(detokenizer: &Detokenizer, mode_label: &str, session_num: u32) -> bool {
    pw_log::info!(
        "=== Starting Rust Detokenizer Perf Test Session #{} [{}] ({} iterations/case) ===",
        session_num as u32,
        mode_label as &str,
        ITERATIONS as u32
    );

    let mut all_passed = true;
    let session_start = Instant::now();

    for case in BINARY_CASES {
        let start = Instant::now();
        let mut last_result = String::new();
        for _ in 0..ITERATIONS {
            last_result = black_box(detokenizer.detokenize(black_box(case.data)).best_string());
        }
        let elapsed_ns = u64::try_from(start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        let elapsed_us = u32::try_from(elapsed_ns / 1000).unwrap_or(u32::MAX);
        if last_result != case.expected {
            pw_log::error!(
                "FAIL [{}] {}: expected '{}', got '{}'",
                mode_label as &str,
                case.name as &str,
                case.expected as &str,
                last_result.as_str() as &str
            );
            all_passed = false;
        }
        let ns_per_iter = u32::try_from(elapsed_ns / u64::from(ITERATIONS)).unwrap_or(u32::MAX);
        pw_log::info!(
            "[PERF][{}] {}: {} iters in {} us ({} ns/iter)",
            mode_label as &str,
            case.name as &str,
            ITERATIONS as u32,
            elapsed_us as u32,
            ns_per_iter as u32
        );
    }

    for case in TEXT_CASES {
        let start = Instant::now();
        let mut last_result = String::new();
        for _ in 0..ITERATIONS {
            last_result = black_box(detokenizer.detokenize_text(black_box(case.text)));
        }
        let elapsed_ns = u64::try_from(start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        let elapsed_us = u32::try_from(elapsed_ns / 1000).unwrap_or(u32::MAX);
        if last_result != case.expected {
            pw_log::error!(
                "FAIL [{}] {}: expected '{}', got '{}'",
                mode_label as &str,
                case.name as &str,
                case.expected as &str,
                last_result.as_str() as &str
            );
            all_passed = false;
        }
        let ns_per_iter = u32::try_from(elapsed_ns / u64::from(ITERATIONS)).unwrap_or(u32::MAX);
        pw_log::info!(
            "[PERF][{}] {}: {} iters in {} us ({} ns/iter)",
            mode_label as &str,
            case.name as &str,
            ITERATIONS as u32,
            elapsed_us as u32,
            ns_per_iter as u32
        );
    }

    let session_elapsed_us = u32::try_from(session_start.elapsed().as_micros()).unwrap_or(u32::MAX);
    let total_cases = u32::try_from(BINARY_CASES.len() + TEXT_CASES.len()).unwrap_or(u32::MAX);
    let total_detokenizations = total_cases.saturating_mul(ITERATIONS);
    pw_log::info!(
        "=== Session #{} [{}] Complete: {} cases ({} total detokenizations) in {} us ===",
        session_num as u32,
        mode_label as &str,
        total_cases as u32,
        total_detokenizations as u32,
        session_elapsed_us as u32
    );

    all_passed
}

fn main() {
    pw_log::info!("Initializing Rust detokenizers from binary database...");

    let init_on_demand_start = Instant::now();
    let detokenizer_on_demand = match Detokenizer::from_binary(DATA_WITH_ARGUMENTS) {
        Ok(d) => d,
        Err(_) => {
            pw_log::error!("Failed to initialize On-Demand Detokenizer!");
            std::process::exit(1);
        }
    };
    let init_on_demand_us =
        u32::try_from(init_on_demand_start.elapsed().as_micros()).unwrap_or(u32::MAX);

    let init_preparsed_start = Instant::now();
    let detokenizer_preparsed = match Detokenizer::from_binary_preparsed(DATA_WITH_ARGUMENTS) {
        Ok(d) => d,
        Err(_) => {
            pw_log::error!("Failed to initialize Pre-Parsed Detokenizer!");
            std::process::exit(1);
        }
    };
    let init_preparsed_us =
        u32::try_from(init_preparsed_start.elapsed().as_micros()).unwrap_or(u32::MAX);

    pw_log::info!(
        "Detokenizers initialized: On-Demand in {} us, Pre-Parsed in {} us",
        init_on_demand_us as u32,
        init_preparsed_us as u32
    );

    let passed_on_demand = run_perf_session(&detokenizer_on_demand, "On-Demand", 1);
    let passed_preparsed = run_perf_session(&detokenizer_preparsed, "Pre-Parsed", 1);
    if !passed_on_demand || !passed_preparsed {
        std::process::exit(1);
    }
}
