# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Example demonstrating a custom Python client transport for pw_rpc."""

import unittest
from pw_hdlc import decode, encode
import pw_rpc
from pw_rpc import callback_client

# Import generated proto modules
from pw_rpc.examples import sensor_service_pb2


# DOCSTAG: [pw_rpc-examples-python-transport]
def send_to_device(data: bytes) -> None:
    """Encapsulates data in an HDLC frame and sends it over the transport."""
    frame = encode.ui_frame(ord('R'), data)
    # Write the frame to your transport (e.g. serial port, socket, or BLE):
    # ser.write(frame)
    del frame


CHANNEL_ID = 1
channel = pw_rpc.Channel(CHANNEL_ID, send_to_device)

# Create the RPC Client:
client = pw_rpc.Client.from_modules(
    callback_client.Impl(),
    [channel],
    [sensor_service_pb2],
)


def handle_incoming_bytes(raw_bytes: bytes) -> None:
    """Processes incoming bytes from the transport and forwards RPC packets."""
    decoder = decode.FrameDecoder()
    for frame in decoder.process_valid_frames(raw_bytes):
        if frame.address == ord('R'):
            client.process_packet(frame.data)


def invoke_rpc_example() -> None:
    """Invokes an RPC method on the connected device."""
    sensor_service = client.channel(
        CHANNEL_ID
    ).rpcs.pw.rpc.examples.SensorService

    # 1. Unary call
    status, response = sensor_service.GetReading(sensor_id=1)
    if status.ok():
        print(f"Temperature: {response.temperature} C")
    else:
        print(f"RPC failed with status: {status}")


# DOCSTAG: [pw_rpc-examples-python-transport]


class HostClientExampleTest(unittest.TestCase):
    def test_client_instantiation(self) -> None:
        self.assertIsNotNone(client)
        self.assertEqual(len(client.services), 1)


if __name__ == '__main__':
    unittest.main()
