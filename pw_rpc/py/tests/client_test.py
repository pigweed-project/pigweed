#!/usr/bin/env python3
# Copyright 2020 The Pigweed Authors
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
"""Tests creating pw_rpc client."""

import unittest
from typing import Any, Callable

from pw_protobuf_compiler import python_protos
from pw_status import Status

from pw_rpc import callback_client, client, packets, query_pb2
import pw_rpc.ids
from pw_rpc.internal.packet_pb2 import PacketType, RpcPacket
from pw_rpc.descriptors import RpcIds

TEST_PROTO_1 = """\
syntax = "proto3";

package pw.test1;

message SomeMessage {
  uint32 magic_number = 1;
}

message AnotherMessage {
  enum Result {
    FAILED = 0;
    FAILED_MISERABLY = 1;
    I_DONT_WANT_TO_TALK_ABOUT_IT = 2;
  }

  Result result = 1;
  string payload = 2;
}

service PublicService {
  rpc SomeUnary(SomeMessage) returns (AnotherMessage) {}
  rpc SomeServerStreaming(SomeMessage) returns (stream AnotherMessage) {}
  rpc SomeClientStreaming(stream SomeMessage) returns (AnotherMessage) {}
  rpc SomeBidiStreaming(stream SomeMessage) returns (stream AnotherMessage) {}
}
"""

TEST_PROTO_2 = """\
syntax = "proto2";

package pw.test2;

message Request {
  optional float magic_number = 1;
}

message Response {
}

service Alpha {
  rpc Unary(Request) returns (Response) {}
}

service Bravo {
  rpc BidiStreaming(stream Request) returns (stream Response) {}
}
"""

PROTOS = python_protos.Library.from_strings([TEST_PROTO_1, TEST_PROTO_2])

SOME_CHANNEL_ID: int = 237
SOME_SERVICE_ID: int = 193
SOME_METHOD_ID: int = 769
SOME_CALL_ID: int = 452

CLIENT_FIRST_CHANNEL_ID: int = 557
CLIENT_SECOND_CHANNEL_ID: int = 474


def create_client(
    proto_modules: Any,
    first_channel_output_fn: Callable[[bytes], Any] = lambda _: None,
) -> client.Client:
    return client.Client.from_modules(
        callback_client.Impl(),
        [
            client.Channel(CLIENT_FIRST_CHANNEL_ID, first_channel_output_fn),
            client.Channel(CLIENT_SECOND_CHANNEL_ID, lambda _: None),
        ],
        proto_modules,
    )


class ChannelClientTest(unittest.TestCase):
    """Tests the ChannelClient."""

    def setUp(self) -> None:
        client_instance = create_client(PROTOS.modules())
        self._channel_client: client.ChannelClient = client_instance.channel(
            CLIENT_FIRST_CHANNEL_ID
        )

    def test_access_service_client_as_attribute_or_index(self) -> None:
        self.assertIs(
            self._channel_client.rpcs.pw.test1.PublicService,
            self._channel_client.rpcs['pw.test1.PublicService'],
        )
        self.assertIs(
            self._channel_client.rpcs.pw.test1.PublicService,
            self._channel_client.rpcs[
                pw_rpc.ids.calculate('pw.test1.PublicService')
            ],
        )

    def test_access_method_client_as_attribute_or_index(self) -> None:
        self.assertIs(
            self._channel_client.rpcs.pw.test2.Alpha.Unary,
            self._channel_client.rpcs['pw.test2.Alpha']['Unary'],
        )
        self.assertIs(
            self._channel_client.rpcs.pw.test2.Alpha.Unary,
            self._channel_client.rpcs['pw.test2.Alpha'][
                pw_rpc.ids.calculate('Unary')
            ],
        )

    def test_service_name(self) -> None:
        self.assertEqual(
            self._channel_client.rpcs.pw.test2.Alpha.Unary.service.name, 'Alpha'
        )
        self.assertEqual(
            self._channel_client.rpcs.pw.test2.Alpha.Unary.service.full_name,
            'pw.test2.Alpha',
        )

    def test_method_name(self) -> None:
        self.assertEqual(
            self._channel_client.rpcs.pw.test2.Alpha.Unary.method.name, 'Unary'
        )
        self.assertEqual(
            self._channel_client.rpcs.pw.test2.Alpha.Unary.method.full_name,
            'pw.test2.Alpha.Unary',
        )

    def test_iterate_over_all_methods(self) -> None:
        channel_client = self._channel_client
        all_methods = {
            channel_client.rpcs.pw.test1.PublicService.SomeUnary,
            channel_client.rpcs.pw.test1.PublicService.SomeServerStreaming,
            channel_client.rpcs.pw.test1.PublicService.SomeClientStreaming,
            channel_client.rpcs.pw.test1.PublicService.SomeBidiStreaming,
            channel_client.rpcs.pw.test2.Alpha.Unary,
            channel_client.rpcs.pw.test2.Bravo.BidiStreaming,
        }
        self.assertEqual(set(channel_client.methods()), all_methods)

    def test_check_for_presence_of_services(self) -> None:
        self.assertIn('pw.test1.PublicService', self._channel_client.rpcs)
        self.assertIn(
            pw_rpc.ids.calculate('pw.test1.PublicService'),
            self._channel_client.rpcs,
        )

    def test_check_for_presence_of_missing_services(self) -> None:
        self.assertNotIn('PublicService', self._channel_client.rpcs)
        self.assertNotIn('NotAService', self._channel_client.rpcs)
        self.assertNotIn(-1213, self._channel_client.rpcs)

    def test_check_for_presence_of_methods(self) -> None:
        service = self._channel_client.rpcs.pw.test1.PublicService
        self.assertIn('SomeUnary', service)
        self.assertIn(pw_rpc.ids.calculate('SomeUnary'), service)

    def test_check_for_presence_of_missing_methods(self) -> None:
        service = self._channel_client.rpcs.pw.test1.PublicService
        self.assertNotIn('Some', service)
        self.assertNotIn('Unary', service)
        self.assertNotIn(12345, service)

    def test_method_fully_qualified_name(self) -> None:
        self.assertIs(
            self._channel_client.method('pw.test2.Alpha/Unary'),
            self._channel_client.rpcs.pw.test2.Alpha.Unary,
        )
        self.assertIs(
            self._channel_client.method('pw.test2.Alpha.Unary'),
            self._channel_client.rpcs.pw.test2.Alpha.Unary,
        )

    def test_service_client_properties(self) -> None:
        service = self._channel_client.rpcs.pw.test1.PublicService
        self.assertEqual(service.name, 'PublicService')
        self.assertEqual(service.full_name, 'pw.test1.PublicService')

    def test_service_client_method_ids_and_names(self) -> None:
        # pylint: disable=protected-access
        service = self._channel_client.rpcs.pw.test1.PublicService
        method_id = pw_rpc.ids.calculate('SomeUnary')
        self.assertEqual(service._method_name(method_id), 'SomeUnary')
        self.assertEqual(service._method_names[method_id], 'SomeUnary')

    def test_query_services_fails_when_query_service_not_present(self) -> None:
        with self.assertRaises(client.Error):
            self._channel_client.query_services()

    def test_is_service_available_fails_when_query_service_not_present(
        self,
    ) -> None:
        with self.assertRaises(client.Error):
            self._channel_client.is_service_available('pw.test1.PublicService')

    def test_is_method_available_fails_when_query_service_not_present(
        self,
    ) -> None:
        with self.assertRaises(client.Error):
            self._channel_client.is_method_available(
                'pw.test1.PublicService', 'SomeUnary'
            )


class ClientTest(unittest.TestCase):
    """Tests the pw_rpc Client independently of the ClientImpl."""

    def setUp(self) -> None:
        self._last_packet_sent_bytes: bytes | None = None
        self._client = create_client(PROTOS.modules(), self._save_packet)

    def _save_packet(self, packet) -> None:
        self._last_packet_sent_bytes = packet

    def _last_packet_sent(self) -> RpcPacket:
        packet = RpcPacket()
        assert self._last_packet_sent_bytes is not None
        packet.MergeFromString(self._last_packet_sent_bytes)
        return packet

    def test_channel(self) -> None:
        self.assertEqual(
            self._client.channel(CLIENT_FIRST_CHANNEL_ID).channel.id,
            CLIENT_FIRST_CHANNEL_ID,
        )
        self.assertEqual(
            self._client.channel(CLIENT_SECOND_CHANNEL_ID).channel.id,
            CLIENT_SECOND_CHANNEL_ID,
        )

    def test_channel_default_is_first_listed(self) -> None:
        self.assertEqual(
            self._client.channel().channel.id, CLIENT_FIRST_CHANNEL_ID
        )

    def test_channel_invalid(self) -> None:
        with self.assertRaises(KeyError):
            self._client.channel(404)

    def test_all_methods(self) -> None:
        services = self._client.services

        all_methods = {
            services['pw.test1.PublicService'].methods['SomeUnary'],
            services['pw.test1.PublicService'].methods['SomeServerStreaming'],
            services['pw.test1.PublicService'].methods['SomeClientStreaming'],
            services['pw.test1.PublicService'].methods['SomeBidiStreaming'],
            services['pw.test2.Alpha'].methods['Unary'],
            services['pw.test2.Bravo'].methods['BidiStreaming'],
        }
        self.assertEqual(set(self._client.methods()), all_methods)

    def test_method_present(self) -> None:
        self.assertIs(
            self._client.method('pw.test1.PublicService.SomeUnary'),
            self._client.services['pw.test1.PublicService'].methods[
                'SomeUnary'
            ],
        )
        self.assertIs(
            self._client.method('pw.test1.PublicService/SomeUnary'),
            self._client.services['pw.test1.PublicService'].methods[
                'SomeUnary'
            ],
        )

    def test_method_invalid_format(self) -> None:
        with self.assertRaises(ValueError):
            self._client.method('SomeUnary')

    def test_method_not_present(self) -> None:
        with self.assertRaises(KeyError):
            self._client.method('pw.test1.PublicService/ThisIsNotGood')

        with self.assertRaises(KeyError):
            self._client.method('nothing.Good')

    def test_process_packet_invalid_proto_data(self) -> None:
        self.assertIs(
            self._client.process_packet(b'NOT a packet!'), Status.DATA_LOSS
        )

    def test_process_packet_not_for_client(self) -> None:
        self.assertIs(
            self._client.process_packet(
                RpcPacket(type=PacketType.REQUEST).SerializeToString()
            ),
            Status.INVALID_ARGUMENT,
        )

    def test_process_packet_unrecognized_channel(self) -> None:
        self.assertIs(
            self._client.process_packet(
                packets.encode_response(
                    RpcIds(
                        SOME_CHANNEL_ID,
                        SOME_SERVICE_ID,
                        SOME_METHOD_ID,
                        SOME_CALL_ID,
                    ),
                    PROTOS.packages.pw.test2.Request(),
                )
            ),
            Status.NOT_FOUND,
        )

    def test_process_packet_unrecognized_service(self) -> None:
        self.assertIs(
            self._client.process_packet(
                packets.encode_response(
                    RpcIds(
                        CLIENT_FIRST_CHANNEL_ID,
                        SOME_SERVICE_ID,
                        SOME_METHOD_ID,
                        SOME_CALL_ID,
                    ),
                    PROTOS.packages.pw.test2.Request(),
                )
            ),
            Status.OK,
        )

        self.assertEqual(
            self._last_packet_sent(),
            RpcPacket(
                type=PacketType.CLIENT_ERROR,
                channel_id=CLIENT_FIRST_CHANNEL_ID,
                service_id=SOME_SERVICE_ID,
                method_id=SOME_METHOD_ID,
                call_id=SOME_CALL_ID,
                status=Status.NOT_FOUND.value,
            ),
        )

    def test_process_packet_unrecognized_method(self) -> None:
        service = next(iter(self._client.services))

        self.assertIs(
            self._client.process_packet(
                packets.encode_response(
                    RpcIds(
                        CLIENT_FIRST_CHANNEL_ID,
                        service.id,
                        SOME_METHOD_ID,
                        SOME_CALL_ID,
                    ),
                    PROTOS.packages.pw.test2.Request(),
                )
            ),
            Status.OK,
        )

        self.assertEqual(
            self._last_packet_sent(),
            RpcPacket(
                type=PacketType.CLIENT_ERROR,
                channel_id=CLIENT_FIRST_CHANNEL_ID,
                service_id=service.id,
                method_id=SOME_METHOD_ID,
                call_id=SOME_CALL_ID,
                status=Status.NOT_FOUND.value,
            ),
        )

    def test_process_packet_non_pending_method(self) -> None:
        service = next(iter(self._client.services))
        method = next(iter(service.methods))

        self.assertIs(
            self._client.process_packet(
                packets.encode_response(
                    RpcIds(
                        CLIENT_FIRST_CHANNEL_ID,
                        service.id,
                        method.id,
                        SOME_CALL_ID,
                    ),
                    PROTOS.packages.pw.test2.Request(),
                )
            ),
            Status.OK,
        )

        self.assertEqual(
            self._last_packet_sent(),
            RpcPacket(
                type=PacketType.CLIENT_ERROR,
                channel_id=CLIENT_FIRST_CHANNEL_ID,
                service_id=service.id,
                method_id=method.id,
                call_id=SOME_CALL_ID,
                status=Status.FAILED_PRECONDITION.value,
            ),
        )

    def test_process_packet_non_pending_calls_response_callback(self) -> None:
        method = self._client.method('pw.test1.PublicService.SomeUnary')
        reply = method.response_type(payload='hello')

        def response_callback(
            rpc: client.PendingRpc,
            message,
            status: Status | None,
        ) -> None:
            self.assertEqual(
                rpc,
                client.PendingRpc(
                    self._client.channel(CLIENT_FIRST_CHANNEL_ID).channel,
                    method.service,
                    method,
                    call_id=SOME_CALL_ID,
                ),
            )
            self.assertEqual(message, reply)
            self.assertIs(status, Status.OK)

        self._client.response_callback = response_callback

        self.assertIs(
            self._client.process_packet(
                packets.encode_response(
                    RpcIds(
                        CLIENT_FIRST_CHANNEL_ID,
                        method.service.id,
                        method.id,
                        SOME_CALL_ID,
                    ),
                    reply,
                )
            ),
            Status.OK,
        )


class QueryServiceIntegrationTest(unittest.TestCase):
    """Tests query_services, is_service_available, and is_method_available."""

    def setUp(self) -> None:
        self._protos = python_protos.Library.from_strings([TEST_PROTO_1])
        self._client = client.Client.from_modules(
            callback_client.Impl(),
            [client.Channel(CLIENT_FIRST_CHANNEL_ID, self._handle_packet)],
            [*self._protos.modules(), query_pb2],
        )
        self._channel_client = self._client.channel(CLIENT_FIRST_CHANNEL_ID)

        self._public_service_id = pw_rpc.ids.calculate('pw.test1.PublicService')
        self._some_unary_id = pw_rpc.ids.calculate('SomeUnary')
        self._query_service_id = pw_rpc.ids.calculate('pw.rpc.Query')
        self._list_services_id = pw_rpc.ids.calculate('ListServices')
        self._list_methods_id = pw_rpc.ids.calculate('ListMethods')
        self._is_service_avail_id = pw_rpc.ids.calculate('IsServiceAvailable')
        self._is_method_avail_id = pw_rpc.ids.calculate('IsMethodAvailable')

    def _handle_packet(self, data: bytes) -> None:
        req_packet = packets.decode(data)
        if req_packet.service_id != self._query_service_id:
            return

        rpc_ids = RpcIds(
            req_packet.channel_id,
            req_packet.service_id,
            req_packet.method_id,
            req_packet.call_id,
        )

        if req_packet.method_id == self._list_services_id:
            services_resp = query_pb2.ListServicesResponse(
                service_ids=[self._public_service_id, 0x12345678]
            )
            self._client.process_packet(
                packets.encode_server_stream(rpc_ids, services_resp)
            )
            self._client.process_packet(packets.encode_response(rpc_ids))
        elif req_packet.method_id == self._list_methods_id:
            methods_req = query_pb2.ListMethodsRequest.FromString(
                req_packet.payload
            )
            if methods_req.service_id == self._public_service_id:
                methods_resp = query_pb2.ListMethodsResponse(
                    method_ids=[self._some_unary_id, 0x87654321]
                )
            else:
                methods_resp = query_pb2.ListMethodsResponse(
                    method_ids=[0x99999999]
                )
            self._client.process_packet(
                packets.encode_server_stream(rpc_ids, methods_resp)
            )
            self._client.process_packet(packets.encode_response(rpc_ids))
        elif req_packet.method_id == self._is_service_avail_id:
            service_avail_req = query_pb2.IsServiceAvailableRequest.FromString(
                req_packet.payload
            )
            avail_resp = query_pb2.IsAvailableResponse(
                is_available=(
                    service_avail_req.service_id == self._public_service_id
                )
            )
            self._client.process_packet(
                packets.encode_response(rpc_ids, avail_resp)
            )
        elif req_packet.method_id == self._is_method_avail_id:
            method_avail_req = query_pb2.IsMethodAvailableRequest.FromString(
                req_packet.payload
            )
            is_avail = (
                method_avail_req.service_id == self._public_service_id
                and method_avail_req.method_id == self._some_unary_id
            )
            avail_resp = query_pb2.IsAvailableResponse(is_available=is_avail)
            self._client.process_packet(
                packets.encode_response(rpc_ids, avail_resp)
            )

    def test_query_services(self) -> None:
        result = self._channel_client.query_services()
        self.assertIn('pw.test1.PublicService', result)
        self.assertEqual(
            result['pw.test1.PublicService'],
            ['SomeUnary', 'unknown_method_0x87654321'],
        )
        self.assertIn('unknown_service_0x12345678', result)
        self.assertEqual(
            result['unknown_service_0x12345678'],
            ['unknown_method_0x99999999'],
        )

    def test_is_service_available(self) -> None:
        self.assertTrue(
            self._channel_client.is_service_available('pw.test1.PublicService')
        )
        self.assertFalse(
            self._channel_client.is_service_available('pw.nonexistent.Service')
        )

    def test_is_method_available(self) -> None:
        self.assertTrue(
            self._channel_client.is_method_available(
                'pw.test1.PublicService', 'SomeUnary'
            )
        )
        self.assertFalse(
            self._channel_client.is_method_available(
                'pw.test1.PublicService', 'NonExistentMethod'
            )
        )


if __name__ == '__main__':
    unittest.main()
