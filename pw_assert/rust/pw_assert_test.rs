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
    use pw_assert::{assert, debug_assert, debug_eq, debug_ne, eq, ne};

    // Because infrastructure to verify panics does not exist, these tests only
    // check for the valid condition and the syntax of the macros being correct.

    #[test]
    fn assert_syntax_works() {
        assert!(true as bool);
        assert!(true as bool,);

        assert!(true as bool, "custom msg");
        assert!(true as bool, "custom msg",);

        assert!(true as bool, "custom msg with arg {}", 42 as u32);
        assert!(true as bool, "custom msg with arg {}", 42 as u32,);
    }

    #[test]
    fn debug_assert_syntax_works() {
        debug_assert!(true as bool);
        debug_assert!(true as bool,);

        debug_assert!(true as bool, "custom msg");
        debug_assert!(true as bool, "custom msg",);

        debug_assert!(true as bool, "custom msg with arg {}", 42 as u32);
        debug_assert!(true as bool, "custom msg with arg {}", 42 as u32,);
    }

    #[test]
    fn assert_eq_syntax_works() {
        eq!(1 as u32, 1 as u32);
        eq!(1 as u32, 1 as u32,);

        eq!(1 as u32, 1 as u32, "custom msg");
        eq!(1 as u32, 1 as u32, "custom msg",);

        eq!(1 as u32, 1 as u32, "custom msg with arg {}", 42 as u32);
        eq!(1 as u32, 1 as u32, "custom msg with arg {}", 42 as u32,);
    }

    #[test]
    fn assert_ne_syntax_works() {
        ne!(1 as u32, 2 as u32);
        ne!(1 as u32, 2 as u32,);

        ne!(1 as u32, 2 as u32, "custom msg");
        ne!(1 as u32, 2 as u32, "custom msg",);

        ne!(1 as u32, 2 as u32, "custom msg with arg {}", 42 as u32);
        ne!(1 as u32, 2 as u32, "custom msg with arg {}", 42 as u32,);
    }

    #[test]
    fn debug_eq_syntax_works() {
        debug_eq!(1 as u32, 1 as u32);
        debug_eq!(1 as u32, 1 as u32,);

        debug_eq!(1 as u32, 1 as u32, "custom msg");
        debug_eq!(1 as u32, 1 as u32, "custom msg",);

        debug_eq!(1 as u32, 1 as u32, "custom msg with arg {}", 42 as u32);
        debug_eq!(1 as u32, 1 as u32, "custom msg with arg {}", 42 as u32,);
    }

    #[test]
    fn debug_ne_syntax_works() {
        debug_ne!(1 as u32, 2 as u32);
        debug_ne!(1 as u32, 2 as u32,);

        debug_ne!(1 as u32, 2 as u32, "custom msg");
        debug_ne!(1 as u32, 2 as u32, "custom msg",);

        debug_ne!(1 as u32, 2 as u32, "custom msg with arg {}", 42 as u32);
        debug_ne!(1 as u32, 2 as u32, "custom msg with arg {}", 42 as u32,);
    }
}
