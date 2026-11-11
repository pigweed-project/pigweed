# Pigweed Zephyr Module Tests

This directory contains tests verifying Pigweed modules integrate and run on
the Zephyr RTOS, executing on the `native_sim` platform.

## Running tests

To run the Bazel test suite directly:

```bash
$ cd zephyr/pw_tests
$ bazelisk test //pw_module_tests
```

Or if you know the target name for a specific test, you can run it directly:

```bash
$ cd zephyr/pw_tests
$ bazelisk test //pw_module_tests:pw_allocator_zephyr_test
```

## Listing tests

The list of tests in the suite is defined in Pigweed.

You can list the individual tests contained in the `pw_module_tests` suite with
a `bazelisk query`:

```bash
$ cd zephyr/pw_tests
$ bazelisk query "tests(//pw_module_tests:pw_module_tests)"
//pw_module_tests:pw_allocator_zephyr_test
...
```

## Configuring the tests in Pigweed

In the Pigweed source tree, the tests to run here are defined in the
`zephyr/pw_module_tests.bzl` source code file.

You update the test configuration there.
