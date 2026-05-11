# Implementation Plan: `pw_kernel` Adventure Example

This document outlines a phased implementation plan for the "Adventure" example in `pw_kernel`, based on the design described in [design.md](file:///Users/konkers/work/worktrees/example/pw_kernel/examples/adventure/design.md). It focuses on incremental development, verification at each step, and pausing for user input between phases.

## Phase 1: Project Infrastructure and Protocol

**Goal**: Set up the repository structure and define the shared IPC protocol.

1.  **Setup Bazel Workspace**:
    *   Create `//pw_kernel/examples/adventure/MODULE.bazel` and `BUILD.bazel` based on the OpenPRoT example.
    *   Add `pw_kernel/examples/adventure` to the root `.bazelignore` file to ensure it is ignored by the main Bazel build.
    *   Verify that the empty repository can be recognized by Bazel.
2.  **Define Protocol**:
    *   Create `//pw_kernel/examples/adventure/protocol/`.
    *   Implement `lib.rs` with `RequestType`, `RequestHeader`, and `ResponseHeader` using `zerocopy`.
    *   Implement the `From` conversion for `ResponseHeader` to `pw_status::Result<u32>`.
3.  **Verification**:
    *   Build the protocol library target.

> [!IMPORTANT]
> **Pause point**: Report success of Phase 1 and request approval to proceed to Phase 2.

---

## Phase 2: Target Configuration and Manifests

**Goal**: Define the target-specific configurations and system manifests.

1.  **Create Target Directories**:
    *   Create `//pw_kernel/examples/adventure/targets/rp2350/` and `//pw_kernel/examples/adventure/targets/riscv_qemu/`.
2.  **System Manifests**:
    *   Create `system.json5` for each target.
    *   Define constants for UART base addresses and interrupt numbers.
    *   Configure initial processes (Supervisor, UART Driver, Game).
3.  **Verification**:
    *   Create stub processes (minimal `main` functions) for Supervisor, UART Driver, and Game to satisfy manifest requirements.
    *   Verify that the system builds successfully with the new manifests and stub processes.
    *   Verify that the system can be executed (e.g., in QEMU) and that the stub processes are loaded correctly, confirming the configuration is valid.

> [!IMPORTANT]
> **Pause point**: Report success of Phase 2 and request approval to proceed to Phase 3.

---

## Phase 3: Mock Driver and Adventure Game

**Goal**: Implement the game logic and a mock UART driver to verify IPC without hardware dependencies.

1.  **Mock UART Driver**:
    *   Implement a simple driver in `//pw_kernel/examples/adventure/apps/uart_driver_mock/` that does not use hardware interrupts but simulates data availability or echoes input.
2.  **Adventure Game**:
    *   Implement the game in `//pw_kernel/examples/adventure/apps/adventure_game/`.
    *   Implement commands: Canned response, Clean exit, Crash.
3.  **Verification**:
    *   Run the game and mock driver in QEMU (RISC-V) and verify that they can communicate via IPC and the game logic works.

> [!IMPORTANT]
> **Pause point**: Report success of Phase 3 and request approval to proceed to Phase 4.

---

## Phase 4: Real UART Drivers

**Goal**: Implement the hardware-specific UART drivers.

1.  **16550 Driver (QEMU)**:
    *   Implement `//pw_kernel/examples/adventure/apps/uart_driver_16550/` using interrupts.
2.  **RP2350 Driver**:
    *   Implement `//pw_kernel/examples/adventure/apps/uart_driver_rp2350/` using interrupts.
3.  **Verification**:
    *   Run in QEMU with the 16550 driver and verify interaction.
    *   (Optional but recommended) Deploy to RP2350 and verify interaction.

> [!IMPORTANT]
> **Pause point**: Report success of Phase 4 and request approval to proceed to Phase 5.

---

## Phase 5: Supervisor Process

**Goal**: Implement the process monitoring and restart logic.

1.  **Supervisor**:
    *   Implement `//pw_kernel/examples/adventure/apps/supervisor/`.
    *   Use `WaitGroup` to monitor processes.
    *   Handle termination and restart.
2.  **Verification**:
    *   Run full system.
    *   Trigger crash and clean exit in game and verify restart by Supervisor.

---

## Final Review

*   Verify all requirements from `kernel_exmaple.md` are met.
*   Complete documentation.
