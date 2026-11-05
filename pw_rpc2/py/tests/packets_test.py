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
"""Tests encoding and decoding pw_rpc2 packets."""

import unittest

from pw_rpc2._packets import (
    CloseMode,
    CodeOrigin,
    DecodeError,
    Handshake,
    HandshakeType,
    Packet,
    PacketFlag,
    PacketType,
    ProtocolError,
    RpcStatus,
    UnknownProtocolError,
    is_client_error,
    is_server_error,
    origin_of,
    peer_error_to_rpc_status,
    to_protocol_error,
)

# The valid type bytes, which match the combinations in protocol.rst.
_VALID_TYPES = frozenset(
    {
        0x02,
        0x03,
        0x04,
        0x06,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0E,
        0x11,
        0x13,
        0x18,
        0x19,
    }
)


class PacketTypeTest(unittest.TestCase):
    """Tests the packet type properties."""

    def test_packet_type_enums(self) -> None:
        self.assertEqual(list(PacketFlag), [0x01, 0x02, 0x04])
        self.assertEqual(PacketFlag.SERVER, 0x01)
        self.assertEqual(PacketFlag.HAS_PAYLOAD, 0x02)
        self.assertEqual(PacketFlag.START, 0x04)
        self.assertEqual(str(PacketFlag.HAS_PAYLOAD), 'HAS_PAYLOAD')
        self.assertEqual(CloseMode.OPEN, 0x00)
        self.assertEqual(CloseMode.STREAM_END, 0x08)
        self.assertEqual(CloseMode.OK_TERMINAL, 0x10)
        self.assertEqual(CloseMode.ERROR_TERMINAL, 0x18)
        self.assertEqual(str(CloseMode.OK_TERMINAL), 'OK_TERMINAL')

    def test_from_byte_accepts_only_valid_types(self) -> None:
        valid = set()
        for byte in range(256):
            try:
                PacketType.from_byte(byte)
            except DecodeError:
                continue
            valid.add(byte)
        self.assertEqual(valid, _VALID_TYPES)

    def test_properties_match_bits(self) -> None:
        for byte in _VALID_TYPES:
            with self.subTest(byte=byte):
                packet_type = PacketType.from_byte(byte)
                self.assertEqual(packet_type.value, byte)
                self.assertEqual(packet_type.from_server, bool(byte & 0x01))
                self.assertNotEqual(
                    packet_type.from_client, packet_type.from_server
                )
                self.assertEqual(packet_type.has_payload, bool(byte & 0x02))
                self.assertEqual(packet_type.is_start, bool(byte & 0x04))
                self.assertIs(packet_type.close_mode, CloseMode(byte & 0x18))
                self.assertEqual(
                    packet_type.is_error,
                    packet_type.close_mode is CloseMode.ERROR_TERMINAL,
                )
                self.assertEqual(
                    packet_type.is_terminal,
                    packet_type.close_mode
                    in (CloseMode.OK_TERMINAL, CloseMode.ERROR_TERMINAL),
                )

    def test_constructor_rejects_invalid_combinations(self) -> None:
        for flags, close_mode in [
            # Only clients start calls.
            (PacketFlag.SERVER | PacketFlag.START, CloseMode.OPEN),
            # Start packets cannot end the RPC.
            (PacketFlag.START, CloseMode.ERROR_TERMINAL),
            # A packet must do something.
            (PacketFlag.SERVER, CloseMode.OPEN),
            # Only servers complete RPCs successfully.
            (PacketFlag.HAS_PAYLOAD, CloseMode.OK_TERMINAL),
            # Error packets carry no message.
            (PacketFlag.HAS_PAYLOAD, CloseMode.ERROR_TERMINAL),
        ]:
            with self.subTest(flags=flags, close_mode=close_mode):
                with self.assertRaisesRegex(
                    ValueError, 'is not a valid packet type'
                ):
                    PacketType(flags, close_mode)

        # Reserved or close mode bits in flags must be zero.
        for bad_flags in (
            0x20 | PacketFlag.HAS_PAYLOAD,
            CloseMode.STREAM_END | PacketFlag.HAS_PAYLOAD,
        ):
            with self.subTest(bad_flags=bad_flags):
                with self.assertRaisesRegex(ValueError, 'Invalid packet flags'):
                    PacketType(bad_flags, CloseMode.OPEN)

    def test_equality(self) -> None:
        response = PacketType(
            PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD, CloseMode.OK_TERMINAL
        )
        self.assertEqual(response, PacketType.from_byte(0x13))
        self.assertEqual(hash(response), hash(PacketType.from_byte(0x13)))
        self.assertNotEqual(response, PacketType.from_byte(0x11))

    def test_str(self) -> None:
        self.assertEqual(
            str(PacketType.from_byte(0x13)),
            'SERVER|HAS_PAYLOAD|OK_TERMINAL (0x13)',
        )
        self.assertEqual(
            str(PacketType.from_byte(0x0E)),
            'CLIENT|HAS_PAYLOAD|START|STREAM_END (0x0e)',
        )
        self.assertEqual(
            str(PacketType.from_byte(0x02)), 'CLIENT|HAS_PAYLOAD (0x02)'
        )
        self.assertEqual(
            str(PacketType.from_byte(0x18)), 'CLIENT|ERROR_TERMINAL (0x18)'
        )

    def test_request_and_open_stream_are_distinct(self) -> None:
        # A unary request is distinguishable from a streaming open on the wire,
        # even when the request message is empty.
        self.assertNotEqual(
            Packet.request(1, 2, 3).encode(),
            Packet.open_stream(1, 2, 3).encode(),
        )


class PacketTest(unittest.TestCase):
    """Tests Packet encoding and decoding."""

    def test_encode_request(self) -> None:
        packet = Packet.request(1, 0x2CE55DD9, 0x91D11BF6, b'\x08\x01')
        self.assertEqual(
            packet.encode(),
            bytes.fromhex('01000000 0e d95de52c f61bd191 0801'),
        )

    def test_encode_request_without_payload(self) -> None:
        self.assertEqual(
            Packet.request(0x01020304, 5, 6).encode(),
            bytes.fromhex('04030201 0e 05000000 06000000'),
        )

    def test_encode_open_stream(self) -> None:
        self.assertEqual(
            Packet.open_stream(0x01020304, 5, 6).encode(),
            bytes.fromhex('04030201 04 05000000 06000000'),
        )

    def test_encode_messages(self) -> None:
        self.assertEqual(
            Packet.client_message(7, b'hi').encode(),
            bytes.fromhex('07000000 02') + b'hi',
        )
        self.assertEqual(
            Packet.server_message(7, b'hi').encode(),
            bytes.fromhex('07000000 03') + b'hi',
        )
        self.assertEqual(
            Packet.response(7, b'').encode(), bytes.fromhex('07000000 13')
        )

    def test_encode_stream_end(self) -> None:
        self.assertEqual(
            Packet.client_stream_end(0xFFFFFFFF).encode(),
            bytes.fromhex('ffffffff 08'),
        )
        self.assertEqual(
            Packet.server_stream_end(0xFFFFFFFF).encode(),
            bytes.fromhex('ffffffff 09'),
        )

    def test_encode_server_finish(self) -> None:
        self.assertEqual(
            Packet.server_finish(0xFFFFFFFF).encode(),
            bytes.fromhex('ffffffff 11'),
        )

    def test_encode_errors(self) -> None:
        self.assertEqual(
            Packet.client_error(2, ProtocolError.CANCELLED).encode(),
            bytes.fromhex('02000000 18 02'),
        )
        self.assertEqual(
            Packet.server_error(2, ProtocolError.UNKNOWN_METHOD).encode(),
            bytes.fromhex('02000000 19 41'),
        )

    def test_error_without_code_fails(self) -> None:
        with self.assertRaises(ValueError):
            Packet(PacketType(PacketFlag.SERVER, CloseMode.ERROR_TERMINAL), 1)

    def test_error_on_non_error_packet_fails(self) -> None:
        with self.assertRaises(ValueError):
            Packet(
                PacketType(PacketFlag.SERVER, CloseMode.OK_TERMINAL),
                1,
                error=ProtocolError.CANCELLED,
            )

    def test_service_or_method_id_on_non_start_packet_fails(self) -> None:
        with self.assertRaises(ValueError):
            Packet(
                PacketType(PacketFlag.HAS_PAYLOAD),
                1,
                service_id=2,
                payload=b'hi',
            )
        with self.assertRaises(ValueError):
            Packet(
                PacketType(PacketFlag.HAS_PAYLOAD),
                1,
                method_id=3,
                payload=b'hi',
            )

    def test_payload_without_has_payload_fails(self) -> None:
        with self.assertRaises(ValueError):
            Packet(PacketType(PacketFlag.START), 1, 2, 3, b'payload')

    def test_round_trip(self) -> None:
        """Tests encoding and decoding all packet types."""
        packets = [
            Packet.request(1, 2, 3, b'request'),
            Packet.open_stream(1, 2, 3),
            Packet(
                PacketType(PacketFlag.START | PacketFlag.HAS_PAYLOAD),
                1,
                2,
                3,
                b'first',
            ),
            Packet(PacketType(PacketFlag.START, CloseMode.STREAM_END), 1, 2, 3),
            Packet.response(4, b'response'),
            Packet.client_message(5, b'client'),
            Packet.server_message(6, b'server'),
            Packet(
                PacketType(PacketFlag.HAS_PAYLOAD, CloseMode.STREAM_END),
                5,
                payload=b'last',
            ),
            Packet(
                PacketType(
                    PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD,
                    CloseMode.STREAM_END,
                ),
                6,
                payload=b'last',
            ),
            Packet.client_stream_end(7),
            Packet.server_stream_end(7),
            Packet.server_finish(8),
            Packet.client_error(
                9, ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT
            ),
            Packet.client_error(9, ProtocolError.METHOD_TYPE_MISMATCH),
            Packet.server_error(
                10,
                ProtocolError.FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING,
            ),
            Packet.server_error(10, ProtocolError.METHOD_TYPE_MISMATCH),
        ]
        self.assertEqual({p.type.value for p in packets}, _VALID_TYPES)
        for packet in packets:
            with self.subTest(packet=packet):
                self.assertEqual(Packet.decode(packet.encode()), packet)

    def test_decode_empty_payload(self) -> None:
        packet = Packet.decode(bytes.fromhex('01000000 03'))
        self.assertEqual(packet, Packet.server_message(1))

    def test_decode_rejects_trailing_bytes_of_packets_without_payloads(
        self,
    ) -> None:
        for data in [
            bytes.fromhex('01000000 11 ab'),
            bytes.fromhex('01000000 09 abcd'),
            bytes.fromhex('01000000 08 ab'),
            bytes.fromhex('01000000 04 02000000 03000000 ab'),
            bytes.fromhex('01000000 0c 02000000 03000000 ab'),
            bytes.fromhex('01000000 19 03 00'),
        ]:
            with self.subTest(data=data):
                with self.assertRaisesRegex(DecodeError, 'trailing'):
                    Packet.decode(data)

    def test_decode_too_short(self) -> None:
        for data in [
            b'',
            bytes.fromhex('01000000'),
            bytes.fromhex('01000000 0e 0100000002'),
            bytes.fromhex('01000000 04 01'),
            bytes.fromhex('01000000 19'),
        ]:
            with self.subTest(data=data):
                with self.assertRaises(DecodeError) as context:
                    Packet.decode(data)
                self.assertIs(
                    context.exception.status, RpcStatus.PROTOCOL_ERROR
                )

    def test_decode_unknown_type(self) -> None:
        # 0x05 and 0x07: only clients start calls. 0x10: only servers complete
        # them. 0x20 and up: reserved bits are set.
        for raw_type in (0x00, 0x01, 0x05, 0x07, 0x10, 0x20, 0xFF):
            with self.subTest(raw_type=raw_type):
                with self.assertRaises(DecodeError) as context:
                    Packet.decode(bytes([1, 0, 0, 0, raw_type]))
                self.assertIs(
                    context.exception.status, RpcStatus.PROTOCOL_ERROR
                )

    def test_decode_unknown_error_codes(self) -> None:
        raw = bytes.fromhex('01000000 19 ff')
        packet = Packet.decode(raw)
        self.assertEqual(packet.encode(), raw)
        assert packet.error is not None
        self.assertEqual(packet.error, UnknownProtocolError(0xFF))
        self.assertIs(
            peer_error_to_rpc_status(packet.error, from_server=True),
            RpcStatus.PROTOCOL_ERROR,
        )

        none_err = Packet.decode(bytes.fromhex('01000000 18 00')).error
        self.assertIs(none_err, ProtocolError.NONE)
        assert none_err is not None
        self.assertIs(
            peer_error_to_rpc_status(none_err, from_server=False),
            RpcStatus.PROTOCOL_ERROR,
        )

        wrong_role = Packet.decode(bytes.fromhex('01000000 18 41')).error
        self.assertIs(wrong_role, ProtocolError.UNKNOWN_METHOD)
        assert wrong_role is not None
        self.assertIs(
            peer_error_to_rpc_status(wrong_role, from_server=False),
            RpcStatus.PROTOCOL_ERROR,
        )


class ProtocolErrorAndStatusTest(unittest.TestCase):
    """Tests RpcStatus and ProtocolError helpers."""

    def test_rpc_status_enum(self) -> None:
        self.assertEqual(RpcStatus.OK, 0)
        self.assertEqual(RpcStatus.PROTOCOL_ERROR, 12)
        self.assertEqual(str(RpcStatus.UNIMPLEMENTED), 'UNIMPLEMENTED')
        self.assertTrue(RpcStatus.is_valid(0))
        self.assertFalse(RpcStatus.is_valid(13))
        self.assertTrue(RpcStatus.OK.ok())
        for status in RpcStatus:
            if status is not RpcStatus.OK:
                self.assertFalse(status.ok())

    def test_protocol_error_enum(self) -> None:
        self.assertEqual(ProtocolError.NONE, 0x00)
        self.assertEqual(ProtocolError.UNKNOWN_SERVICE, 0x40)
        self.assertEqual(
            ProtocolError.METHOD_RETURNED_WITHOUT_FINISHING,
            0x48,
        )
        self.assertEqual(str(ProtocolError.UNKNOWN_METHOD), 'UNKNOWN_METHOD')
        self.assertTrue(ProtocolError.is_valid(0x48))
        self.assertFalse(ProtocolError.is_valid(0x3F))

    def test_decode_error_status(self) -> None:
        self.assertIs(DecodeError('bad').status, RpcStatus.PROTOCOL_ERROR)
        self.assertIs(
            DecodeError('bad', status=RpcStatus.DATA_ERROR).status,
            RpcStatus.DATA_ERROR,
        )

    def test_to_protocol_error(self) -> None:
        self.assertIs(to_protocol_error(0x41), ProtocolError.UNKNOWN_METHOD)
        self.assertIs(
            to_protocol_error(ProtocolError.CANCELLED), ProtocolError.CANCELLED
        )
        self.assertEqual(to_protocol_error(0x06), UnknownProtocolError(0x06))

        for bad_code in (-1, 0x100):
            with self.subTest(bad_code=bad_code):
                with self.assertRaises(ValueError):
                    to_protocol_error(bad_code)

    def test_unknown_protocol_error(self) -> None:
        error = UnknownProtocolError(0xFF)
        self.assertEqual(error.value, 0xFF)
        self.assertEqual(error.name, 'UNKNOWN_0xff')
        self.assertEqual(str(error), 'UNKNOWN_0xff')
        self.assertEqual(repr(error), 'UnknownProtocolError(0xff)')
        self.assertNotEqual(error, 0xFF)

        for known in (0x00, 0x41, ProtocolError.CANCELLED):
            with self.subTest(known=known):
                with self.assertRaises(ValueError):
                    UnknownProtocolError(known)

        for bad_code in (-1, 0x100):
            with self.subTest(bad_code=bad_code):
                with self.assertRaises(ValueError):
                    UnknownProtocolError(bad_code)

    def test_role_ranges(self) -> None:
        self.assertIs(origin_of(ProtocolError.NONE), CodeOrigin.ANY)
        self.assertFalse(is_server_error(ProtocolError.NONE))
        self.assertFalse(is_client_error(ProtocolError.NONE))

        self.assertIs(origin_of(ProtocolError.CANCELLED), CodeOrigin.ANY)
        self.assertTrue(is_server_error(ProtocolError.CANCELLED))
        self.assertTrue(is_client_error(ProtocolError.CANCELLED))

        self.assertIs(
            origin_of(ProtocolError.UNKNOWN_METHOD), CodeOrigin.SERVER
        )
        self.assertTrue(is_server_error(ProtocolError.UNKNOWN_METHOD))
        self.assertFalse(is_client_error(ProtocolError.UNKNOWN_METHOD))

    def test_peer_error_to_rpc_status_from_server(self) -> None:
        expected_from_server = {
            ProtocolError.NONE: RpcStatus.PROTOCOL_ERROR,
            ProtocolError.INTERNAL: RpcStatus.PROTOCOL_ERROR,
            ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT: (
                RpcStatus.PROTOCOL_ERROR
            ),
            ProtocolError.CANCELLED: RpcStatus.CANCELLED,
            ProtocolError.FAILED_TO_ALLOCATE_CALL: RpcStatus.UNAVAILABLE,
            ProtocolError.FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING: (
                RpcStatus.ABORTED
            ),
            ProtocolError.METHOD_TYPE_MISMATCH: RpcStatus.UNIMPLEMENTED,
            ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL: (
                RpcStatus.PROTOCOL_ERROR
            ),
            ProtocolError.UNKNOWN_SERVICE: RpcStatus.UNIMPLEMENTED,
            ProtocolError.UNKNOWN_METHOD: RpcStatus.UNIMPLEMENTED,
            ProtocolError.SERVICE_UNREGISTERED: RpcStatus.ABORTED,
            ProtocolError.DROPPED_WITHOUT_RESPONSE: RpcStatus.CANCELLED,
            ProtocolError.METHOD_RETURNED_WITHOUT_FINISHING: (
                RpcStatus.CANCELLED
            ),
            ProtocolError.INVALID_REQUEST_PAYLOAD: RpcStatus.DATA_ERROR,
            ProtocolError.RECEIVED_DUPLICATE_START: RpcStatus.PROTOCOL_ERROR,
        }
        self.assertEqual(set(expected_from_server), set(ProtocolError))
        for error, expected in expected_from_server.items():
            with self.subTest(error=error):
                self.assertIs(
                    peer_error_to_rpc_status(error, from_server=True), expected
                )

    def test_peer_error_to_rpc_status_from_client(self) -> None:
        expected_from_client = {
            ProtocolError.CANCELLED: RpcStatus.CANCELLED,
            ProtocolError.METHOD_TYPE_MISMATCH: RpcStatus.UNIMPLEMENTED,
        }
        for error in ProtocolError:
            expected = expected_from_client.get(error, RpcStatus.PROTOCOL_ERROR)
            with self.subTest(error=error):
                self.assertIs(
                    peer_error_to_rpc_status(error, from_server=False), expected
                )
        self.assertIs(
            peer_error_to_rpc_status(
                UnknownProtocolError(0x80), from_server=False
            ),
            RpcStatus.PROTOCOL_ERROR,
        )


class HandshakeTest(unittest.TestCase):
    """Tests handshake packets."""

    def test_encode(self) -> None:
        self.assertEqual(
            Handshake(HandshakeType.SYN).encode(), b'PRPC\x01\x01\x00\x00'
        )
        self.assertEqual(
            Handshake(HandshakeType.ACK, 7).encode(), b'PRPC\x07\x03\x00\x00'
        )

    def test_round_trip(self) -> None:
        for handshake_type in HandshakeType:
            handshake = Handshake(handshake_type)
            self.assertEqual(Handshake.decode(handshake.encode()), handshake)

    def test_decode_accepts_other_versions(self) -> None:
        self.assertEqual(
            Handshake.decode(b'PRPC\x09\x02\x00\x00'),
            Handshake(HandshakeType.SYN_ACK, 9),
        )

    def test_decode_accepts_trailing_bytes(self) -> None:
        self.assertEqual(
            Handshake.decode(b'PRPC\x01\x01\x00\x00\x00\xff'),
            Handshake(HandshakeType.SYN, 1),
        )

    def test_decode_invalid(self) -> None:
        for data in [
            b'PRPC\x01\x01\x00',
            b'PRPC\x00\x01\x00\x00',
            b'XRPC\x01\x01\x00\x00',
            b'PRPC\x01\x04\x00\x00',
        ]:
            with self.subTest(data=data):
                with self.assertRaises(DecodeError) as context:
                    Handshake.decode(data)
                self.assertIs(
                    context.exception.status, RpcStatus.PROTOCOL_ERROR
                )


if __name__ == '__main__':
    unittest.main()
