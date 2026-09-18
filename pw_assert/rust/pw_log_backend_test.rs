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

#[unsafe(no_mangle)]
#[allow(non_snake_case)]
pub extern "C-unwind" fn pw_assert_HandleFailure() -> ! {
    core::panic!("pw_assert panic")
}

#[cfg(test)]
mod tests {
    use printf_backend_test::run_with_capture;
    use pw_assert::{assert, eq, ne, panic};
    use pw_assert_backend::__private_log_panic_banner;

    fn panic_banner() -> String {
        let (output, result) = run_with_capture(|| {
            __private_log_panic_banner!();
        });
        result.unwrap();
        output
    }

    #[test]
    fn panic_works() {
        let banner = panic_banner();
        let (output, result) = run_with_capture(|| {
            panic!("custom panic message msg with arg {}", 12 as u32);
        });
        assert_eq!(
            output,
            format!("{banner}[FTL] custom panic message msg with arg 12\n"),
        );
        assert_eq!(
            result.unwrap_err().downcast_ref::<&str>(),
            Some(&"pw_assert panic"),
        );
    }

    #[test]
    fn assert_works() {
        let banner = panic_banner();
        let line = line!() + 2;
        let (output, result) = run_with_capture(|| {
            assert!(
                false as bool,
                "custom assert message with arg {}",
                34 as u32
            );
        });
        assert_eq!(
            output,
            format!(
                "{banner}[FTL] assertion failed: false as bool @ {}:{line}\n[FTL] custom assert message with arg 34\n",
                file!()
            ),
        );
        assert_eq!(
            result.unwrap_err().downcast_ref::<&str>(),
            Some(&"pw_assert panic"),
        );
    }

    #[test]
    fn eq_works() {
        let banner = panic_banner();
        let line = line!() + 2;
        let (output, result) = run_with_capture(|| {
            eq!(
                1 as u32,
                2 as u32,
                "custom eq message with arg {}",
                56 as u32,
            );
        });
        assert_eq!(
            output,
            format!(
                "{banner}[FTL] assertion failed: 1 == 2 @ {}:{line}\n[FTL] custom eq message with arg 56\n",
                file!()
            ),
        );
        assert_eq!(
            result.unwrap_err().downcast_ref::<&str>(),
            Some(&"pw_assert panic"),
        );
    }

    #[test]
    fn ne_works() {
        let banner = panic_banner();
        let line = line!() + 2;
        let (output, result) = run_with_capture(|| {
            ne!(
                1 as u32,
                1 as u32,
                "custom ne message with arg {}",
                78 as u32,
            );
        });
        assert_eq!(
            output,
            format!(
                "{banner}[FTL] assertion failed: 1 != 1 @ {}:{line}\n[FTL] custom ne message with arg 78\n",
                file!()
            ),
        );
        assert_eq!(
            result.unwrap_err().downcast_ref::<&str>(),
            Some(&"pw_assert panic"),
        );
    }
}
