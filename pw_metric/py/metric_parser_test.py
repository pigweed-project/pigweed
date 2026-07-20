#!/usr/bin/env python3
# Copyright 2025 The Pigweed Authors
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
"""Tests for retrieving and parsing metrics."""
from collections.abc import Mapping
from typing import Iterable
from unittest import TestCase, mock, main
from pw_metric import metric_parser

from pw_metric_proto import metric_service_pb2
from pw_status import Status
from pw_tokenizer import detokenize, tokens

DATABASE = tokens.Database(
    [
        tokens.TokenizedStringEntry(0x01148A48, "total_dropped"),
        tokens.TokenizedStringEntry(0x03796798, "min_queue_remaining"),
        tokens.TokenizedStringEntry(0x22198280, "total_created"),
        tokens.TokenizedStringEntry(0x534A42F4, "max_queue_used"),
        tokens.TokenizedStringEntry(0x5D087463, "pw::work_queue::WorkQueue"),
        tokens.TokenizedStringEntry(0xA7C43965, "log"),
    ]
)


def _dict_traverse(d) -> Iterable[tuple[object, object]]:
    for k, v in d.items():
        yield k, v
        if isinstance(v, Mapping):
            yield from _dict_traverse(v)


class TestParseMetrics(TestCase):
    """Test parsing metrics received from RPCs"""

    def setUp(self) -> None:
        """Creating detokenizer and mocking RPC."""
        self.detokenize = detokenize.Detokenizer(DATABASE)
        self.rpc_timeout_s = 1
        self.rpcs = mock.Mock()
        self.rpcs.pw = mock.Mock()
        self.rpcs.pw.metric = mock.Mock()
        self.rpcs.pw.metric.proto = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService.Get = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService.Get.return_value = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.status = (
            Status.OK
        )
        # Creating a group and metric name for better identification.
        self.log = 0xA7C43965
        self.total_created = 0x22198280
        self.total_dropped = 0x01148A48
        self.min_queue_remaining = 0x03796798
        self.metric = [
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_created],
                string_path=['N/A'],
                as_float=3.0,
            ),
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_dropped],
                string_path=['N/A'],
                as_float=4.0,
            ),
        ]

    def test_no_detokenizer(self) -> None:
        """Tests parsing metrics without a detokenizer."""
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric)
        ]
        metrics = metric_parser.parse_metrics(
            self.rpcs, None, self.rpc_timeout_s
        )
        expected_metrics = {
            '$a7c43965': {
                '$22198280': 3.0,
                '$01148a48': 4.0,
            }
        }
        self.assertEqual(expected_metrics, metrics)

    def test_bad_stream_status(self) -> None:
        """Test stream response has a status other than OK."""
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.status = (
            Status.ABORTED
        )
        self.assertEqual(
            {},
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Stream response was not aborted.',
        )

    def test_parse_metrics(self) -> None:
        """Test metrics being parsed and recorded."""
        # Loading metric into RPC.
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric)
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': 3.0,
                    'total_dropped': 4.0,
                }
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )

    def test_parse_64bit_metrics(self) -> None:
        """Test parsing 64-bit metrics (as_uint64, as_int64)."""
        metrics_64bit = [
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_created],
                as_uint64=2**64 - 1,
            ),
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_dropped],
                as_int64=-(2**63),
            ),
        ]
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=metrics_64bit)
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': 2**64 - 1,
                    'total_dropped': -(2**63),
                }
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='64-bit metrics are not equal.',
        )

    def test_parse_metric_fallback_28_bit(self) -> None:
        """Tests fallback to 28-bit mask match for old 31-bit database."""
        # Database has 0x22198280 ("total_created").
        # Request has 0x02198280 (28-bit masked version).
        metric = [
            metric_service_pb2.Metric(
                token_path=[self.log, 0x02198280],
                string_path=['N/A'],
                as_float=3.0,
            ),
        ]
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=metric)
        ]
        with self.assertLogs(level='WARNING') as log_capture:
            metrics = metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            )
        self.assertEqual(
            {
                'log': {
                    'total_created': 3.0,
                }
            },
            metrics,
        )
        self.assertEqual(len(log_capture.output), 1)
        self.assertIn("fell back to compatible match", log_capture.output[0])

    def test_three_metric_names(self) -> None:
        """Test creating a dictionary with three paths."""
        # Creating another leaf.
        self.metric.append(
            metric_service_pb2.Metric(
                token_path=[self.log, self.min_queue_remaining],
                string_path=['N/A'],
                as_float=1.0,
            )
        )
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric)
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': 3.0,
                    'total_dropped': 4.0,
                    'min_queue_remaining': 1.0,
                },
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )

    def test_inserting_unknown_token(self) -> None:
        # Inserting an unknown token as a group name.
        self.metric.append(
            metric_service_pb2.Metric(
                token_path=[0x007, self.total_dropped],
                string_path=['N/A'],
                as_float=1.0,
            )
        )
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric)
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': 3.0,
                    'total_dropped': 4.0,
                },
                '$00000007': {'total_dropped': 1.0},
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )

    def test_multiple_metric_response(self) -> None:
        """Tests multiple metric responses being handled."""
        # Adding more than one MetricResponses.
        metric = [
            metric_service_pb2.Metric(
                token_path=[0x007, self.total_dropped],
                string_path=['N/A'],
                as_float=1.0,
            )
        ]
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric),
            metric_service_pb2.MetricResponse(metrics=metric),
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': 3.0,
                    'total_dropped': 4.0,
                },
                '$00000007': {
                    'total_dropped': 1.0,
                },
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )

    def test_paths_longer_than_two(self) -> None:
        """Tests metric paths longer than two."""
        # Path longer than two.
        longest_metric = [
            metric_service_pb2.Metric(
                token_path=[
                    self.log,
                    self.total_created,
                    self.min_queue_remaining,
                ],
                string_path=['N/A'],
                as_float=1.0,
            ),
        ]
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=longest_metric),
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': {'min_queue_remaining': 1.0},
                }
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )
        # Create a new leaf in log.
        longest_metric.append(
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_dropped],
                string_path=['N/A'],
                as_float=3.0,
            )
        )
        metric = [
            metric_service_pb2.Metric(
                token_path=[0x007, self.total_dropped],
                string_path=['N/A'],
                as_float=1.0,
            ),
            metric_service_pb2.Metric(
                token_path=[0x007, self.total_created],
                string_path=['N/A'],
                as_float=2.0,
            ),
        ]
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=longest_metric),
            metric_service_pb2.MetricResponse(metrics=metric),
        ]
        self.assertEqual(
            {
                'log': {
                    'total_created': {
                        'min_queue_remaining': 1.0,
                    },
                    'total_dropped': 3.0,
                },
                '$00000007': {
                    'total_dropped': 1.0,
                    'total_created': 2.0,
                },
            },
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            ),
            msg='Metrics are not equal.',
        )

    def test_conflicting_keys(self) -> None:
        """Tests conflicting key and value assignment."""
        longest_metric = [
            metric_service_pb2.Metric(
                token_path=[
                    self.log,
                    self.total_created,
                    self.min_queue_remaining,
                ],
                as_float=1.0,
            ),
        ]
        # Creates a conflict at log/total_created, should throw an error.
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=longest_metric),
            metric_service_pb2.MetricResponse(metrics=self.metric),
        ]
        with self.assertRaises(ValueError):
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            )

    def test_conflicting_logs(self) -> None:
        """Tests conflicting loga being streamed."""
        longest_metric = [
            metric_service_pb2.Metric(
                token_path=[self.log, self.total_created],
                as_float=1.0,
            ),
        ]
        # Creates a duplicate metric for log/total_created.
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=longest_metric),
            metric_service_pb2.MetricResponse(metrics=self.metric),
        ]
        with self.assertRaises(ValueError):
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            )
        # Duplicate metrics being loaded.
        self.rpcs.pw.metric.proto.MetricService.Get.return_value.responses = [
            metric_service_pb2.MetricResponse(metrics=self.metric),
            metric_service_pb2.MetricResponse(metrics=self.metric),
        ]
        with self.assertRaises(ValueError):
            metric_parser.parse_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            )


class GetAllMetricsTest(TestCase):
    """Test get_all_metrics using the paginated Walk RPC."""

    def setUp(self) -> None:
        """Set up mocks for the Walk RPC."""
        self.detokenize = detokenize.Detokenizer(DATABASE)
        self.rpc_timeout_s = 1
        self.rpcs = mock.Mock()
        self.rpcs.pw = mock.Mock()
        self.rpcs.pw.metric = mock.Mock()
        self.rpcs.pw.metric.proto = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService = mock.Mock()
        self.rpcs.pw.metric.proto.MetricService.Walk = mock.Mock()

        # Define some tokens from the DATABASE
        self.log = 0xA7C43965
        self.total_created = 0x22198280
        self.total_dropped = 0x01148A48
        self.min_queue_remaining = 0x03796798
        self.work_queue = 0x5D087463
        self.max_queue_used = 0x534A42F4

        # Note: The WalkRequest mock is handled via patch in
        # test_get_all_metrics_with_pagination, not here in setUp.

    def test_get_all_metrics_with_pagination(self) -> None:
        """Tests that the client correctly handles a paginated response."""
        # Create metrics for page 1
        metric1 = metric_service_pb2.Metric(
            token_path=[self.log, self.total_created], as_int=100
        )
        metric2 = metric_service_pb2.Metric(
            token_path=[self.log, self.total_dropped], as_int=5
        )

        # Create metrics for page 2 (with a deeper path)
        metric3 = metric_service_pb2.Metric(
            token_path=[self.work_queue, self.log, self.max_queue_used],
            as_int=50,
        )

        # Create responses
        response1 = metric_service_pb2.WalkResponse(
            metrics=[metric1, metric2], cursor=12345, done=False
        )
        response2 = metric_service_pb2.WalkResponse(
            metrics=[metric3], done=True
        )

        # Set up the mock to return responses in sequence
        self.rpcs.pw.metric.proto.MetricService.Walk.side_effect = [
            (Status.OK, response1),
            (Status.OK, response2),
        ]

        # Patch the WalkRequest constructor where it's imported in the
        # metric_parser module. This is necessary because the code under
        # test (metric_parser.py) imports and calls
        # metric_service_pb2.WalkRequest directly.
        with mock.patch(
            'pw_metric.metric_parser.metric_service_pb2.WalkRequest'
        ) as mock_walk_request:
            # The mock must return a value when constructed, as it's
            # passed as the request object to the Walk RPC.
            mock_walk_request.return_value = mock.Mock()

            metrics = metric_parser.get_all_metrics(
                self.rpcs, self.detokenize, self.rpc_timeout_s
            )

        expected_metrics = {
            'log': {'total_created': 100, 'total_dropped': 5},
            'pw::work_queue::WorkQueue': {'log': {'max_queue_used': 50}},
        }
        self.assertEqual(expected_metrics, metrics)

        # Verify RPC calls using the mock_walk_request.
        self.assertEqual(mock_walk_request.call_count, 2)
        calls = mock_walk_request.call_args_list
        # First call has a cursor of 0.
        self.assertEqual(calls[0].kwargs['cursor'], 0)
        # Second call uses cursor from the first response
        self.assertEqual(calls[1].kwargs['cursor'], 12345)

    def test_get_all_metrics_single_page(self) -> None:
        """Tests a response that fits in a single page."""
        metric = metric_service_pb2.Metric(
            token_path=[self.total_created], as_int=100
        )
        response = metric_service_pb2.WalkResponse(metrics=[metric], done=True)
        self.rpcs.pw.metric.proto.MetricService.Walk.return_value = (
            Status.OK,
            response,
        )

        metrics = metric_parser.get_all_metrics(
            self.rpcs, self.detokenize, self.rpc_timeout_s
        )

        self.assertEqual({'total_created': 100}, metrics)

        # Ensure the returned type is exactly dict and not a subclass, all
        # sub-dicts are too.
        self.assertEqual(type(metrics), dict)
        for _, v in _dict_traverse(metrics):
            if isinstance(v, Mapping):
                self.assertEqual(type(v), dict)

        self.rpcs.pw.metric.proto.MetricService.Walk.assert_called_once()

    def test_rpc_failure(self) -> None:
        """Tests that an empty dict is returned on RPC failure."""
        self.rpcs.pw.metric.proto.MetricService.Walk.return_value = (
            Status.UNAVAILABLE,
            None,
        )
        metrics = metric_parser.get_all_metrics(
            self.rpcs, self.detokenize, self.rpc_timeout_s
        )
        self.assertEqual({}, metrics)

    def test_rpc_failure_mid_stream(self) -> None:
        """Tests returning partial metrics when an RPC fails mid-stream."""
        metric1 = metric_service_pb2.Metric(
            token_path=[self.log, self.total_created], as_int=100
        )
        response1 = metric_service_pb2.WalkResponse(
            metrics=[metric1], cursor=12345, done=False
        )
        self.rpcs.pw.metric.proto.MetricService.Walk.side_effect = [
            (Status.OK, response1),
            (Status.UNAVAILABLE, None),
        ]

        metrics = metric_parser.get_all_metrics(
            self.rpcs, self.detokenize, self.rpc_timeout_s
        )
        self.assertEqual({'log': {'total_created': 100}}, metrics)

    def test_no_detokenizer(self) -> None:
        """Tests parsing metrics without a detokenizer."""
        metric = metric_service_pb2.Metric(
            token_path=[self.total_created], as_int=100
        )
        response = metric_service_pb2.WalkResponse(metrics=[metric], done=True)
        self.rpcs.pw.metric.proto.MetricService.Walk.return_value = (
            Status.OK,
            response,
        )
        metrics = metric_parser.get_all_metrics(
            self.rpcs, None, self.rpc_timeout_s
        )
        self.assertEqual({'$22198280': 100}, metrics)
        self.rpcs.pw.metric.proto.MetricService.Walk.assert_called_once()


if __name__ == '__main__':
    main()
