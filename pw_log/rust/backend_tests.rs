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
#[cfg(test)]
// Untyped prints code rely on as casts to annotate type information.
#[allow(clippy::unnecessary_cast)]
mod tests {
    use pw_log_backend::{pw_log_backend, pw_logf_backend};
    use pw_log_backend_api::LogLevel;

    // Run with capture that checks no panics occurred in the result.
    fn run_with_capture<F: FnOnce()>(action: F) -> String {
        let (output, result) = crate::run_with_capture(action);
        result.unwrap();
        output
    }

    #[test]
    fn no_argument_log_line_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "test")),
            "[INF] test\n"
        );
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test")),
            "[INF] test\n"
        );
    }

    #[test]
    fn integer_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %d", -1)),
            "[INF] test -1\n",
        );
    }

    #[test]
    fn unsigned_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %u", 1u32)),
            "[INF] test 1\n",
        );
    }

    #[test]
    fn string_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %s", "test")),
            "[INF] test test\n",
        );
    }
    #[test]
    fn character_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %c", 'c')),
            "[INF] test c\n",
        );
    }

    #[test]
    fn untyped_i32_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "test {}", -1 as i32)),
            "[INF] test -1\n",
        );

        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %v", -1 as i32)),
            "[INF] test -1\n",
        );
    }
    #[test]
    fn untyped_u32_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "test {}", 1 as u32)),
            "[INF] test 1\n",
        );

        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %v", 1 as u32)),
            "[INF] test 1\n",
        );
    }

    #[test]
    fn untyped_str_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "test {}", "Pigweed" as &str)),
            "[INF] test Pigweed\n",
        );

        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "test %v", "Pigweed" as &str)),
            "[INF] test Pigweed\n",
        );
    }

    #[test]
    fn untyped_hex_integer_argument_prints_to_stdout() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "{:x}", 0xdecafbad as u32)),
            "[INF] decafbad\n",
        );
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "{:X}!", 0xdecafbad as u32)),
            "[INF] DECAFBAD!\n",
        );
    }

    #[test]
    fn typed_min_fields_width_and_zero_padding_formats_correctly() {
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "%8x", 0xcafe as u32)),
            "[INF]     cafe\n",
        );
        assert_eq!(
            run_with_capture(|| pw_logf_backend!(LogLevel::Info, "%08X!", 0xcafe as u32)),
            "[INF] 0000CAFE!\n",
        );
    }

    #[test]
    fn untyped_min_fields_width_and_zero_padding_formats_correctly() {
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "{:8x}", 0xcafe as u32)),
            "[INF]     cafe\n",
        );
        assert_eq!(
            run_with_capture(|| pw_log_backend!(LogLevel::Info, "{:08X}!", 0xcafe as u32)),
            "[INF] 0000CAFE!\n",
        );
    }
}
