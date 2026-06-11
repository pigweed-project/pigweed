// Copyright 2023 The Pigweed Authors
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

//! # Infrastructure for building calls to `printf`.
//!
//! The `varargs` modules solves a particularly tricky problem: Some arguments
//! passed to `pw_log` result in a single argument passed to printf (a `u32`
//! is passed directly) while some may result in multiple arguments (a `&str`
//! is passed as a length argument and a data argument).  Since function like
//! proc macros don't know the types of the arguments we rely on chained generic
//! types.
//!
//! ## VarArgs trait
//! The [`VarArgs`] both encapsulates the structure of the arguments as well
//! as provides a method for calling `printf`.  It accomplishes this through a
//! recursive, chained/wrapped type through it's `OneMore<T>` associated type.
//! We then provide generic implementations for [`VarArgs`] for tuples
//! ov values that implement [`Clone`] such that:
//!
//! ```
//! # use pw_log_backend_printf::varargs::VarArgs;
//! # use core::any::TypeId;
//! # use core::ffi::{c_int, c_uchar};
//! type VarArgsType =
//!   <<<<() as VarArgs>
//!       ::OneMore<u32> as VarArgs>
//!       ::OneMore<i32> as VarArgs>
//!       ::OneMore<c_int> as VarArgs>
//!       ::OneMore<*const c_uchar>;
//! type TupleType = (u32, i32, c_int, *const c_uchar);
//! assert_eq!(TypeId::of::<VarArgsType>(), TypeId::of::<TupleType>());
//! ```
//!
//! [`VarArgs`] provides an `append()` method that allows building up these
//! typed tuple values:
//!
//! ```
//! # use pw_log_backend_printf::varargs::VarArgs;
//! # use core::ffi::{c_int, c_uchar};
//! let string = "test";
//! let args = ()
//!   .append(0u32)
//!   .append(-1i32)
//!   .append(string.len() as c_int)
//!   .append(string.as_ptr().cast::<*const c_uchar>());
//! assert_eq!(args, (
//!   0u32,
//!  -1i32,
//!  string.len() as c_int,
//!  string.as_ptr().cast::<*const c_uchar>()));
//! ```
//!
//! Lastly [`VarArgs`] exposes an unsafe `call_printf()` method that calls
//! printf and passes the build up arguments to `printf()`:
//!
//! ```
//! # use pw_log_backend_printf::varargs::VarArgs;
//! # use core::ffi::{c_int, c_uchar};
//! let string = "test";
//! unsafe {
//!   // This generates a call to:
//!   // `printf("[%s] %u %d %.*s\0".as_ptr(), "INF".as_ptr(), 0u32, -1i32, 4, string.as_ptr())`.
//!   ()
//!     .append(0u32)
//!     .append(-1i32)
//!     .append(string.len() as c_int)
//!     .append(string.as_ptr().cast::<*const c_uchar>())
//!     // Note that we always pass the null terminated log level string here
//!     // as an argument to `call_printf()`.
//!     .call_printf("[%s] %u %d %.*s\0".as_ptr().cast(), "INF\0".as_ptr().cast());
//! }
//! ```
//!
//! ## Arguments Trait
//! The [`Arguments`] trait is the final piece of the puzzle.  It is used to
//! generate one or more calls to [`VarArgs::append()`] for each argument to
//! `pw_log`.  Most simple cases that push a single argument look like:
//!
//! ```ignore
//! # // Ignored as a test as the test is neither the crate that defines
//! # // `Arguments` nor `i32`.
//! impl Arguments<i32> for i32 {
//!   type PushArg<Head: VarArgs> = Head::OneMore<i32>;
//!   fn push_arg<Head: VarArgs>(head: Head, arg: &i32) -> Self::PushArg<Head> {
//!     head.append(*arg)
//!   }
//! }
//! ```
//!
//! A more complex case like `&str` can push multiple arguments:
//! ```ignore
//! # // Ignored as a test as the test is neither the crate that defines
//! # // `Arguments` nor `&str`.
//! impl Arguments<&str> for &str {
//!   // Arguments are a chain of two `OneMore` types.  One for the string length
//!   // and one for the pointer to the string data.
//!   type PushArg<Head: VarArgs> =
//!     <Head::OneMore<c_int> as VarArgs>::OneMore<*const c_uchar>;
//!
//!   // `push_arg` pushes both the length and pointer to the string into the args tuple.
//!   fn push_arg<Head: VarArgs>(head: Head, arg: &&str) -> Self::PushArg<Head> {
//!     let arg = *arg;
//!     head.append(arg.len() as c_int).append(arg.as_ptr().cast::<*const c_uchar>())
//!   }
//! }
//! ```
//!
//! ## Putting it all together
//! With all of these building blocks, the backend proc macro emits the following
//! code:
//!
//! ```
//! # use pw_log_backend_printf::varargs::{Arguments, VarArgs};
//! // Code emitted for a call to `info!("Hello {}.  It is {}:00", "Pigweed" as &str, 2 as u32)
//! let args = ();
//! let args = <&str as Arguments<&str>>::push_arg(args, &("Pigweed" as &str));
//! let args = <u32 as Arguments<u32>>::push_arg(args, &(2 as u32));
//! unsafe {
//!   args.call_printf("[%s] Hello %.*s.  It is %d:00".as_ptr().cast(), "INF\0".as_ptr().cast());
//! }
//! ```
use core::convert::Infallible;
use core::ffi::{c_int, c_uchar};

/// Implements a list of arguments to a vararg call.
///
/// See [module level docs](crate::varargs) for a detailed description on how
/// [`Arguments`] works and is used.
pub trait Arguments<T: ?Sized> {
    /// Type produced by calling [`Self::push_arg()`].
    type PushArg<Head: VarArgs>: VarArgs;

    /// Push an argument onto the list of varargs.
    ///
    /// This may actually push zero, one, or more arguments onto the list
    /// depending on implementation.
    fn push_arg<Head: VarArgs>(head: Head, arg: &T) -> Self::PushArg<Head>;
}

/// Represents a variable length list of arguments to printf.
///
/// See [module level docs](crate::varargs) for a detailed description on how
/// how [`VarArgs`] works and is used.
pub trait VarArgs: Clone {
    /// The type that is produced by a call to `append()`
    type OneMore<T: Clone>: VarArgs;

    /// Used to check if there is space left in the argument list.
    ///
    /// If the there is no space left in the argument list an [`Arguments<T>`]'s
    /// PushArg type will expand to a type where `CHECK` us unable to be
    /// compiled.
    const CHECK: () = ();

    /// Append an additional argument to this argument list.
    fn append<T: Clone>(self, val: T) -> Self::OneMore<T>;

    /// Calls `printf` with the arguments in `self` and the given format and log level string.
    ///
    /// # Safety
    ///
    /// Calls into `libc` printf without any input validation.  The code further
    /// up the stack is responsible for initializing valid [`VarArgs`] that
    /// will cause printf to execute in a sound manner.
    unsafe fn call_printf(self, format_str: *const c_uchar, log_level_str: *const c_uchar)
        -> c_int;
}

#[derive(Clone)]
/// A sentinel type for trying to append too many (>12) arguments to a
/// [`VarArgs`] tuple.
pub struct TooMany(Infallible);
impl TooMany {
    const fn panic() -> ! {
        panic!("Too many arguments to logging call")
    }
}

#[doc(hidden)]
/// Implementation VarArgs for TooMany.  Usages of TooMany::CHECK will cause a
/// compile-time error.
impl VarArgs for TooMany {
    type OneMore<T: Clone> = TooMany;
    const CHECK: () = Self::panic();

    fn append<T: Clone>(self, _: T) -> Self::OneMore<T> {
        Self::panic()
    }

    unsafe fn call_printf(self, _: *const c_uchar, _: *const c_uchar) -> c_int {
        Self::panic()
    }
}

/// Used to implement [`VarArgs`] on tuples.
///
/// This recursive macro divides it's arguments into a set of arguments for use
/// in the recursive case in `[]`s and arguments used for the current
/// implementation of [`VarArgs`] following the `[]`'s.
macro_rules! impl_args_list {
      // Entry point into the macro that directly invokes `@impl` with its
      // arguments.
      //
      // Take a list of arguments of the form `ident => position`.  `ident`
      // is used to name the generic type argument for the implementation
      // of `[VarArgs]` (i.e. `impl<ident: Clone, ...> VarArgs for (ident, ...)`).
      // `position` is used to index into the argument tuple when calling
      // printf (i.e. `printf(format_str, ..., args.position, ...)`).
      ($($arg:ident => $arg_ident:tt),* $(,)?) => {
          impl_args_list!(@impl [$($arg => $arg_ident),*]);
      };

      // Recursive case for [`VarArgs`] implementation.
      //
      // Implements [`VarArgs`] for a tuple with length equal to the number
      // of arguments listed after the `[]`.  It then recurses with taking
      // the first argument between the `[]`s and appending it to the list
      // after the `[]`s.
      (@impl [$next:ident => $next_num:tt
              $(, $remaining:ident => $next_remaining:tt)*]
              $($current:ident => $current_num:tt),*) => {
          impl<$($current: Clone),*> VarArgs for ($($current,)*) {
              type OneMore<$next: Clone> = ($($current,)* $next,);
              fn append<$next>(self, val: $next) -> ($($current,)* $next,) {
                  ($(self. $current_num,)* val,)
              }

              unsafe fn call_printf(
                  self,
                  format_str: *const c_uchar,
                  log_level_str: *const c_uchar,
              ) -> c_int {
                  unsafe extern "C" {
                    fn printf(fmt: *const c_uchar, ...) -> c_int;
                  }
                  unsafe { printf(format_str, log_level_str, $(self. $current_num),*) }
              }
          }

          impl_args_list!(@impl
              [$($remaining => $next_remaining),*]
              $($current => $current_num,)* $next => $next_num);
      };

      // Base for [`VarArgs`] implementation.
      //
      // Implements [`VarArgs`] for the full list of arguments and sets
      // its `OneMore` type to `TooMany` to cause a compilation error
      // if code tries to instantiate an argument list longer that this.
      (@impl [] $($current:ident => $current_num:tt),*) => {
          impl<$($current: Clone),*> VarArgs for ($($current),*) {
              type OneMore<T: Clone> = TooMany;
              fn append<T: Clone>(self, _: T) -> TooMany {
                  panic!("Too many arguments to logging call")
              }

              unsafe fn call_printf(
                  self,
                  format_str: *const c_uchar,
                  log_level_str: *const c_uchar,
              ) -> c_int {
                  unsafe extern "C" {
                    fn printf(fmt: *const c_uchar, ...) -> c_int;
                  }
                  unsafe { printf(format_str, log_level_str, $(self. $current_num),*) }
              }
          }
      };
  }

// Expands to implementations of [`VarArgs`] for tuples of length 0-12.
impl_args_list!(
    ARGS0 => 0,
    ARGS1 => 1,
    ARGS2 => 2,
    ARGS3 => 3,
    ARGS4 => 4,
    ARGS5 => 5,
    ARGS6 => 6,
    ARGS7 => 7,
    ARGS8 => 8,
    ARGS9 => 9,
    ARGS10 => 10,
    ARGS11 => 11,
    ARGS12 => 12,
);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appended_args_yields_correct_tuple() {
        let string = "test";
        let args =
            ().append(0u32)
                .append(-1i32)
                .append(string.len() as c_int)
                .append(string.as_ptr().cast::<*const c_uchar>());

        assert_eq!(
            args,
            (
                0u32,
                -1i32,
                string.len() as c_int,
                string.as_ptr().cast::<*const c_uchar>()
            )
        );
    }

    #[test]
    fn twelve_argument_long_tuples_are_supported() {
        let args =
            ().append(0u32)
                .append(1u32)
                .append(2u32)
                .append(3u32)
                .append(4u32)
                .append(5u32)
                .append(6u32)
                .append(7u32)
                .append(8u32)
                .append(9u32)
                .append(10u32)
                .append(11u32);

        assert_eq!(args, (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11));
    }
}
