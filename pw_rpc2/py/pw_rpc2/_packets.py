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
"""Encodes and decodes pw_rpc2 wire protocol packets.

This module is a sans-I/O implementation of the :ref:`pw_rpc2 wire protocol
<module-pw_rpc2-protocol>`, providing :py:class:`Handshake` for connection
establishment and :py:class:`Packet` for regular RPC packets.
"""

from __future__ import annotations

import dataclasses
import enum
import struct

# Enums generated from pw_rpc2's TOML definitions.
from pw_rpc2.internal.packet_type_enum import CloseMode, PacketFlag
from pw_rpc2.internal.protocol_error_enum import ProtocolError
from pw_rpc2.rpc_code import RpcStatus

__all__ = [
    'HANDSHAKE_MAGIC',
    'PROTOCOL_VERSION',
    'CloseMode',
    'CodeOrigin',
    'DecodeError',
    'Handshake',
    'HandshakeType',
    'Packet',
    'PacketFlag',
    'PacketType',
    'ProtocolError',
    'RpcStatus',
    'UnknownProtocolError',
    'is_client_error',
    'is_server_error',
    'origin_of',
    'peer_error_to_rpc_status',
    'to_protocol_error',
]


class DecodeError(Exception):
    """The bytes are not a valid pw_rpc2 packet or protobuf message.

    Attributes:
      status: The :py:class:`RpcStatus` for the failure:
        :py:attr:`RpcStatus.PROTOCOL_ERROR` for a malformed packet, or
        :py:attr:`RpcStatus.DATA_ERROR` for a payload that could not be decoded
        as a protobuf message.
    """

    def __init__(
        self,
        message: str,
        status: RpcStatus = RpcStatus.PROTOCOL_ERROR,
    ) -> None:
        super().__init__(message)
        self.status = status


_FLAGS_MASK = PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD | PacketFlag.START
_CLOSE_MODE_MASK = (
    CloseMode.STREAM_END | CloseMode.OK_TERMINAL | CloseMode.ERROR_TERMINAL
)


def _is_valid(flags: int, close_mode: CloseMode) -> bool:
    """The rules that determine which packet types are valid."""
    if flags & ~_FLAGS_MASK:
        return False

    server = bool(flags & PacketFlag.SERVER)

    if flags & PacketFlag.START:
        # Only a client starts a call, and a start packet cannot end the RPC.
        return not server and close_mode in (
            CloseMode.OPEN,
            CloseMode.STREAM_END,
        )

    if close_mode is CloseMode.STREAM_END:
        # Either side may half-close its stream, with or without a message.
        return True
    if close_mode is CloseMode.OK_TERMINAL:
        # Only the server completes an RPC successfully.
        return server
    if close_mode is CloseMode.ERROR_TERMINAL:
        # Error packets carry an error code, never a message.
        return not bool(flags & PacketFlag.HAS_PAYLOAD)

    # A packet that neither starts a call, carries a message, nor closes
    # anything would be a no-op.
    return bool(flags & PacketFlag.HAS_PAYLOAD)


@dataclasses.dataclass(frozen=True)
class PacketType:
    """The validated type byte of a regular (post-handshake) RPC packet.

    Combines :py:class:`PacketFlag` bits with a :py:class:`CloseMode` as
    described in :ref:`module-pw_rpc2-protocol-types`. Constructing an invalid
    combination raises ``ValueError``.
    """

    flags: int = 0
    close_mode: CloseMode = CloseMode.OPEN

    def __post_init__(self) -> None:
        # Normalize flags to int and close_mode to CloseMode.
        object.__setattr__(self, 'flags', int(self.flags))
        object.__setattr__(self, 'close_mode', CloseMode(self.close_mode))
        if self.flags & ~_FLAGS_MASK:
            raise ValueError(f'Invalid packet flags {self.flags:#04x}')
        if not _is_valid(self.flags, self.close_mode):
            raise ValueError(f'{self} is not a valid packet type')

    @classmethod
    def from_byte(cls, value: int) -> PacketType:
        """Decodes a type byte received on the wire.

        Raises:
          DecodeError: The byte is not a valid packet type.
        """
        try:
            return _TYPES_BY_BYTE[value]
        except KeyError:
            raise DecodeError(f'Unknown packet type {value:#04x}') from None

    @property
    def value(self) -> int:
        """The type byte as sent on the wire."""
        return self.flags | self.close_mode

    @property
    def from_server(self) -> bool:
        """True if the server sent this packet."""
        return bool(self.flags & PacketFlag.SERVER)

    @property
    def from_client(self) -> bool:
        """True if the client sent this packet."""
        return not self.from_server

    @property
    def has_payload(self) -> bool:
        """True if this packet carries a message, which may be empty."""
        return bool(self.flags & PacketFlag.HAS_PAYLOAD)

    @property
    def is_start(self) -> bool:
        """True if this packet starts a call."""
        return bool(self.flags & PacketFlag.START)

    @property
    def is_error(self) -> bool:
        """True if this packet aborts the RPC with an error code."""
        return self.close_mode is CloseMode.ERROR_TERMINAL

    @property
    def is_terminal(self) -> bool:
        """True if this packet completes the RPC."""
        return self.close_mode in (
            CloseMode.OK_TERMINAL,
            CloseMode.ERROR_TERMINAL,
        )

    def __str__(self) -> str:
        names = ['SERVER' if self.from_server else 'CLIENT']
        names += [
            flag.name
            for flag in (PacketFlag.HAS_PAYLOAD, PacketFlag.START)
            if self.flags & flag
        ]
        if self.close_mode is not CloseMode.OPEN:
            names.append(self.close_mode.name)
        return f'{"|".join(names)} ({self.value:#04x})'


def _valid_packet_types() -> dict[int, PacketType]:
    types: dict[int, PacketType] = {}
    # Values with any reserved bits (5-7) set are never valid.
    for value in range((_CLOSE_MODE_MASK | _FLAGS_MASK) + 1):
        flags = value & _FLAGS_MASK
        mode = CloseMode(value & _CLOSE_MODE_MASK)
        if _is_valid(flags, mode):
            types[value] = PacketType(flags, mode)
    return types


_TYPES_BY_BYTE = _valid_packet_types()


class CodeOrigin(enum.IntEnum):
    """Which endpoint may send a ProtocolError, encoded in its top two bits."""

    ANY = 0x00
    SERVER = 0x40
    CLIENT = 0x80
    RESERVED = 0xC0


_ORIGIN_MASK = 0xC0


@dataclasses.dataclass(frozen=True)
class UnknownProtocolError:
    """An unrecognized 8-bit error code received in an error packet.

    Endpoints only send error codes defined by the negotiated protocol version,
    so receiving an unknown code is a protocol violation.
    """

    value: int

    def __post_init__(self) -> None:
        if not 0 <= self.value <= 0xFF:
            raise ValueError(f'Error code {self.value} does not fit in 8 bits')
        if ProtocolError.is_valid(self.value):
            raise ValueError(
                f'Error code {self.value:#04x} is known '
                f'({ProtocolError(self.value)})'
            )

    @property
    def name(self) -> str:
        return f'UNKNOWN_{self.value:#04x}'

    def __str__(self) -> str:
        return self.name

    def __repr__(self) -> str:
        return f'{type(self).__name__}({self.value:#04x})'


def to_protocol_error(code: int) -> ProtocolError | UnknownProtocolError:
    """Decodes an 8-bit wire error code.

    Raises:
      ValueError: The code does not fit in 8 bits.
    """
    try:
        return ProtocolError(code)
    except ValueError:
        return UnknownProtocolError(code)


def origin_of(error: ProtocolError) -> CodeOrigin:
    """Returns which endpoint may send an error, from its top two bits."""
    return CodeOrigin(error & _ORIGIN_MASK)


def is_server_error(error: ProtocolError) -> bool:
    """True if a server may send this error code in an error packet."""
    return error is not ProtocolError.NONE and origin_of(error) in (
        CodeOrigin.ANY,
        CodeOrigin.SERVER,
    )


def is_client_error(error: ProtocolError) -> bool:
    """True if a client may send this error code in an error packet."""
    return error is not ProtocolError.NONE and origin_of(error) in (
        CodeOrigin.ANY,
        CodeOrigin.CLIENT,
    )


def peer_error_to_rpc_status(
    error: ProtocolError | UnknownProtocolError, *, from_server: bool
) -> RpcStatus:
    """Converts an error code received in an error packet to RpcStatus."""
    if isinstance(error, UnknownProtocolError):
        return RpcStatus.PROTOCOL_ERROR
    if not (is_server_error(error) if from_server else is_client_error(error)):
        return RpcStatus.PROTOCOL_ERROR
    return _PEER_ERROR_TO_RPC_STATUS[error]


# LINT.IfChange(py_rpc2_peer_error_mapping)
_PEER_ERROR_TO_RPC_STATUS: dict[ProtocolError, RpcStatus] = {
    ProtocolError.CANCELLED: RpcStatus.CANCELLED,
    ProtocolError.DROPPED_WITHOUT_RESPONSE: RpcStatus.CANCELLED,
    ProtocolError.METHOD_RETURNED_WITHOUT_FINISHING: RpcStatus.CANCELLED,
    ProtocolError.FAILED_TO_ALLOCATE_CALL: RpcStatus.UNAVAILABLE,
    ProtocolError.FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING: (
        RpcStatus.ABORTED
    ),
    ProtocolError.SERVICE_UNREGISTERED: RpcStatus.ABORTED,
    ProtocolError.METHOD_TYPE_MISMATCH: RpcStatus.UNIMPLEMENTED,
    ProtocolError.UNKNOWN_SERVICE: RpcStatus.UNIMPLEMENTED,
    ProtocolError.UNKNOWN_METHOD: RpcStatus.UNIMPLEMENTED,
    ProtocolError.INVALID_REQUEST_PAYLOAD: RpcStatus.DATA_ERROR,
    ProtocolError.NONE: RpcStatus.PROTOCOL_ERROR,
    ProtocolError.INTERNAL: RpcStatus.PROTOCOL_ERROR,
    ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT: RpcStatus.PROTOCOL_ERROR,
    ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL: RpcStatus.PROTOCOL_ERROR,
    ProtocolError.RECEIVED_DUPLICATE_START: RpcStatus.PROTOCOL_ERROR,
}
# LINT.ThenChange(//pw_rpc2/cpp/connection_task.cc:cpp_rpc2_peer_error_mapping)


_HEADER = struct.Struct('<IB')  # call_id, type
_REQUEST_IDS = struct.Struct('<II')  # service_id, method_id
_ERROR = struct.Struct('<B')


def _header_size(packet_type: PacketType) -> int:
    """Returns the size of a packet of this type, excluding its payload."""
    if packet_type.is_start:
        return _HEADER.size + _REQUEST_IDS.size
    if packet_type.is_error:
        return _HEADER.size + _ERROR.size
    return _HEADER.size


_REQUEST_TYPE = PacketType(
    PacketFlag.START | PacketFlag.HAS_PAYLOAD, CloseMode.STREAM_END
)
_OPEN_STREAM_TYPE = PacketType(PacketFlag.START)
_RESPONSE_TYPE = PacketType(
    PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD, CloseMode.OK_TERMINAL
)
_CLIENT_MESSAGE_TYPE = PacketType(PacketFlag.HAS_PAYLOAD)
_SERVER_MESSAGE_TYPE = PacketType(PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD)
_CLIENT_STREAM_END_TYPE = PacketType(close_mode=CloseMode.STREAM_END)
_SERVER_STREAM_END_TYPE = PacketType(PacketFlag.SERVER, CloseMode.STREAM_END)
_SERVER_FINISH_TYPE = PacketType(PacketFlag.SERVER, CloseMode.OK_TERMINAL)
_CLIENT_ERROR_TYPE = PacketType(close_mode=CloseMode.ERROR_TERMINAL)
_SERVER_ERROR_TYPE = PacketType(PacketFlag.SERVER, CloseMode.ERROR_TERMINAL)


@dataclasses.dataclass(frozen=True)
class Packet:
    """A regular pw_rpc2 packet, exchanged after the handshake completes.

    The classmethods (e.g. :py:meth:`request`) construct the common packet
    types; the constructor accepts any valid :py:class:`PacketType` and checks
    that only the fields for that type are set.
    """

    type: PacketType
    call_id: int
    service_id: int = 0
    method_id: int = 0
    payload: bytes = b''
    error: ProtocolError | UnknownProtocolError | None = None

    def __post_init__(self) -> None:
        if not self.type.is_start and (self.service_id or self.method_id):
            raise ValueError(
                f'{self.type} packets do not carry service or method IDs'
            )
        if self.type.is_error:
            if self.error is None:
                raise ValueError(f'{self.type} packets require an error')
        elif self.error is not None:
            raise ValueError(f'{self.type} packets carry no error')
        if not self.type.has_payload and self.payload:
            raise ValueError(f'{self.type} packets carry no payload')

    @classmethod
    def request(
        cls, call_id: int, service_id: int, method_id: int, payload: bytes = b''
    ) -> Packet:
        """Starts a unary or server streaming call with its request."""
        return cls(_REQUEST_TYPE, call_id, service_id, method_id, payload)

    @classmethod
    def open_stream(
        cls, call_id: int, service_id: int, method_id: int
    ) -> Packet:
        """Starts a client or bidirectional streaming call."""
        return cls(_OPEN_STREAM_TYPE, call_id, service_id, method_id)

    @classmethod
    def response(cls, call_id: int, payload: bytes = b'') -> Packet:
        """Completes the RPC successfully with a final message."""
        return cls(_RESPONSE_TYPE, call_id, payload=payload)

    @classmethod
    def client_message(cls, call_id: int, payload: bytes = b'') -> Packet:
        """Sends a message on the client's stream."""
        return cls(_CLIENT_MESSAGE_TYPE, call_id, payload=payload)

    @classmethod
    def server_message(cls, call_id: int, payload: bytes = b'') -> Packet:
        """Sends a message on the server's stream."""
        return cls(_SERVER_MESSAGE_TYPE, call_id, payload=payload)

    @classmethod
    def client_stream_end(cls, call_id: int) -> Packet:
        """Half-closes the client's stream without ending the RPC."""
        return cls(_CLIENT_STREAM_END_TYPE, call_id)

    @classmethod
    def server_stream_end(cls, call_id: int) -> Packet:
        """Half-closes the server's stream without ending the RPC."""
        return cls(_SERVER_STREAM_END_TYPE, call_id)

    @classmethod
    def server_finish(cls, call_id: int) -> Packet:
        """Completes the RPC successfully without a message."""
        return cls(_SERVER_FINISH_TYPE, call_id)

    @classmethod
    def client_error(cls, call_id: int, error: ProtocolError) -> Packet:
        """Aborts the RPC from the client with an error code."""
        return cls(_CLIENT_ERROR_TYPE, call_id, error=error)

    @classmethod
    def server_error(cls, call_id: int, error: ProtocolError) -> Packet:
        """Aborts the RPC from the server with an error code."""
        return cls(_SERVER_ERROR_TYPE, call_id, error=error)

    def encode(self) -> bytes:
        """Returns the wire representation of this packet."""
        data = _HEADER.pack(self.call_id, self.type.value)

        if self.type.is_start:
            data += _REQUEST_IDS.pack(self.service_id, self.method_id)

        if self.type.is_error:
            assert self.error is not None
            data += _ERROR.pack(self.error.value)

        if self.type.has_payload:
            data += self.payload

        return data

    @classmethod
    def decode(cls, data: bytes) -> Packet:
        """Parses a complete packet.

        Raises:
          DecodeError: The packet has an unrecognized type, is too short for
            its type, or has unexpected trailing bytes.
        """
        if len(data) < _HEADER.size:
            raise DecodeError(f'{len(data)}-byte packet is too short')

        call_id, raw_type = _HEADER.unpack_from(data)
        packet_type = PacketType.from_byte(raw_type)

        size = _header_size(packet_type)
        if len(data) < size:
            raise DecodeError(
                f'{len(data)}-byte {packet_type} packet is too short; '
                f'expected at least {size} bytes'
            )

        service_id = method_id = 0
        if packet_type.is_start:
            service_id, method_id = _REQUEST_IDS.unpack_from(data, _HEADER.size)

        error = (
            to_protocol_error(_ERROR.unpack_from(data, _HEADER.size)[0])
            if packet_type.is_error
            else None
        )

        if packet_type.has_payload:
            payload = data[size:]
        elif len(data) != size:
            raise DecodeError(
                f'{len(data)}-byte {packet_type} packet has '
                f'{len(data) - size} unexpected trailing bytes'
            )
        else:
            payload = b''

        return cls(packet_type, call_id, service_id, method_id, payload, error)


# The only protocol version that exists so far.
PROTOCOL_VERSION = 1

HANDSHAKE_MAGIC = 0x43505250  # 'PRPC'

_HANDSHAKE = struct.Struct('<IBBH')  # magic, version, type, reserved


class HandshakeType(enum.IntEnum):
    """The stage of the three-way connection handshake."""

    SYN = 1
    SYN_ACK = 2
    ACK = 3


@dataclasses.dataclass(frozen=True)
class Handshake:
    """A connection handshake packet.

    Decoding accepts any non-zero version so that the endpoint, which knows
    which versions it supports, can decide how to negotiate. Bytes past the
    8-byte header are ignored for forward compatibility.
    """

    type: HandshakeType
    version: int = PROTOCOL_VERSION

    def encode(self) -> bytes:
        """Returns the wire representation of this handshake packet."""
        return _HANDSHAKE.pack(
            HANDSHAKE_MAGIC, self.version, self.type.value, 0
        )

    @classmethod
    def decode(cls, data: bytes) -> Handshake:
        """Parses a handshake packet.

        Raises:
          DecodeError: The data is not a valid handshake packet.
        """
        if len(data) < _HANDSHAKE.size:
            raise DecodeError(
                f'Handshake packets are at least {_HANDSHAKE.size} bytes, '
                f'not {len(data)}'
            )

        magic, version, raw_type, _ = _HANDSHAKE.unpack_from(data)

        if magic != HANDSHAKE_MAGIC:
            raise DecodeError(f'Bad handshake magic {magic:#010x}')

        if version == 0:
            raise DecodeError('Handshake version cannot be 0')

        try:
            return cls(HandshakeType(raw_type), version)
        except ValueError:
            raise DecodeError(f'Bad handshake type {raw_type}') from None
