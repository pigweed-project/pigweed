.. _module-pw_kernel-guides:

======
Guides
======
.. pigweed-module-subpage::
   :name: pw_kernel

This guide covers real-world usage topics such as unit testing and panic
detecting.

.. _module-pw_kernel-guides-unit-testing:

------------
Unit testing
------------
``pw_kernel`` includes a lightweight unit testing framework designed for both
bare-metal and kernel-aware tests.

Writing a test
==============
Tests are written as standard Rust functions annotated with the ``#[test]``
attribute from the ``unittest`` crate. The test function should return a
``unittest::Result<()>``.

Bare-metal test example:

.. code-block:: rust

   // in your_module/lib.rs or your_module/tests.rs
   #[cfg(test)]
   mod tests {
       use unittest::test;

       fn add(a: u32, b: u32) -> u32 {
           a + b
       }

       #[test]
       fn test_addition() -> unittest::Result<()> {
           unittest::assert_eq!(add(2, 2), 4);
           unittest::assert_ne!(add(2, 2), 5);
           unittest::assert_true!(add(1,1) == 2);
           Ok(())
       }
   }

Assertions
==========
The ``unittest`` crate provides several assertion macros:

- ``unittest::assert_eq!(a, b)``: Asserts that ``a`` is equal to ``b``.
- ``unittest::assert_ne!(a, b)``: Asserts that ``a`` is not equal to ``b``.
- ``unittest::assert_true!(expr)``: Asserts that ``expr`` evaluates to true.
- ``unittest::assert_false!(expr)``: Asserts that ``expr`` evaluates to false.

Kernel-aware tests
==================
For tests that require kernel services (e.g., testing scheduler behavior, mutexes,
or timers), you can mark them as needing the kernel by passing the
``needs_kernel`` argument to the ``#[test]`` attribute:

.. code-block:: rust

   use kernel::sync::mutex::Mutex;
   use unittest::test;

   static MY_MUTEX: Mutex<u32> = Mutex::new(0);

   #[test(needs_kernel)]
   fn test_mutex_locking() -> unittest::Result<()> {
       let guard = MY_MUTEX.lock();
       unittest::assert_eq!(*guard, 0);
       // guard is dropped here, unlocking the mutex
       Ok(())
   }

.. _module-pw_kernel-guides-panic-detector:

--------------
Panic detector
--------------
``panic_detector`` scans a Rust binary and uses static analysis to display
a list of all panic call sites.

Currently only ``rv32`` ELF files are supported.

Setup:

1. Add a panic handler to the Rust binary which will be used by
   ``panic_detector`` to find the panic call sites.

   .. code-block:: rust

      #[panic_handler]
      fn panic_handler(info: &core::panic::PanicInfo) -> ! {
          if let Some(location) = info.location() {
              panic_is_possible(location.file().as_ptr(), location.file().len(), location.line(), location.column());
          } else {
              panic_is_possible(core::ptr::null(), 0, 0, 0);
          }
      }

      #[unsafe(no_mangle)]
      #[inline(never)]
      extern "C" fn panic_is_possible(filename: *const u8, filename_len: usize, line: u32, col: u32) -> !{
          // The arguments to this function are reverse-engineereddocs.html
          // from the machine code by static analysis to
          // display a list of all the panic call-sites to the
          // user in the mutask_no_panic_test() error message.
          // See welder/src/check_panic.rs for more details.

          // If this symbol exists in the binary, panics are
          // possible. Presubmit tests can ensure that this symbol
          // does not exist in the final binary.  Do not rename or
          // remove this function.
          core::hint::black_box(filename);
          core::hint::black_box(filename_len);
          core::hint::black_box(line);
          core::hint::black_box(col);
          loop {}
      }

2. Add a test target to Bazel for the binary to validate.

   .. code-block:: bazel

      rust_binary(
          name = "example",
          srcs = ["main.rs"],
      )

      load("//pw_kernel/tooling/panic_detector:rust_binary_no_panics_test.bzl", "rust_binary_no_panics_test")

      rust_binary_no_panics_test(
          name = "example_no_panic_test",
          binary = ":example",
      )

3. Run the bazel test. If any panics are detected , ``panic_detector``
   prints the locations to ``stdout``.

   .. code-block:: console

      $ bazel test //:example_no_panic_test
      …
      Found panic ufmt-0.2.0/src/helpers.rs line 58 column 13. Branch trace:
        00103896 lui      a0,0x104           (task0_entry)
        0010389e jal      ra,-1734           (task0_entry)
        0010320a jalr     ra,ra,-82          (panic_const_add_overflow)
        001031d2 jalr     ra,ra,-78          (core::panicking::panic_fmt)
        00103192 jal      ra,2               (rust_begin_unwind)
        00103194 addi     sp,sp,-16          (panic_is_possible)
        …
