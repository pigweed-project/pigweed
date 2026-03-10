# Sapphire Host Stack (pw_bluetooth_sapphire)

## Headers
In each host layer, headers are in a deeply nested path in the public
directory. They are NOT located next to the .cc files. For example:
`pw_bluetooth_sapphire/host/hci/public/pw_bluetooth_sapphire/internal/host/hci/connection.h`
is the header for `pw_bluetooth_sapphire/host/hci/connection.cc`.

## Bazel
### Running Bazel Tests
You MUST use the `--config googletest` Bazel flag when running Bazel tests.

Example running only hci layer tests:
```
bazelisk test --config googletest //pw_bluetooth_sapphire/host/hci:hci_test \
  --test_arg=--gtest_filter="*" \
  --test_output=all \
  --copt=-DPW_LOG_LEVEL_DEFAULT=PW_LOG_LEVEL_ERROR
```
You can usethe `gtest_filter` option to run a specific test. `gtest_filter`
supports wildcard syntax: `*MyTest`.

To run all tests:
```
bazelisk test --config googletest //pw_bluetooth_sapphire/host/...
```

### Bazel tips
Do NOT use the `copts` parameter, especially not to fix include path issues.
Use `strip_include_prefix` to add the public directory to the include path.

## Packet serialization/deserialization
You must use Emboss tool to serialize and deserialize packets. Emboss packet
definitions are in the pw_bluetooth module. Emboss works by generating a C++
header (with suffix `.emb.h`) from an `.emb` file, so you have to build your
`.emb` change for it to be reflected in C++. The generated `.emb.h` file is in
a build directory that you don't have access to.

The HCI command Emboss packet definitions are in `pw_bluetooth/public/pw_bluetooth/hci_commands.emb`.
The HCI event Emboss packet definitions are in `pw_bluetooth/public/pw_bluetooth/hci_events.emb`.
Common HCI emboss definitions (like the OpCode enum) are in `pw_bluetooth/public/pw_bluetooth/hci_common.emb`.

<!-- inclusive-language: disable -->
You can read the Emboss language reference here: [Emboss language reference](https://raw.githubusercontent.com/google/emboss/refs/heads/master/doc/language-reference.md)
You can read the Emboss C++ documentation here: [Emboss C++ documentation](https://raw.githubusercontent.com/google/emboss/refs/heads/master/doc/cpp-reference.md)
<!-- inclusive-language: enable -->

You should always read the HCI specification before creating new Emboss packets
if you aren't certain what the command/event parameters are.

If you get compiler errors that an Emboss field doesn't exist, read the `.emb`
file again. You probably got the field name wrong.

### Writing/Reading a device address (BdAddr, DeviceAddress) Emboss field
Use the bt::DeviceAddressBytes::view() method to get an Emboss BdAddrView that can be written to an Emboss BdAddr field.
Similarly, DeviceAddressBytes has a constructor that takes a BdAddrView read from an Emboss field.

### Use Emboss to read an EventPacket
EventPacket inherits from DynamicPacket in `pw_bluetooth_sapphire/host/transport/public/pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h`.
DynamicPacket has a `view<T>()` template method templated on the Emboss view type and returns an Emboss view.

Example:
```
auto view = event.view<pw::bluetooth::emboss::LEPeriodicAdvertisingSyncLostSubeventView>();
```

### Use Emboss to write a command packet
Emboss generates a `*Writer` type for each Emboss struct.
E.g. `LESetExtendedScanParametersCommand` results in `LESetExtendedScanParametersCommandWriter`.
Don't forget the word "Command"!

Example:
```
auto packet =
    hci::CommandPacket::New<LESetExtendedScanParametersCommandWriter>(
        hci_spec::kLESetExtendedScanParameters, packet_size);
auto params = packet.view_t();
params.scanning_filter_policy().Write(filter_policy);
```

### Opcodes
HCI command opcodes are an OpCode enum in
`pw_bluetooth/public/pw_bluetooth/hci_common.emb`, accessible with the
`pw::bluetooth::emboss::OpCode` enum type. There are legacy opcode types in
`hci-spec/protocol.h` like hci_spec::kOpCodeName, but DO NOT use or add to these.

If the Emboss OpCode enum is missing an opcode, you will get an error like:
```
error: no member named              │
 │    'LE_PERIODIC_ADVERTISING_TERMINATE_SYNC' in 'pw::bluetooth::emboss::OpCode'
```
In this case, add the opcode to `OpCode` in `hci_common.emb`.

## Error and result types
### HCI
The HCI error and result types are in `pw_bluetooth_sapphire/internal/host/transport/error.h`.

## Function and callback types
Use `pw::Function` (not `fit::function`) for functions that are called multiple
times. Use `pw::Callback` (not `fit::callback`) for functions that must only be
called once.

## C++ tips
* Do not use std::make_unique with private constructors or you will get a "calling a private constructor of class" error.

## Creating HCI packets in tests
Use `pw_bluetooth_sapphire/internal/host/testing/test_packets.h` for HCI packet
creation helper functions when writing expectations or sending events in tests.

