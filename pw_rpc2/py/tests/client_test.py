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
"""Tests the pw_rpc2 clients against a scripted fake server."""

import copy
import dataclasses
import inspect
import threading
import time
from typing import Any
import unittest

from google.protobuf import descriptor_pb2, descriptor_pool
from google.protobuf.descriptor import FileDescriptor

import pw_rpc2
from pw_rpc2._descriptors import Service
from pw_rpc2._packets import (
    CloseMode,
    Handshake,
    HandshakeType,
    Packet,
    PacketFlag,
    PacketType,
    ProtocolError,
    RpcStatus,
)
from pw_rpc2_test_pb import pw_rpc2_test_pb2

from fake_server import FakeServer

# pylint: disable=protected-access

Request = pw_rpc2_test_pb2.EchoRequest
Response = pw_rpc2_test_pb2.EchoResponse

_SERVICE_ID = 0xFE6FE65C
_UNARY_ID = 0x8BA299DF


def _response(val: int) -> bytes:
    return Response(val=val).SerializeToString()


_DYNAMIC_PACKAGE = 'pw.rpc2.test.dynamic'


def _dynamic_file(
    package: str = _DYNAMIC_PACKAGE,
    service_name: str = 'Dynamic',
    field_names: tuple[str, ...] = ('request', 'timeout', 'rpc2_timeout'),
) -> FileDescriptor:
    """Creates a proto with a service and configurable request fields."""
    file = descriptor_pb2.FileDescriptorProto(
        name=f'client_test_{package}_{service_name}.proto',
        package=package,
        syntax='proto3',
    )
    request = file.message_type.add(name='Request')
    for number, name in enumerate(field_names, 1):
        request.field.add(
            name=name,
            number=number,
            type=descriptor_pb2.FieldDescriptorProto.TYPE_UINT32,
            label=descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL,
        )
    file.message_type.add(name='Response')

    prefix = f'.{package}' if package else ''
    service = file.service.add(name=service_name)
    for name, client_streaming in (('Unary', False), ('ClientStream', True)):
        service.method.add(
            name=name,
            input_type=f'{prefix}.Request',
            output_type=f'{prefix}.Response',
            client_streaming=client_streaming,
        )

    pool = descriptor_pool.DescriptorPool()
    pool.Add(file)
    return pool.FindFileByName(file.name)


class ClientConnectTest(unittest.TestCase):
    """Tests constructing and closing pw_rpc2.Client."""

    def test_connect_and_context_manager(self) -> None:
        server = FakeServer()
        with pw_rpc2.Client(server.socket, [pw_rpc2_test_pb2]) as client:
            server.expect_handshake()
            self.assertFalse(client.closed)
        self.assertTrue(client.closed)
        self.assertTrue(server.socket.closed)

    def test_handshake_failure_closes_socket(self) -> None:
        server = FakeServer(Handshake(HandshakeType.SYN).encode())
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'SYN_ACK'
        ) as context:
            pw_rpc2.Client(server.socket)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(server.socket.closed)

    def test_invalid_protos_close_socket(self) -> None:
        server = FakeServer()
        with self.assertRaises(TypeError):
            bad: list[Any] = ['not a proto']
            pw_rpc2.Client(server.socket, bad)
        self.assertTrue(server.socket.closed)

    def test_negative_handshake_timeout_closes_socket(self) -> None:
        server = FakeServer()
        with self.assertRaises(ValueError):
            pw_rpc2.Client(server.socket, handshake_timeout=-1)
        self.assertTrue(server.socket.closed)
        self.assertTrue(server.nothing_sent())

    def test_negative_default_timeout_closes_socket(self) -> None:
        server = FakeServer()
        with self.assertRaises(ValueError):
            pw_rpc2.Client(server.socket, default_timeout=-1)
        self.assertTrue(server.socket.closed)
        self.assertTrue(server.nothing_sent())


class ClientTestCase(unittest.TestCase):
    """Base for tests with a connected synchronous client."""

    def setUp(self) -> None:
        self.server = FakeServer()
        self.client = pw_rpc2.Client(
            self.server.socket, [pw_rpc2_test_pb2], default_timeout=5.0
        )
        self.server.expect_handshake()
        self.service = self.client.rpcs.pw.rpc2.test.TestEcho

    def tearDown(self) -> None:
        self.client.close()


class ServicesTest(ClientTestCase):
    """Tests navigating to service and method clients."""

    def test_package_navigation(self) -> None:
        self.assertIsInstance(self.service, pw_rpc2.ServiceClient)
        self.assertIs(
            self.client.rpcs.pw.rpc2.test.TestEcho,
            self.client.rpcs['pw.rpc2.test.TestEcho'],
        )
        self.assertEqual(self.service.name, 'TestEcho')
        self.assertEqual(self.service.full_name, 'pw.rpc2.test.TestEcho')
        self.assertEqual(self.service.package, 'pw.rpc2.test')
        self.assertIs(
            self.service.descriptor,
            pw_rpc2_test_pb2.DESCRIPTOR.services_by_name['TestEcho'],
        )

        self.assertIsInstance(self.service.EchoUnary, pw_rpc2.MethodClient)
        self.assertIs(self.service.EchoUnary, self.service['EchoUnary'])
        self.assertEqual(self.service.EchoUnary.name, 'EchoUnary')
        self.assertEqual(
            self.service.EchoUnary.full_name, 'pw.rpc2.test.TestEcho.EchoUnary'
        )
        self.assertIs(self.service.EchoUnary.type, pw_rpc2.MethodType.UNARY)
        self.assertIs(
            self.service.EchoUnary.descriptor,
            self.service.descriptor.methods_by_name['EchoUnary'],
        )
        self.assertIs(self.service.EchoUnary.request, Request)
        self.assertIs(self.service.EchoUnary.response, Response)

    def test_iteration(self) -> None:
        self.assertEqual(
            [service.full_name for service in self.client.rpcs],
            ['pw.rpc2.test.TestEcho'],
        )
        self.assertEqual(len(self.client.rpcs), 1)
        self.assertIn('pw.rpc2.test.TestEcho', self.client.rpcs)
        self.assertNotIn('pw.rpc2.test', self.client.rpcs)
        self.assertEqual(
            [method.name for method in self.service],
            [
                'EchoUnary',
                'CountUpServerStream',
                'AccumulateClientStream',
                'EchoBidiStream',
            ],
        )
        self.assertEqual(len(self.service), 4)
        self.assertIn('EchoUnary', self.service)
        self.assertNotIn('Nope', self.service)
        self.assertEqual(
            [method.full_name for method in self.client.methods()],
            [
                'pw.rpc2.test.TestEcho.EchoUnary',
                'pw.rpc2.test.TestEcho.CountUpServerStream',
                'pw.rpc2.test.TestEcho.AccumulateClientStream',
                'pw.rpc2.test.TestEcho.EchoBidiStream',
            ],
        )

    def test_dir(self) -> None:
        self.assertIn('pw', dir(self.client.rpcs))
        self.assertIn('TestEcho', dir(self.client.rpcs.pw.rpc2.test))
        self.assertIn('CountUpServerStream', dir(self.service))

    def test_missing(self) -> None:
        with self.assertRaisesRegex(AttributeError, "'nope'"):
            _ = self.client.rpcs.pw.nope
        with self.assertRaisesRegex(AttributeError, 'no method'):
            _ = self.service.Nope
        with self.assertRaises(KeyError):
            _ = self.client.rpcs['pw.rpc2.test.Nope']
        with self.assertRaises(KeyError):
            _ = self.service['Nope']

    def test_add_services_replaces_by_name(self) -> None:
        old_service = self.service
        self.client.add_services(
            [pw_rpc2_test_pb2.DESCRIPTOR.services_by_name['TestEcho']]
        )
        self.assertIsNot(self.client.rpcs['pw.rpc2.test.TestEcho'], old_service)
        self.assertEqual(len(self.client.rpcs), 1)

    def test_add_services_keeps_other_service_clients(self) -> None:
        old_service = self.service
        self.client.add_services([_dynamic_file()])

        self.assertEqual(len(self.client.rpcs), 2)
        self.assertIsInstance(
            self.client.rpcs.pw.rpc2.test.dynamic.Dynamic, pw_rpc2.ServiceClient
        )
        self.assertIs(self.client.rpcs.pw.rpc2.test.TestEcho, old_service)
        self.assertIs(self.client.rpcs['pw.rpc2.test.TestEcho'], old_service)

    def test_packageless_service(self) -> None:
        self.client.add_services(
            [_dynamic_file(package='', service_name='TopLevel')]
        )
        top = self.client.rpcs.TopLevel
        self.assertIs(top, self.client.rpcs['TopLevel'])
        self.assertEqual(top.package, '')
        self.assertEqual(top.full_name, 'TopLevel')
        with self.assertRaisesRegex(AttributeError, 'rpcs has no service'):
            _ = self.client.rpcs.Missing

    def test_service_and_package_name_collision(self) -> None:
        self.client.add_services([_dynamic_file(package='a', service_name='b')])
        with self.assertRaisesRegex(ValueError, 'service and a package'):
            self.client.add_services(
                [_dynamic_file(package='a.b', service_name='C')]
            )
        self.assertIn('a.b', self.client.rpcs)
        self.assertNotIn('a.b.C', self.client.rpcs)

    def test_package_and_service_name_collision(self) -> None:
        self.client.add_services(
            [_dynamic_file(package='a.b', service_name='C')]
        )
        with self.assertRaisesRegex(ValueError, 'package and a service'):
            self.client.add_services(
                [_dynamic_file(package='a', service_name='b')]
            )
        self.assertIn('a.b.C', self.client.rpcs)
        self.assertNotIn('a.b', self.client.rpcs)

    def test_service_id_collision(self) -> None:
        colliding = dataclasses.replace(
            Service.from_descriptor(
                _dynamic_file().services_by_name['Dynamic']
            ),
            _id=_SERVICE_ID,
        )
        with self.assertRaisesRegex(ValueError, 'same ID'):
            self.client._registry.add_services([colliding])
        self.assertEqual(len(self.client.rpcs), 1)
        self.assertNotIn('pw.rpc2.test.dynamic.Dynamic', self.client.rpcs)

    def test_method_signature_and_doc(self) -> None:
        unary = inspect.signature(self.service.EchoUnary)
        self.assertEqual(
            list(unary.parameters),
            ['request', 'rpc2_timeout', 'val'],
        )
        self.assertEqual(
            list(
                inspect.signature(
                    self.service.AccumulateClientStream
                ).parameters
            ),
            ['rpc2_timeout'],
        )
        assert self.service.EchoUnary.__doc__ is not None
        assert self.service.CountUpServerStream.__doc__ is not None
        self.assertIn('Unary RPC', self.service.EchoUnary.__doc__)
        self.assertIn('val: int = 0', self.service.EchoUnary.__doc__)
        self.assertIn(
            'Returns the EchoResponse', self.service.EchoUnary.__doc__
        )
        self.assertIn(
            'Server streaming RPC', self.service.CountUpServerStream.__doc__
        )
        self.assertIn(
            'ServerStreamingCall', self.service.CountUpServerStream.__doc__
        )

    def test_repr(self) -> None:
        self.assertIn('TestEcho', repr(self.client))
        self.assertIn('TestEcho', repr(self.client.rpcs))
        self.assertIn('rpc2', repr(self.client.rpcs.pw))
        self.assertIn('CountUpServerStream', repr(self.service))

    def test_copy(self) -> None:
        self.assertIs(copy.copy(self.service).EchoUnary, self.service.EchoUnary)
        self.assertIs(
            copy.copy(self.client.rpcs).pw.rpc2.test.TestEcho, self.service
        )


class MethodClientTest(ClientTestCase):
    """Tests invoking RPCs through MethodClient."""

    def test_unary_invoke_and_call(self) -> None:
        call = self.service.EchoUnary.invoke(val=5)
        self.assertIsInstance(call, pw_rpc2.UnaryCall)
        self.assertEqual(
            self.server.receive(),
            Packet.request(
                1,
                _SERVICE_ID,
                _UNARY_ID,
                Request(val=5).SerializeToString(),
            ),
        )
        self.server.send(Packet.response(call._call_id, _response(6)))
        self.assertEqual(call.response(), Response(val=6))

        def respond() -> None:
            packet = self.server.receive()
            self.server.send(Packet.response(packet.call_id, _response(9)))

        thread = threading.Thread(target=respond)
        thread.start()
        self.assertEqual(self.service.EchoUnary(Request(val=1)).val, 9)
        thread.join()

    def test_timeout_cancels_direct_unary_calls(self) -> None:
        start = time.monotonic()
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            self.service.EchoUnary(val=1, rpc2_timeout=0.05)
        self.assertGreaterEqual(time.monotonic() - start, 0.05)

        self.assertIs(context.exception.method, self.service.EchoUnary._method)
        self.assertEqual(context.exception.timeout, 0.05)

        request = self.server.receive()
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(request.call_id, ProtocolError.CANCELLED),
        )

    def test_default_timeout(self) -> None:
        self.client.default_timeout = 0.01
        call = self.service.EchoUnary.invoke()
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            call.response()
        self.assertEqual(context.exception.timeout, 0.01)

    def test_negative_default_timeout_rejected(self) -> None:
        with self.assertRaises(ValueError):
            self.client.default_timeout = -1
        self.assertEqual(self.client.default_timeout, 5.0)

    def test_negative_timeout(self) -> None:
        with self.assertRaises(ValueError):
            self.service.EchoUnary.invoke(rpc2_timeout=-1)

    def test_invalid_request(self) -> None:
        with self.assertRaises(TypeError):
            self.service.EchoUnary(Response())
        with self.assertRaises(TypeError):
            self.service.EchoUnary(Request(), val=1)
        with self.assertRaises(ValueError):
            self.service.EchoUnary(nonexistent=1)
        with self.assertRaisesRegex(TypeError, 'write'):
            self.service.AccumulateClientStream(val=1)
        with self.assertRaises(TypeError):
            self.service.AccumulateClientStream(Request())
        self.assertTrue(self.server.nothing_sent())

    def test_request_fields_named_like_arguments(self) -> None:
        """Tests request fields that share names with RPC arguments."""
        self.client.add_services([_dynamic_file()])
        service = self.client.rpcs.pw.rpc2.test.dynamic.Dynamic
        request = service.Unary.request

        # The rpc2_timeout field is not listed, since that keyword is always
        # the call's timeout.
        self.assertEqual(
            list(inspect.signature(service.Unary).parameters),
            ['request_', 'rpc2_timeout', 'request', 'timeout'],
        )

        call = service.Unary.invoke(request=1, timeout=2, rpc2_timeout=0.01)
        self.assertEqual(
            self.server.receive().payload,
            request(request=1, timeout=2).SerializeToString(),
        )
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            call.response()
        self.assertEqual(context.exception.timeout, 0.01)

        # The rpc2_timeout field can still be set with a request message.
        service.Unary.invoke(request(rpc2_timeout=3))
        self.assertEqual(
            self.server.receive().payload,
            request(rpc2_timeout=3).SerializeToString(),
        )

        # write() takes no timeout, so every field is a keyword argument.
        with service.ClientStream() as stream:
            self.server.receive()
            self.assertEqual(
                list(inspect.signature(stream.write).parameters),
                ['request_', 'request', 'timeout', 'rpc2_timeout'],
            )
            stream.write(request=4, timeout=5, rpc2_timeout=6)
            self.assertEqual(
                self.server.receive().payload,
                request(
                    request=4, timeout=5, rpc2_timeout=6
                ).SerializeToString(),
            )

    def test_request_field_named_python_keyword(self) -> None:
        """Tests request fields whose names are Python keywords."""
        self.client.add_services(
            [
                _dynamic_file(
                    package='pw.rpc2.test.kw',
                    service_name='Keyword',
                    field_names=('from', 'val'),
                )
            ]
        )
        service = self.client.rpcs.pw.rpc2.test.kw.Keyword
        request = service.Unary.request

        self.assertEqual(
            list(inspect.signature(service.Unary).parameters),
            ['request', 'rpc2_timeout', 'val'],
        )
        service.Unary.invoke(**{'from': 7, 'val': 8})
        self.assertEqual(
            self.server.receive().payload,
            request(**{'from': 7, 'val': 8}).SerializeToString(),
        )

        with service.ClientStream() as stream:
            self.server.receive()
            self.assertEqual(
                list(inspect.signature(stream.write).parameters),
                ['request', 'val'],
            )
            stream.write(**{'from': 9})
            self.assertEqual(
                self.server.receive().payload,
                request(**{'from': 9}).SerializeToString(),
            )

    def test_server_streaming(self) -> None:
        server_stream = self.service.CountUpServerStream(val=10)
        self.assertIsInstance(server_stream, pw_rpc2.ServerStreamingCall)
        self.assertIsInstance(server_stream, pw_rpc2.Reader)
        self.assertNotIsInstance(server_stream, pw_rpc2.Writer)
        self.server.receive()
        self.server.send(
            Packet.server_message(server_stream._call_id, _response(11))
        )
        self.server.send(Packet.server_finish(server_stream._call_id))
        self.assertEqual([r.val for r in server_stream], [11])

    def test_client_streaming(self) -> None:
        client_stream = self.service.AccumulateClientStream()
        self.assertIsInstance(client_stream, pw_rpc2.ClientStreamingCall)
        self.assertIsInstance(client_stream, pw_rpc2.Writer)
        self.assertNotIsInstance(client_stream, pw_rpc2.Reader)
        self.server.receive()
        client_stream.write(val=3)
        self.server.receive()

        def respond() -> None:
            self.assertEqual(
                self.server.receive(),
                Packet.client_stream_end(client_stream._call_id),
            )
            self.server.send(
                Packet.response(client_stream._call_id, _response(3))
            )

        thread = threading.Thread(target=respond)
        thread.start()
        self.assertEqual(client_stream.finish().val, 3)
        thread.join()

    def test_bidirectional_streaming(self) -> None:
        with self.service.EchoBidiStream() as bidi_stream:
            self.assertIsInstance(
                bidi_stream, pw_rpc2.BidirectionalStreamingCall
            )
            self.assertIsInstance(bidi_stream, pw_rpc2.Reader)
            self.assertIsInstance(bidi_stream, pw_rpc2.Writer)
            self.assertEqual(
                self.server.receive().type, PacketType(PacketFlag.START)
            )
            bidi_stream.write(val=7)
            self.server.receive()
            self.server.send(
                Packet.server_message(bidi_stream._call_id, _response(7))
            )
            response = bidi_stream.read()
            assert response is not None
            self.assertEqual(response.val, 7)
        self.assertEqual(
            self.server.receive().type,
            PacketType(close_mode=CloseMode.ERROR_TERMINAL),
        )

    def test_bidirectional_reader_writer_threads(self) -> None:
        """Exercises sharing a bidi call as Reader and Writer across threads."""
        received: list[int] = []

        def send_requests(writer: pw_rpc2.Writer) -> None:
            writer.write(val=1)
            writer.write(val=2)

        def read_responses(reader: pw_rpc2.Reader) -> None:
            for message in reader:
                received.append(message.val)

        with self.service.EchoBidiStream() as call:
            self.server.receive()
            sender = threading.Thread(target=send_requests, args=(call,))
            receiver = threading.Thread(target=read_responses, args=(call,))
            sender.start()
            receiver.start()

            for _ in range(2):
                packet = self.server.receive()
                self.server.send(
                    Packet.server_message(call._call_id, packet.payload)
                )
            sender.join()
            call.finish()
            self.assertEqual(
                self.server.receive(), Packet.client_stream_end(call._call_id)
            )
            self.server.send(Packet.server_finish(call._call_id))
            receiver.join()

        self.assertEqual(received, [1, 2])

    def test_close_ends_calls_and_rejects_new_calls(self) -> None:
        call = self.service.EchoBidiStream()
        self.client.close()

        self.assertIs(call.status, RpcStatus.CLOSED)
        with self.assertRaises(pw_rpc2.SocketClosed):
            self.service.EchoUnary()


if __name__ == '__main__':
    unittest.main()
