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

import asyncio
import copy
import dataclasses
import inspect
import math
import threading
import time
from typing import Any, Callable
import unittest

from google.protobuf import descriptor_pb2, descriptor_pool
from google.protobuf.descriptor import FileDescriptor

import pw_rpc2
from pw_rpc2 import aio
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
import pw_transport

from fake_server import FakeServer

# pylint: disable=protected-access

Request = pw_rpc2_test_pb2.EchoRequest
Response = pw_rpc2_test_pb2.EchoResponse

_SERVICE_ID = 0xFE6FE65C
_UNARY_ID = 0x8BA299DF


def _response(val: int) -> bytes:
    return Response(val=val).SerializeToString()


async def _wait_until(
    condition: Callable[[], bool], timeout: float = 5.0
) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError('Timed out waiting for condition')
        await asyncio.sleep(0.001)


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


class AsyncClientTestCase(unittest.IsolatedAsyncioTestCase):
    """Base for tests with a connected asyncio client."""

    # pylint: disable=attribute-defined-outside-init

    async def asyncSetUp(self) -> None:
        self.server = FakeServer()
        self.client = await aio.Client.connect(
            self.server.socket, [pw_rpc2_test_pb2], default_timeout=5.0
        )
        self.server.expect_handshake()
        self.service = self.client.rpcs.pw.rpc2.test.TestEcho

    async def asyncTearDown(self) -> None:
        await self.client.close()

    async def receive(self) -> Packet:
        """Receives the next packet from the client without blocking."""
        return await asyncio.to_thread(self.server.receive)


class AsyncUnaryTest(AsyncClientTestCase):
    """Tests unary RPCs on the asyncio client."""

    async def test_unary(self) -> None:
        response = asyncio.ensure_future(self.service.EchoUnary(val=1))
        request = await self.receive()
        self.assertEqual(request.payload, Request(val=1).SerializeToString())
        self.server.send(Packet.response(request.call_id, _response(2)))
        self.assertEqual((await response).val, 2)

    async def test_unary_invoke(self) -> None:
        call = self.service.EchoUnary.invoke(val=1)
        self.assertIsInstance(call, aio.UnaryCall)
        self.server.send(Packet.response(call._call_id, _response(2)))
        r1, r2 = await asyncio.gather(call.response(), call.response())
        self.assertEqual(r1.val, 2)
        self.assertEqual(r2.val, 2)
        self.assertIs(call.status, RpcStatus.OK)

    async def test_unary_error(self) -> None:
        response = asyncio.ensure_future(self.service.EchoUnary())
        request = await self.receive()
        self.server.send(
            Packet.server_error(request.call_id, ProtocolError.UNKNOWN_SERVICE)
        )
        with self.assertRaises(pw_rpc2.RpcError) as context:
            await response
        self.assertIs(context.exception.status, RpcStatus.UNIMPLEMENTED)
        self.assertIs(
            context.exception._protocol_error, ProtocolError.UNKNOWN_SERVICE
        )

    async def test_unary_timeout_cancels(self) -> None:
        with self.assertRaises(pw_rpc2.RpcTimeout):
            await self.service.EchoUnary(rpc2_timeout=0.01)

        request = await self.receive()
        self.assertEqual(
            await self.receive(),
            Packet.client_error(request.call_id, ProtocolError.CANCELLED),
        )

    async def test_task_cancellation_cancels_rpc(self) -> None:
        task = asyncio.ensure_future(self.service.EchoUnary())
        request = await self.receive()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(
            await self.receive(),
            Packet.client_error(request.call_id, ProtocolError.CANCELLED),
        )

    async def test_unstarted_task_cancellation_does_not_start_rpc(self) -> None:
        """Cancelling a unary task before its first step sends nothing."""
        task = asyncio.create_task(self.service.EchoUnary())
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))
        self.assertEqual(self.client._connection._calls, {})

    async def test_closing_unstarted_unary_coroutine_does_not_start_rpc(
        self,
    ) -> None:
        coro = self.service.EchoUnary()
        self.assertTrue(inspect.iscoroutine(coro))
        coro.close()
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))
        self.assertEqual(self.client._connection._calls, {})

    async def test_closing_started_unary_coroutine_cancels_rpc(self) -> None:
        coro = self.service.EchoUnary()
        coro.send(None)
        request = await self.receive()
        coro.close()
        self.assertEqual(
            await self.receive(),
            Packet.client_error(request.call_id, ProtocolError.CANCELLED),
        )

    async def test_unary_validates_arguments_before_await(self) -> None:
        with self.assertRaises(ValueError):
            self.service.EchoUnary(nonexistent=1)
        with self.assertRaises(ValueError):
            self.service.EchoUnary(rpc2_timeout=-1)
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))

    async def test_wait_for_timeout_cancels_rpc(self) -> None:
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(self.service.EchoUnary(), 0.01)
        request = await self.receive()
        self.assertEqual(
            await self.receive(),
            Packet.client_error(request.call_id, ProtocolError.CANCELLED),
        )


class AsyncClientTest(AsyncClientTestCase):
    """Tests the asyncio client."""

    async def test_service_client(self) -> None:
        self.assertIs(pw_rpc2.aio, aio)
        self.assertIsInstance(self.service, pw_rpc2.ServiceClient)
        self.assertIsInstance(self.service.EchoUnary, aio.MethodClient)
        self.assertIs(self.service.EchoUnary.request, Request)
        self.assertIs(self.service.EchoUnary.response, Response)
        assert self.service.EchoUnary.__doc__ is not None
        self.assertIn('Unary RPC', self.service.EchoUnary.__doc__)
        self.assertEqual(len(list(self.client.methods())), 4)

    async def test_request_fields_named_like_arguments(self) -> None:
        self.client.add_services([_dynamic_file()])
        method = self.client.rpcs.pw.rpc2.test.dynamic.Dynamic.Unary

        self.assertEqual(
            list(inspect.signature(method).parameters),
            ['request_', 'rpc2_timeout', 'request', 'timeout'],
        )

        # request and timeout are request fields; rpc2_timeout is the timeout.
        call = method.invoke(request=1, timeout=2, rpc2_timeout=0.01)
        self.assertEqual(
            (await self.receive()).payload,
            method.request(request=1, timeout=2).SerializeToString(),
        )
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            await call.response()
        self.assertEqual(context.exception.timeout, 0.01)

    async def test_server_stream(self) -> None:
        call = self.service.CountUpServerStream(val=1)
        self.assertIsInstance(call, aio.ServerStreamingCall)
        self.assertIsInstance(call, aio.Reader)
        self.assertNotIsInstance(call, aio.Writer)
        for value in (1, 2):
            self.server.send(
                Packet.server_message(call._call_id, _response(value))
            )
        self.server.send(Packet.server_finish(call._call_id))

        self.assertEqual([r.val async for r in call], [1, 2])
        self.assertIs(call.status, RpcStatus.OK)

    async def test_server_stream_read_timeout(self) -> None:
        call = self.service.CountUpServerStream()
        with self.assertRaises(pw_rpc2.RpcTimeout):
            await call.read(timeout=0.01)
        self.assertFalse(call.completed)

    async def test_negative_default_timeout_rejected(self) -> None:
        with self.assertRaises(ValueError):
            self.client.default_timeout = -1
        self.assertEqual(self.client.default_timeout, 5.0)

    async def test_client_stream(self) -> None:
        async with self.service.AccumulateClientStream() as call:
            self.assertIsInstance(call, aio.ClientStreamingCall)
            self.assertIsInstance(call, aio.Writer)
            self.assertNotIsInstance(call, aio.Reader)
            self.assertEqual(
                list(inspect.signature(call.write).parameters),
                ['request', 'val'],
            )
            self.assertTrue(inspect.iscoroutinefunction(call.write))
            await call.write(val=1)
            await call.write(val=2)
            finish = asyncio.ensure_future(call.finish())

            packets = [await self.receive() for _ in range(4)]
            self.assertEqual(
                [p.type for p in packets],
                [
                    PacketType(PacketFlag.START),
                    PacketType(PacketFlag.HAS_PAYLOAD),
                    PacketType(PacketFlag.HAS_PAYLOAD),
                    PacketType(close_mode=CloseMode.STREAM_END),
                ],
            )
            self.server.send(Packet.response(call._call_id, _response(3)))
            self.assertEqual((await finish).val, 3)

    async def test_bidi_stream(self) -> None:
        async with self.service.EchoBidiStream() as call:
            self.assertIsInstance(call, aio.BidirectionalStreamingCall)
            self.assertIsInstance(call, aio.Reader)
            self.assertIsInstance(call, aio.Writer)
            await call.write(val=5)
            await self.receive()
            packet = await self.receive()
            self.server.send(
                Packet.server_message(packet.call_id, packet.payload)
            )
            response = await call.read()
            assert response is not None
            self.assertEqual(response.val, 5)
            await call.finish()
            self.assertEqual(
                await self.receive(), Packet.client_stream_end(call._call_id)
            )
            with self.assertRaises(pw_rpc2.StreamClosedError) as context:
                await call.finish()
            self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)

        self.assertIs(call.status, RpcStatus.CANCELLED)
        self.assertEqual(
            (await self.receive()).type,
            PacketType(close_mode=CloseMode.ERROR_TERMINAL),
        )

    async def test_bidi_reader_writer_tasks(self) -> None:
        """Exercises sharing a bidi call as Reader and Writer across tasks."""
        received: list[int] = []

        async def send_requests(writer: aio.Writer) -> None:
            await writer.write(val=1)
            await writer.write(val=2)

        async def read_responses(reader: aio.Reader) -> None:
            async for message in reader:
                received.append(message.val)

        async with self.service.EchoBidiStream() as call:
            await self.receive()
            sender = asyncio.create_task(send_requests(call))
            receiver = asyncio.create_task(read_responses(call))

            for _ in range(2):
                packet = await self.receive()
                self.server.send(
                    Packet.server_message(call._call_id, packet.payload)
                )
            await sender
            await call.finish()
            self.assertEqual(
                await self.receive(), Packet.client_stream_end(call._call_id)
            )
            self.server.send(Packet.server_finish(call._call_id))
            await receiver

        self.assertEqual(received, [1, 2])

    async def test_cancel(self) -> None:
        call = self.service.CountUpServerStream()
        self.assertTrue(call.cancel())
        with self.assertRaises(pw_rpc2.RpcError) as context:
            await call.read()
        self.assertIs(context.exception.status, RpcStatus.CANCELLED)

    async def test_disconnect(self) -> None:
        call = self.service.CountUpServerStream()
        await self.receive()
        self.server.disconnect()
        with self.assertRaises(pw_rpc2.SocketClosed):
            await call.read()
        self.assertTrue(self.client.closed)

    async def test_close(self) -> None:
        call = self.service.EchoBidiStream()
        await self.client.close()
        with self.assertRaises(pw_rpc2.RpcError) as context:
            await call.read()
        self.assertIs(context.exception.status, RpcStatus.CLOSED)
        with self.assertRaises(pw_rpc2.RpcError):
            await call.write(val=1)
        with self.assertRaises(pw_rpc2.StreamClosedError):
            await call.finish()
        with self.assertRaises(pw_rpc2.SocketClosed):
            self.service.EchoUnary.invoke()

    async def test_invoke_while_closing_raises_socket_closed(self) -> None:
        """Covers the sender stopping between registering and sending a call."""
        self.client._sender.shut_down()
        with self.assertRaisesRegex(pw_rpc2.SocketClosed, 'closed'):
            self.service.EchoUnary.invoke()
        self.assertEqual(self.client._connection._calls, {})
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))

    async def test_threads_are_daemons(self) -> None:
        """An unclosed client must not keep the interpreter from exiting."""
        threads = [
            thread
            for thread in threading.enumerate()
            if thread.name.startswith('pw_rpc2')
        ]
        self.assertIn('pw_rpc2 sender', [thread.name for thread in threads])
        self.assertEqual([t.name for t in threads if not t.daemon], [])

    async def test_cancelled_read_does_not_cancel_rpc(self) -> None:
        call = self.service.CountUpServerStream()
        await self.receive()
        read = asyncio.create_task(call.read())
        await _wait_until(lambda: bool(call._state._async_waiters))
        read.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await read

        self.assertFalse(call.completed)
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        response = await call.read()
        assert response is not None
        self.assertEqual(response.val, 1)

    async def test_many_concurrent_calls(self) -> None:
        responses = [
            asyncio.ensure_future(self.service.EchoUnary()) for _ in range(20)
        ]
        for _ in range(20):
            request = await self.receive()
            self.server.send(
                Packet.response(request.call_id, _response(request.call_id))
            )
        values = [r.val for r in await asyncio.gather(*responses)]
        self.assertEqual(values, list(range(1, 21)))

    async def test_client_stream_early_response_and_repeated_finish(
        self,
    ) -> None:
        async with self.service.AccumulateClientStream() as call:
            self.assertEqual(
                (await self.receive()).type, PacketType(PacketFlag.START)
            )
            self.server.send(Packet.response(call._call_id, _response(7)))
            await _wait_until(lambda: call.completed)
            self.assertEqual((await call.finish()).val, 7)
            self.assertEqual((await call.finish()).val, 7)
            self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))

    async def test_client_stream_finish_negative_timeout_does_not_send(
        self,
    ) -> None:
        async with self.service.AccumulateClientStream() as call:
            self.assertEqual(
                (await self.receive()).type, PacketType(PacketFlag.START)
            )
            with self.assertRaises(ValueError):
                await call.finish(timeout=-1)
            self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))
            self.assertFalse(call.completed)

            finish = asyncio.ensure_future(call.finish())
            self.assertEqual(
                await self.receive(), Packet.client_stream_end(call._call_id)
            )
            self.server.send(Packet.response(call._call_id, _response(4)))
            self.assertEqual((await finish).val, 4)

    async def test_invoke_oversized_request_raises_value_error(self) -> None:
        server = FakeServer(max_packet_size=12)
        async with await aio.Client.connect(
            server.socket, [pw_rpc2_test_pb2], default_timeout=5.0
        ) as client:
            server.expect_handshake()
            service = client.rpcs.pw.rpc2.test.TestEcho
            with self.assertRaises(ValueError):
                service.EchoUnary.invoke(val=1)
            self.assertTrue(await asyncio.to_thread(server.nothing_sent))

    async def test_invoke_background_send_failure_reports_error(self) -> None:
        server = FakeServer()
        socket = _FailingWriteSocket(server.socket)
        async with await aio.Client.connect(
            socket, [pw_rpc2_test_pb2], default_timeout=5.0
        ) as client:
            server.expect_handshake()
            socket.write_error = pw_transport.SocketClosed('write failed')
            call = client.rpcs.pw.rpc2.test.TestEcho.EchoUnary.invoke(val=1)
            with self.assertRaisesRegex(pw_rpc2.SocketClosed, 'write failed'):
                await call.response()


class AsyncBackpressureTest(unittest.IsolatedAsyncioTestCase):
    """Tests the asyncio client with a write blocked on transport backpressure.

    The sender thread is blocked sending ``val=2`` on :py:attr:`call`, behind
    the packet that started the call and ``val=1``.
    """

    # pylint: disable=attribute-defined-outside-init

    async def asyncSetUp(self) -> None:
        # Queue depth 2 lets the handshake (SYN + ACK) complete before
        # expect_handshake() drains it, then fills after START + one write.
        self.server = FakeServer(queue_depth=2)
        self.client = await aio.Client.connect(
            self.server.socket, [pw_rpc2_test_pb2], default_timeout=5.0
        )
        self.server.expect_handshake()
        self.service = self.client.rpcs.pw.rpc2.test.TestEcho
        self.call = self.service.EchoBidiStream()
        await self.call.write(val=1)
        # The queue is now full (START + val=1); the next write blocks on the
        # sender thread.
        self.blocked_write = asyncio.create_task(self.call.write(val=2))
        await _wait_until(lambda: self.call._state._active_sends > 0)

    async def asyncTearDown(self) -> None:
        await self.client.close()
        await asyncio.gather(self.blocked_write, return_exceptions=True)

    async def receive(self) -> Packet:
        """Receives the next packet from the client without blocking."""
        return await asyncio.to_thread(self.server.receive)

    async def receive_blocked_packets(self) -> None:
        """Receives the packets that were sent or blocked during setup."""
        self.assertEqual(
            (await self.receive()).type, PacketType(PacketFlag.START)
        )
        for value in (1, 2):
            self.assertEqual(
                (await self.receive()).payload,
                Request(val=value).SerializeToString(),
            )

    async def test_does_not_block_event_loop(self) -> None:
        """Verifies transport backpressure does not stall the event loop."""
        # Other event loop tasks still run while writes are backpressured.
        progress = False

        async def other_task() -> None:
            nonlocal progress
            await asyncio.sleep(0.01)
            progress = True

        await other_task()
        self.assertTrue(progress)
        self.assertFalse(self.blocked_write.done())

        # Cancelling completes the call immediately without waiting on the
        # blocked write.
        self.assertTrue(self.call.cancel())
        self.assertIs(self.call.status, RpcStatus.CANCELLED)

        # Draining the queue unblocks the sender thread and lets the
        # deferred CANCELLED packet follow.
        await self.receive_blocked_packets()
        await self.blocked_write
        self.assertEqual(
            await self.receive(),
            Packet.client_error(self.call._call_id, ProtocolError.CANCELLED),
        )

    async def test_call_that_ends_before_its_request_is_sent_is_not_started(
        self,
    ) -> None:
        with self.assertRaises(pw_rpc2.RpcTimeout):
            await self.service.EchoUnary(rpc2_timeout=0.01)
        self.assertTrue(self.service.CountUpServerStream().cancel())

        await self.receive_blocked_packets()
        # The sender runs in order, so it has handled the requests of the calls
        # that ended by the time it sends a later write.
        later_write = asyncio.create_task(self.call.write(val=3))
        self.assertEqual(
            (await self.receive()).payload, Request(val=3).SerializeToString()
        )
        await later_write
        self.assertTrue(await asyncio.to_thread(self.server.nothing_sent))
        self.assertEqual(
            list(self.client._connection._calls), [self.call._call_id]
        )

    async def test_cancelled_write_in_progress_is_still_sent(self) -> None:
        self.blocked_write.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await self.blocked_write
        self.assertFalse(self.call.completed)

        await self.receive_blocked_packets()

    async def test_cancelled_queued_write_is_not_sent(self) -> None:
        queued_write = asyncio.create_task(self.call.write(val=3))
        await asyncio.sleep(0)  # Let the task queue its write.
        queued_write.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await queued_write

        await self.receive_blocked_packets()
        await self.call.write(val=4)
        self.assertEqual(
            (await self.receive()).payload, Request(val=4).SerializeToString()
        )


class _FailingWriteSocket(pw_transport.ReliableSocket):
    """Wraps a ReliableSocket and fails writes once armed."""

    def __init__(self, inner: pw_transport.ReliableSocket) -> None:
        self._inner = inner
        self.write_error: Exception | None = None

    @property
    def closed(self) -> bool:
        return self._inner.closed

    @property
    def max_write_message_size(self) -> int | None:
        return self._inner.max_write_message_size

    def _read(self, deadline: pw_transport.Deadline) -> bytes:
        return self._inner.read(timeout=deadline.remaining())

    def _write(self, data: bytes, deadline: pw_transport.Deadline) -> None:
        if self.write_error is not None:
            raise self.write_error
        self._inner.write(data, timeout=deadline.remaining())

    def close(self) -> None:
        self._inner.close()


class AsyncHandshakeTest(unittest.IsolatedAsyncioTestCase):
    """Tests connecting the asyncio client."""

    async def test_handshake_failure(self) -> None:
        server = FakeServer(handshake_reply=None)
        with self.assertRaises(pw_rpc2.HandshakeError) as context:
            await aio.Client.connect(server.socket, handshake_timeout=0.01)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(server.socket.closed)

    async def test_connect_with_callable(self) -> None:
        server = FakeServer()
        calls = 0

        def connect() -> pw_transport.ReliableSocket:
            nonlocal calls
            calls += 1
            return server.socket

        async with await aio.Client.connect(connect) as client:
            self.assertEqual(calls, 1)
            server.expect_handshake()
            self.assertFalse(client.closed)
        self.assertTrue(server.socket.closed)

    async def test_negative_default_timeout_closes_socket(self) -> None:
        server = FakeServer()
        with self.assertRaises(ValueError):
            await aio.Client.connect(server.socket, default_timeout=-1)
        self.assertTrue(server.socket.closed)
        self.assertTrue(server.nothing_sent())

        called = False

        def connect() -> pw_transport.ReliableSocket:
            nonlocal called
            called = True
            return FakeServer().socket

        with self.assertRaises(ValueError):
            await aio.Client.connect(connect, default_timeout=-1)
        self.assertFalse(called)

    async def test_invalid_protos_close_socket(self) -> None:
        server = FakeServer()
        bad: list[Any] = ['not a proto']
        with self.assertRaises(TypeError):
            await aio.Client.connect(server.socket, bad)
        self.assertTrue(server.socket.closed)
        self.assertTrue(server.nothing_sent())

        called = False

        def connect() -> pw_transport.ReliableSocket:
            nonlocal called
            called = True
            return FakeServer().socket

        with self.assertRaises(TypeError):
            await aio.Client.connect(connect, bad)
        self.assertFalse(called)

    async def test_colliding_protos_rejected_before_connecting(self) -> None:
        colliding = [
            _dynamic_file(package='a', service_name='b'),
            _dynamic_file(package='a.b', service_name='C'),
        ]
        server = FakeServer()
        with self.assertRaisesRegex(ValueError, 'service and a package'):
            await aio.Client.connect(server.socket, colliding)
        self.assertTrue(server.socket.closed)
        self.assertTrue(server.nothing_sent())

        called = False

        def connect() -> pw_transport.ReliableSocket:
            nonlocal called
            called = True
            return FakeServer().socket

        with self.assertRaisesRegex(ValueError, 'service and a package'):
            await aio.Client.connect(connect, colliding)
        self.assertFalse(called)

    async def test_cancelled_connect_closes_opened_socket(self) -> None:
        server = FakeServer()
        started = threading.Event()
        release = threading.Event()

        def slow_connect() -> pw_transport.ReliableSocket:
            started.set()
            assert release.wait(timeout=5.0)
            return server.socket

        task = asyncio.create_task(aio.Client.connect(slow_connect))
        self.assertTrue(await asyncio.to_thread(started.wait, 5.0))
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task

        release.set()
        # Wait for the background worker to finish and close the socket.
        await _wait_until(lambda: server.socket.closed)
        self.assertTrue(server.socket.closed)

    async def test_cancelled_connect_during_handshake_closes_socket(
        self,
    ) -> None:
        server = FakeServer(handshake_reply=None)
        threads = set(threading.enumerate())
        task = asyncio.create_task(
            aio.Client.connect(server.socket, handshake_timeout=math.inf)
        )
        # The client sends SYN, then waits for a reply that never arrives.
        self.assertEqual(
            await asyncio.to_thread(server.receive_raw),
            Handshake(HandshakeType.SYN).encode(),
        )
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task

        self.assertTrue(server.socket.closed)
        await _wait_until(
            lambda: not any(
                thread.name.startswith('pw_rpc2') and thread not in threads
                for thread in threading.enumerate()
            )
        )

    async def test_no_public_constructor(self) -> None:
        with self.assertRaisesRegex(TypeError, 'connect'):
            aio.Client()

    async def test_context_manager(self) -> None:
        server = FakeServer()
        async with await aio.Client.connect(server.socket) as client:
            self.assertFalse(client.closed)
        self.assertTrue(client.closed)


if __name__ == '__main__':
    unittest.main()
