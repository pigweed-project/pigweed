# Bluetooth Virtual Controller

## About

The Bluetooth virtual controller is a Bluetooth controller emulator that allows
system Bluetooth components to be tested in integration against the Bluetooth
HCI protocol.

The virtual controller currently supports the LoopbackDevice and
EmulatorDevice. The `virtual` package ships it as the `meta/bt-hci-virtual.cm`
component, which serves a VirtualController and exposes three FIDL services from
its outgoing directory:

- `fuchsia.hardware.bluetooth.VirtualControllerService`: the single instance
  `default`, published at startup.
- `fuchsia.hardware.bluetooth.EmulatorService`: one instance per live
  emulator, named `emulator-<n>`.
- `fuchsia.hardware.bluetooth.Service`: one instance per published emulator or
  loopback device.

## LoopbackDevice

LoopbackDevice is used by RootCanal, which is used by PTS-bot. The step numbers
below are correlated with the numbers in the diagram.

### Steps

1. `bt-hci-virtual` starts and publishes the `default` instance of
   `fuchsia.hardware.bluetooth.VirtualControllerService`.
2. `bt-rootcanal` connects to `VirtualController` through
   `VirtualControllerService` and calls `CreateLoopbackDevice` with an HCI UART
   channel.
3. `bt-hci-virtual` creates a `LoopbackDevice`, which publishes the `loopback`
   instance of `fuchsia.hardware.bluetooth.Service`.
4. `bt-init` detects the `fuchsia.hardware.bluetooth.Service` instance and
   creates a new `bt-host` component.

```
┌───────────────────────────────┐  2. CreateLoopbackDevice()  ┌──────────────┐
│        bt-hci-virtual         │◄────────────────────────────┤ bt-rootcanal │
│ 1. Publishes                  │                             └──────────────┘
│    VirtualControllerService   │
└───────────────┬───────────────┘
                │
                ▼
┌───────────────────────────────┐
│        LoopbackDevice         │
│ 3. Publishes Service          │
│    instance (loopback)        │
└───────────────┬───────────────┘
                │
                ▼  4. Watched by bt-init
┌───────────────────────────────┐
│            bt-host            │
└───────────────────────────────┘
```

## EmulatorDevice

EmulatorDevice is used for Bluetooth integration tests. The
`fuchsia.hardware.bluetooth.Emulator` API is used to establish and interact
with the EmulatorDevice. The step numbers below are correlated with the numbers
in the diagram. Please note that the term "emulator" is heavily overloaded, so
it's important to be precise when using the term.

### Steps

1. A test realm starts `bt-hci-virtual`, which publishes the `default` instance
   of `fuchsia.hardware.bluetooth.VirtualControllerService`.
2. `hci-emulator-client` connects to `VirtualController` through
   `VirtualControllerService` and calls `CreateEmulator`, receiving the
   emulator's unique name (`emulator-<n>`).
3. `bt-hci-virtual` creates an `EmulatorDevice`, which publishes the
   `emulator-<n>` instance of `fuchsia.hardware.bluetooth.EmulatorService`.
4. `hci-emulator-client` connects to `Emulator` through that `EmulatorService`
   instance (the first connection owns the emulator's lifetime) and calls
   `Emulator.Publish`.
5. `EmulatorDevice` publishes the `emulator-<n>` instance of
   `fuchsia.hardware.bluetooth.Service`.
6. Other `Emulator` API calls can be made.
7. `bt-init` detects the `fuchsia.hardware.bluetooth.Service` instance and
   creates a new `bt-host` component backed by `EmulatorDevice`'s
   `FakeController`.

```
┌───────────────────────────────┐ 2. CreateEmulator() ┌──────────────────────┐
│        bt-hci-virtual         │◄────────────────────┤ hci-emulator-client  │
│ 1. Publishes                  │                     └──────────┬───────────┘
│    VirtualControllerService   │                                │
└───────────────┬───────────────┘                                │
                │                                                │
                ▼                                                │
┌───────────────────────────────┐ 4. Emulator.Publish()          │
│        EmulatorDevice         │◄───────────────────────────────┘
│       (FakeController)        │ 6. Other Emulator API calls
│ 3. Publishes EmulatorService  │◄───────────────────────────────┐
│ 5. Publishes Service          │                                │
└───────────────┬───────────────┘                     ┌──────────┴───────────┐
                │                                     │ BT Integration Tests │
                ▼  7. Watched by bt-init              └──────────────────────┘
┌───────────────────────────────┐
│            bt-host            │
└───────────────────────────────┘
```
