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
"""Tests creating pw_rpc2 service descriptions from protobuf descriptors."""

import inspect
from typing import Any
import unittest
from unittest import mock

from google.protobuf import descriptor_pb2, descriptor_pool

from pw_rpc2 import _descriptors as descriptors
from pw_rpc2._descriptors import MethodType, Service, rpc_id
from pw_rpc2_descriptors_test_pb import (
    descriptors_test_proto2_pb2,
    descriptors_test_proto3_pb2,
)
from pw_rpc2_test_pb import pw_rpc2_test_pb2

# pylint: disable=protected-access

_SERVICE = 'pw.rpc2.test.TestEcho'

_ECHO_DESCRIPTION = """\
Unary RPC pw.rpc2.test.TestEcho.EchoUnary(EchoRequest) returns (EchoResponse).

Request fields (pw.rpc2.test.EchoRequest):
  val: int = 0

Response fields (pw.rpc2.test.EchoResponse):
  val: int"""

_PROTO3_DESCRIPTION = """\
Unary RPC pw.rpc2.test.proto3.Service.Call(TestMessage) returns (TestMessage).

Request fields (pw.rpc2.test.proto3.TestMessage):
  query: str = ''
  data: bytes = b''
  ratio: float = 0.0
  flag: bool = False
  kind: pw.rpc2.test.proto3.Kind = 0
  oneof target:
    target_id: int
    target_name: str
  optional limit: int
  optional sub: pw.rpc2.test.proto3.SubMessage
  tags: Iterable[str] = []

Response fields (pw.rpc2.test.proto3.TestMessage):
  query: str
  data: bytes
  ratio: float
  flag: bool
  kind: pw.rpc2.test.proto3.Kind
  oneof target:
    target_id: int
    target_name: str
  optional limit: int
  optional sub: pw.rpc2.test.proto3.SubMessage
  tags: Iterable[str]"""

_PROTO2_DESCRIPTION = """\
Unary RPC pw.rpc2.test.proto2.Service.Call(TestMessage) returns (TestMessage).

Request fields (pw.rpc2.test.proto2.TestMessage):
  required host: str
  optional tls: bool
  optional port: int = 8080
  oneof endpoint:
    socket_id: int
    env: str = 'dev'

Response fields (pw.rpc2.test.proto2.TestMessage):
  required host: str
  optional tls: bool
  optional port: int
  oneof endpoint:
    socket_id: int
    env: str"""


def dynamic_test_service(*extra_methods: str) -> descriptor_pool.DescriptorPool:
    """Loads pw_rpc2_test.proto into a new pool, with extra unary methods.

    This mimics loading descriptors at runtime, rather than from generated
    modules.
    """
    file_proto = descriptor_pb2.FileDescriptorProto()
    pw_rpc2_test_pb2.DESCRIPTOR.CopyToProto(file_proto)

    (service,) = [s for s in file_proto.service if s.name == 'TestEcho']
    for name in extra_methods:
        service.method.add(
            name=name,
            input_type='.pw.rpc2.test.EchoRequest',
            output_type='.pw.rpc2.test.EchoResponse',
        )

    pool = descriptor_pool.DescriptorPool()
    pool.Add(
        descriptor_pb2.FileDescriptorProto.FromString(
            file_proto.SerializeToString()
        )
    )
    return pool


class RpcIdTest(unittest.TestCase):
    """Tests calculating service and method IDs."""

    def test_matches_generated_cpp_ids(self) -> None:
        self.assertEqual(rpc_id(_SERVICE), 0xFE6FE65C)
        self.assertEqual(rpc_id('EchoUnary'), 0x8BA299DF)
        self.assertEqual(rpc_id('CountUpServerStream'), 0xC9AB4726)
        self.assertEqual(rpc_id('AccumulateClientStream'), 0xA0C83643)
        self.assertEqual(rpc_id('EchoBidiStream'), 0xAEB7023F)

    def test_empty_name(self) -> None:
        self.assertEqual(rpc_id(''), 0)


class ServicesTest(unittest.TestCase):
    """Tests finding services in protos."""

    def test_from_module(self) -> None:
        names = [s.full_name for s in descriptors.services([pw_rpc2_test_pb2])]
        self.assertEqual(names, [_SERVICE])

    def test_from_descriptors(self) -> None:
        file = pw_rpc2_test_pb2.DESCRIPTOR
        service = file.services_by_name['TestEcho']

        found = list(descriptors.services([file, service]))

        self.assertEqual([s.full_name for s in found], [_SERVICE] * 2)

    def test_rejects_other_objects(self) -> None:
        with self.assertRaises(TypeError):
            bad: list[Any] = [_SERVICE]
            list(descriptors.services(bad))

    def test_from_dynamic_pool(self) -> None:
        pool = dynamic_test_service('Extra')
        service = Service.from_descriptor(pool.FindServiceByName(_SERVICE))

        self.assertIn('Extra', service.methods)
        self.assertEqual(service._id, 0xFE6FE65C)
        method = service.methods['Extra']
        self.assertEqual(
            method.request_type.DESCRIPTOR.full_name,
            'pw.rpc2.test.EchoRequest',
        )
        self.assertIsNot(method.request_type, pw_rpc2_test_pb2.EchoRequest)


class ServiceTest(unittest.TestCase):
    """Tests Service and Method."""

    def setUp(self) -> None:
        (self.service,) = list(descriptors.services([pw_rpc2_test_pb2]))

    def test_service(self) -> None:
        self.assertEqual(self.service.name, 'TestEcho')
        self.assertEqual(self.service.full_name, _SERVICE)
        self.assertEqual(self.service.package, 'pw.rpc2.test')
        self.assertEqual(self.service._id, 0xFE6FE65C)
        self.assertEqual(str(self.service), _SERVICE)
        self.assertEqual(
            list(self.service.methods),
            [
                'EchoUnary',
                'CountUpServerStream',
                'AccumulateClientStream',
                'EchoBidiStream',
            ],
        )

    def test_method_types(self) -> None:
        methods = self.service.methods
        self.assertIs(methods['EchoUnary'].type, MethodType.UNARY)
        self.assertIs(
            methods['CountUpServerStream'].type, MethodType.SERVER_STREAMING
        )
        self.assertIs(
            methods['AccumulateClientStream'].type, MethodType.CLIENT_STREAMING
        )
        self.assertIs(
            methods['EchoBidiStream'].type, MethodType.BIDIRECTIONAL_STREAMING
        )

    def test_method(self) -> None:
        method = self.service.methods['EchoUnary']
        self.assertIs(method.service, self.service)
        self.assertEqual(method._id, 0x8BA299DF)
        self.assertEqual(method.name, 'EchoUnary')
        self.assertEqual(method.full_name, f'{_SERVICE}.EchoUnary')
        self.assertIs(method.request_type, pw_rpc2_test_pb2.EchoRequest)
        self.assertIs(method.response_type, pw_rpc2_test_pb2.EchoResponse)

    def test_method_str(self) -> None:
        methods = self.service.methods
        self.assertEqual(
            str(methods['EchoUnary']),
            'pw.rpc2.test.TestEcho.EchoUnary(EchoRequest) returns '
            '(EchoResponse)',
        )
        self.assertEqual(
            str(methods['EchoBidiStream']),
            'pw.rpc2.test.TestEcho.EchoBidiStream(stream EchoRequest) returns '
            '(stream EchoResponse)',
        )

    def test_method_id_collision(self) -> None:
        with mock.patch.object(descriptors, 'rpc_id', return_value=1):
            with self.assertRaisesRegex(ValueError, 'same ID'):
                Service.from_descriptor(self.service.descriptor)


class RequestTest(unittest.TestCase):
    """Tests building requests."""

    def setUp(self) -> None:
        (service,) = list(descriptors.services([pw_rpc2_test_pb2]))
        self.method = service.methods['EchoUnary']
        (p3_service,) = list(
            descriptors.services([descriptors_test_proto3_pb2])
        )
        self.proto3_method = p3_service.methods['Call']
        (p2_service,) = list(
            descriptors.services([descriptors_test_proto2_pb2])
        )
        self.proto2_method = p2_service.methods['Call']

    def test_from_fields(self) -> None:
        self.assertEqual(
            self.method.request(fields={'val': 3}),
            pw_rpc2_test_pb2.EchoRequest(val=3),
        )

    def test_default(self) -> None:
        self.assertEqual(self.method.request(), pw_rpc2_test_pb2.EchoRequest())

    def test_from_message(self) -> None:
        request = pw_rpc2_test_pb2.EchoRequest(val=5)
        self.assertIs(self.method.request(request), request)

    def test_from_message_in_another_pool(self) -> None:
        pool = dynamic_test_service()
        (dynamic,) = list(
            descriptors.services([pool.FindServiceByName(_SERVICE)])
        )
        request = pw_rpc2_test_pb2.EchoRequest(val=5)
        self.assertIs(dynamic.methods['EchoUnary'].request(request), request)

    def test_wrong_message_type(self) -> None:
        with self.assertRaisesRegex(TypeError, 'pw.rpc2.test.EchoRequest'):
            self.method.request(pw_rpc2_test_pb2.EchoResponse())
        with self.assertRaisesRegex(TypeError, 'pw.rpc2.test.EchoRequest'):
            bad: Any = pw_rpc2_test_pb2.EchoRequest
            self.method.request(bad)

    def test_message_and_fields(self) -> None:
        with self.assertRaises(TypeError):
            self.method.request(pw_rpc2_test_pb2.EchoRequest(), {'val': 1})

    def test_unknown_field(self) -> None:
        with self.assertRaises(ValueError):
            self.method.request(fields={'not_a_field': 1})

    def test_request_signature(self) -> None:
        signature = self.method.request_signature()
        self.assertEqual(list(signature.parameters), ['request', 'val'])
        request = signature.parameters['request']
        self.assertIs(request.kind, inspect.Parameter.POSITIONAL_ONLY)
        val = signature.parameters['val']
        self.assertIs(val.kind, inspect.Parameter.KEYWORD_ONLY)
        self.assertEqual(val.default, 0)
        self.assertEqual(val.annotation, 'int')

    def test_request_signature_annotations(self) -> None:
        params = self.proto3_method.request_signature().parameters
        self.assertEqual(params['query'].annotation, 'str')
        self.assertEqual(params['data'].annotation, 'bytes')
        self.assertEqual(params['ratio'].annotation, 'float')
        self.assertEqual(params['flag'].annotation, 'bool')
        self.assertEqual(params['kind'].annotation, 'pw.rpc2.test.proto3.Kind')
        self.assertEqual(params['target_id'].annotation, 'int')
        self.assertEqual(params['target_name'].annotation, 'str')
        self.assertEqual(params['limit'].annotation, 'int')
        self.assertEqual(
            params['sub'].annotation, 'pw.rpc2.test.proto3.SubMessage'
        )
        self.assertEqual(params['tags'].annotation, 'Iterable[str]')

    def test_describe(self) -> None:
        self.assertEqual(self.method.describe(), _ECHO_DESCRIPTION)

    def test_request_signature_defaults(self) -> None:
        """Tests parameter default values for proto3 and proto2 fields."""
        p3_params = self.proto3_method.request_signature().parameters
        # proto3 implicit presence
        self.assertEqual(p3_params['query'].default, '')
        self.assertEqual(p3_params['data'].default, b'')
        self.assertEqual(p3_params['ratio'].default, 0.0)
        self.assertEqual(p3_params['flag'].default, False)
        self.assertEqual(p3_params['kind'].default, 0)
        self.assertEqual(
            self.method.request_signature().parameters['val'].default, 0
        )
        # proto3 optional
        self.assertIsNone(p3_params['limit'].default)
        # proto3 message and repeated
        self.assertIsNone(p3_params['sub'].default)
        self.assertIsNone(p3_params['tags'].default)
        # real oneof fields
        self.assertIsNone(p3_params['target_id'].default)
        self.assertIsNone(p3_params['target_name'].default)

        p2_params = self.proto2_method.request_signature().parameters
        # proto2 required
        self.assertIs(p2_params['host'].default, inspect.Parameter.empty)
        # proto2 optional without explicit default
        self.assertIsNone(p2_params['tls'].default)
        # proto2 optional with explicit default
        self.assertIsNone(p2_params['port'].default)
        # proto2 oneof fields
        self.assertIsNone(p2_params['socket_id'].default)
        self.assertIsNone(p2_params['env'].default)

    def test_describe_proto3(self) -> None:
        self.assertEqual(self.proto3_method.describe(), _PROTO3_DESCRIPTION)

    def test_describe_proto2(self) -> None:
        self.assertEqual(self.proto2_method.describe(), _PROTO2_DESCRIPTION)


if __name__ == '__main__':
    unittest.main()
