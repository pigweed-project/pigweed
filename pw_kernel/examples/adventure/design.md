# Design Document: `pw_kernel` Adventure Example

This document describes the design for the "Adventure" end-to-end example in `pw_kernel`.

## Goal

To illustrate how to build a firmware using the Pigweed kernel and highlight the current feature set:
- Userspace processes
- Userspace drivers
- Userspace process control
- IPC (Inter-Process Communication)
- Wait on multiple objects (WaitGroups)

## Overview

The example consists of a standalone Bazel repository containing a supervisor process, a UART driver process, and a simple text-mode adventure game. It demonstrates how these components interact using Pigweed kernel features.

The repository setup will be based on the [OpenPRoT](https://github.com/OpenPRoT/openprot) repository as an example of how to depend on Pigweed and the Pigweed kernel.

---

## Project Structure

The demo will be its own standalone Bazel repository. The directory structure within the repository will be as follows:

```text
//pw_kernel/examples/adventure/
├── MODULE.bazel       # Standalone repo definition
├── BUILD.bazel        # Top-level build file
├── apps/              # Application processes
│   ├── supervisor/
│   │   ├── BUILD.bazel
│   │   └── main.rs    # Supervisor process
│   ├── uart_driver_rp2350/
│   │   ├── BUILD.bazel
│   │   └── main.rs    # UART driver for RP2350
│   ├── uart_driver_16550/
│   │   ├── BUILD.bazel
│   │   └── main.rs    # UART driver for 16550 (QEMU)
│   └── adventure_game/
│       ├── BUILD.bazel
│       └── main.rs    # Adventure game process
├── protocol/          # Common IPC protocol definitions
│   ├── BUILD.bazel
│   └── lib.rs         # Shared message types
└── targets/           # Target specific configurations
    ├── rp2350/
    │   ├── BUILD.bazel
    │   ├── system.json5
    │   └── target.rs
    └── riscv_qemu/
        ├── BUILD.bazel
        ├── system.json5
        └── target.rs
```

---

## Component Details

### Supervisor Process

The Supervisor process monitors the UART driver and the Adventure Game processes and restarts them if they terminate.

- **Implementation**:
  - Handles to other proceses are provided through the system manifest.
  - It uses a `WaitGroup` to wait for `Signals::JOINABLE` on both process handles simultaneously.
  - Upon receiving a signal, it calls `syscall::process_join` to retrieve the exit status and clean up resources.
  - It then uses `syscall::process_start` to restart the terminated process.

### UART Driver Process (Target-Specific)

To accommodate the different UART hardware on RP2350 and RISC-V QEMU, there will be target-specific driver implementations. This highlights the modularity of the design, as the Supervisor and Adventure Game remain unchanged.

- **Implementations**:
  - `uart_driver_rp2350`: Driver for the RP2350 UART.
  - `uart_driver_16550`: Driver for the 16550 UART used in QEMU.
- **Common Implementation Details**:
  - **Configuration**: The UART base address will be passed into the driver process through a system manifest constant.
  - **Event Loop**: It will use a `WaitGroup` to listen for both hardware interrupts (from the interrupt object) and IPC requests (from the IPC channel) at the same time.
  - It will implement the handler side of the IPC channel defined in `protocol/`.

### Text Mode Adventure Game

A simple game that interacts with the user via the UART service.

- **Implementation**:
  - It acts as the initiator in the IPC channel with the UART driver.
  - It sends requests to read/write data.
  - It supports commands:
    - Canned response.
    - Clean exit.
    - Crash (null pointer dereference).

### Protocol Library

A shared library used by both the UART driver and the Adventure Game to ensure consistent message layout.

- **Implementation**:
  - Defines the message structures using `#[derive(zerocopy::AsBytes, zerocopy::TryFromBytes)]`.
  - Defines the stream IPC protocol using `pw_kernel` IPC channel semantics:
    - **Request Message**: A fixed-size header followed by optional data.
      ```rust
      #[repr(u32)]
      #[derive(Copy, Clone, Debug, zerocopy::AsBytes, zerocopy::TryFromBytes)]
      pub enum RequestType {
          Read = 1,
          Write = 2,
      }

      #[repr(C)]
      #[derive(zerocopy::AsBytes, zerocopy::TryFromBytes)]
      pub struct RequestHeader {
          pub req_type: RequestType,
          pub length: u32,
      }
      ```
    - **Response Message**: A fixed-size header followed by optional data.
      ```rust
      #[repr(C)]
      #[derive(zerocopy::AsBytes, zerocopy::FromBytes)]
      pub struct ResponseHeader {
          pub status: i32,   // 0 for OK, negative for pw_status::Error
          pub length: u32,   // Bytes read or written
      }

      impl From<ResponseHeader> for pw_status::Result<u32> {
          fn from(header: ResponseHeader) -> Self {
              if header.status >= 0 {
                  Ok(header.length)
              } else {
                  let err_code = (-header.status) as u32;
                  // Convert the error code to pw_status::Error. Assumes a TryFrom
                  // implementation exists or is added for pw_status::Error.
                  // Falls back to Internal if the code is invalid.
                  Err(pw_status::Error::try_from(err_code).unwrap_or(pw_status::Error::Internal))
              }
          }
      }
      ```
    - **Stream Semantics**:
      - **Read**: Initiator waits for `Signals::USER` on the channel handle before sending a read request. The handler raises this signal using `syscall::object_set_peer_user_signal` when data is available. Upon seeing the signal, the initiator sends a `RequestHeader` with `req_type = RequestType::Read`. The handler replies immediately with the available data. If no data is available, the handler replies immediately with `length = 0` instead of blocking.
      - **Write**: Initiator sends `RequestHeader` with `req_type = RequestType::Write` followed by the data. Handler writes the data and responds with `ResponseHeader` indicating bytes written.

---

## Target Specific Setup

### RP2350
- Pin configuration is handled by the target main (in `targets/rp2350/target.rs` or similar).
- Default configuration uses UART0 and the lowest numbered pins.

### RISCV QEMU
- Uses the default UART emulation provided by the QEMU target.

---

## Verification Plan

### Automated Tests
- A test target will be created to run the demo in QEMU (RISC-V) and verify that the processes start and the Supervisor restarts them on crash/exit.

### Manual Verification
- Deploy to RP2350 hardware and interact with the adventure game via a serial terminal.
- Trigger the exit and crash commands and verify that the Supervisor restarts the game.
