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
#![no_std]

pub fn add(left: u64, right: u64) -> u64 {
    left + right
}

#[cfg(test)]
mod tests {
    use pw_unit_test::{assert, assert_eq, assert_ne, test};

    use super::*;

    #[test]
    fn test_add() {
        let result = add(2, 2);
        assert_eq!(result, 4);
    }

    #[test]
    fn rust_assert_ne() {
        assert_ne!(add(1, 1), 3);
    }

    #[test]
    fn rust_assert() {
        assert!(add(5, 5) == 10, "5 + 5 should equal 10");
    }
}
