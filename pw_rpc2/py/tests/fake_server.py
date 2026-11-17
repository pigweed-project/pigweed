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
"""A scripted server over pw_transport.forwarding_socket for unit tests."""

from __future__ import annotations

import collections

from pw_transport import ReliableSocket, SocketClosed
from pw_transport.forwarding_socket import socket_pair

from pw_rpc2._packets import Handshake, HandshakeType, Packet


class FakeServer:
    """Plays the server side of a connection, as directed by a test.

    The server's reply to the handshake is queued up front, so constructing a
    client over :py:attr:`socket` completes the handshake immediately.
    """

    def __init__(
        self,
        handshake_reply: bytes | None = Handshake(
            HandshakeType.SYN_ACK
        ).encode(),
        max_packet_size: int | None = None,
        queue_depth: int | None = None,
    ) -> None:
        self.socket: ReliableSocket
        self._server_socket: ReliableSocket
        self.socket, self._server_socket = socket_pair(
            queue_depth=queue_depth,
            max_message_size=max_packet_size,
        )
        self._peeked: collections.deque[bytes] = collections.deque()
        if handshake_reply is not None:
            self._server_socket.write(handshake_reply)

    def expect_handshake(self, version: int = 1) -> None:
        """Checks that the client sent SYN and then ACK."""
        assert self.receive_raw() == Handshake(HandshakeType.SYN).encode()
        assert (
            self.receive_raw() == Handshake(HandshakeType.ACK, version).encode()
        )

    def receive_raw(self, timeout: float = 5.0) -> bytes:
        """Returns the next raw packet the client sent."""
        if self._peeked:
            return self._peeked.popleft()
        return self._server_socket.read(timeout=timeout)

    def receive(self, timeout: float = 5.0) -> Packet:
        """Returns the next packet the client sent."""
        return Packet.decode(self.receive_raw(timeout))

    def nothing_sent(self, timeout: float = 0.05) -> bool:
        """True if the client sends nothing within the timeout."""
        if self._peeked:
            return False
        try:
            packet = self._server_socket.read(timeout=timeout)
        except (TimeoutError, SocketClosed):
            return True
        self._peeked.append(packet)
        return False

    def send(self, packet: Packet | bytes) -> None:
        """Sends a packet or raw bytes to the client."""
        if isinstance(packet, Packet):
            packet = packet.encode()
        self._server_socket.write(packet)

    def disconnect(self) -> None:
        """Simulates the server closing the connection."""
        self._server_socket.close()
