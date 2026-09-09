---
name: Pigweed Kernel Testing
description: Instructions for running tests and lints for the Pigweed kernel (`pw_kernel`).
---

# Pigweed Kernel Testing

This skill provides instructions for running tests and lints for the Pigweed kernel (`pw_kernel`).

## Building and Testing

- **Use `bazelisk`**: Always use `bazelisk` instead of `bazel`.
- **Use `--config`**: All kernel code must be built/tested with a `--config` argument. Configs are defined in `//pw_kernel/kernel.bazelrc` and are prefixed with `k_`.
- **Iterate on one config**: Switching `--config` can be expensive due to rebuilds. Prefer iterating and fixing issues on a single config before switching to another.

### Common Configurations

Configurations are defined in `//workflows.json` under the `kernel` group (via `kernel_host` and `kernel_device` subgroups). Common ones include:

- `k_host`: Host builds and tests.
- `k_qemu_mps2_an505`: ARM QEMU emulation.
- `k_qemu_virt_riscv32`: RISC-V QEMU emulation.
- `k_rp2350`: Raspberry Pi RP2350 (note: may have `no_test` set).
- `k_doctest`: Rust doc tests (takes a long time to execute).

### Running Tests

To run tests for a specific configuration:

```bash
bazelisk test --config=k_host //pw_kernel/...
```

To run a specific test:

```bash
bazelisk test --config=k_host //pw_kernel/kernel/tests:scheduler_test
```

> [!NOTE]
> Use `--noshow_progress --noshow_loading_progress` to reduce output in agent contexts if needed.

> [!TIP]
> Use `--test_timeout=10 --test_output=all` to catch tests which do not terminate.

## Linting and Formatting

### Rust Linting

The kernel build steps named like `k_*_lint` should be run to provide Rust linting for the code. These are defined in `workflows.json`.

Example for linting on host:
```bash
bazelisk build --config=k_lint --config=k_host //pw_kernel/...
```

### Project-wide Formatting and Presubmit

Run these from the repository root:

- **Format**: `./pw format`
- **Presubmit**: `./pw presubmit`

## When to use this skill

Use this skill when you need to run tests, lints, or verify changes in the `pw_kernel` module. This ensures you use the correct configurations and tools as defined by the project.
