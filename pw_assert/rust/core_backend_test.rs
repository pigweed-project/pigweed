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

#[cfg(test)]
mod tests {
    use pw_assert::{assert, eq, ne, panic};

    #[test]
    #[should_panic(expected = "custom panic message with arg 12")]
    fn panic_works() {
        panic!("custom panic message with arg {}", 12 as u32,);
    }

    #[test]
    #[should_panic(expected = "custom assert message with arg 34")]
    fn assert_works() {
        assert!(
            false as bool,
            "custom assert message with arg {}",
            34 as u32
        );
    }

    #[test]
    #[should_panic(
        expected = "assertion `left == right` failed: custom eq message with arg 56\n  left: 1\n right: 2"
    )]
    fn eq_works() {
        eq!(
            1 as u32,
            2 as u32,
            "custom eq message with arg {}",
            56 as u32,
        );
    }

    #[test]
    #[should_panic(
        expected = "assertion `left != right` failed: custom ne message with arg 78\n  left: 1\n right: 1"
    )]
    fn ne_works() {
        ne!(
            1 as u32,
            1 as u32,
            "custom ne message with arg {}",
            78 as u32,
        );
    }
}
