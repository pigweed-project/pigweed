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
#[deny(unused_variables, unused_assignments)]
mod tests {
    use pw_assert::{assert, debug_assert, debug_eq, debug_ne, debug_panic, eq, ne};

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
        let cond = true;
        let arg = 42u32;
        let arg2 = 99u32;
        debug_assert!(cond as bool);
        debug_assert!(cond as bool,);

        debug_assert!(cond as bool, "custom msg");
        debug_assert!(cond as bool, "custom msg",);

        debug_assert!(cond as bool, "custom msg with arg {}", arg as u32);
        debug_assert!(cond as bool, "custom msg with arg {}", arg as u32,);
        debug_assert!(
            cond as bool,
            "custom msg with args {} {}",
            arg as u32, arg2 as u32,
        );

        if false {
            debug_assert!(cond as bool)
        }
    }

    #[test]
    fn debug_panic_syntax_works() {
        let arg = 42u32;
        let arg2 = 99u32;
        if false {
            debug_panic!("custom msg");
        }
        if false {
            debug_panic!("custom msg",);
        }
        if false {
            debug_panic!("custom msg with arg {}", arg as u32);
        }
        if false {
            debug_panic!("custom msg with arg {}", arg as u32,);
        }
        if false {
            debug_panic!("custom msg with args {} {}", arg as u32, arg2 as u32)
        }
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
        let a = 1u32;
        let b = 1u32;
        let arg = 42u32;
        let arg2 = 99u32;
        debug_eq!(a as u32, b as u32);
        debug_eq!(a as u32, b as u32,);

        debug_eq!(a as u32, b as u32, "custom msg");
        debug_eq!(a as u32, b as u32, "custom msg",);

        debug_eq!(a as u32, b as u32, "custom msg with arg {}", arg as u32);
        debug_eq!(a as u32, b as u32, "custom msg with arg {}", arg as u32,);
        debug_eq!(
            a as u32,
            b as u32,
            "custom msg with args {} {}",
            arg as u32,
            arg2 as u32,
        );

        if false {
            debug_eq!(a as u32, b as u32)
        }
    }

    #[test]
    fn debug_ne_syntax_works() {
        let a = 1u32;
        let b = 2u32;
        let arg = 42u32;
        let arg2 = 99u32;
        debug_ne!(a as u32, b as u32);
        debug_ne!(a as u32, b as u32,);

        debug_ne!(a as u32, b as u32, "custom msg");
        debug_ne!(a as u32, b as u32, "custom msg",);

        debug_ne!(a as u32, b as u32, "custom msg with arg {}", arg as u32);
        debug_ne!(a as u32, b as u32, "custom msg with arg {}", arg as u32,);
        debug_ne!(
            a as u32,
            b as u32,
            "custom msg with args {} {}",
            arg as u32,
            arg2 as u32,
        );

        if false {
            debug_ne!(a as u32, b as u32)
        }
    }
}
