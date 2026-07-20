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
"""Tools to retrieve and parse metrics."""
from collections import defaultdict
from collections.abc import Mapping
import dataclasses
import logging
from typing import Any, Iterable, List, Union

from pw_tokenizer import detokenize
from pw_metric_proto import metric_service_pb2

_LOG = logging.getLogger(__name__)

# Token masks used by pw_metric.
_TOKEN_MASK_31 = 0x7FFFFFFF
_TOKEN_MASK_28 = 0x0FFFFFFF


@dataclasses.dataclass(frozen=True)
class ParsedMetric:
    """Dataclass to hold a metric's detokenized path and value."""

    path_names: List[str]
    value: Union[float, int]


def parse_metric(
    metric: metric_service_pb2.Metric,
    detokenizer: detokenize.Detokenizer | None,
) -> ParsedMetric:
    """Parses a single Metric proto, detokenizing its path."""
    path_names = []
    for path_token in metric.token_path:
        path_name = f'${path_token:08x}'  # Default to token if not found
        if detokenizer:
            lookup_result = detokenizer.lookup(path_token)
            if not lookup_result:
                # Fallback: Try a compatible 28-bit masked lookup for backward
                # compatibility with old 31-bit token databases.
                for db_token in detokenizer.database.token_to_entries:
                    if (db_token & _TOKEN_MASK_28) == (
                        path_token & _TOKEN_MASK_28
                    ):
                        lookup_result = detokenizer.lookup(db_token)
                        _LOG.warning(
                            'Metric token 0x%08x not found; fell back '
                            'to compatible match 0x%08x (%s) using 28-bit mask',
                            path_token,
                            db_token,
                            lookup_result[0].entry.string,
                        )
                        break
            if lookup_result:
                # Manually construct a DetokenizedString and strip quotes.
                path_name = str(
                    detokenize.DetokenizedString(
                        path_token, lookup_result, b'', False
                    )
                ).strip('"')
        path_names.append(path_name)

    value_type = metric.WhichOneof('value')
    value: Union[float, int]
    if value_type == 'as_float':
        value = metric.as_float
    elif value_type == 'as_int':
        value = metric.as_int
    elif value_type == 'as_uint64':
        value = metric.as_uint64
    elif value_type == 'as_int64':
        value = metric.as_int64
    return ParsedMetric(
        path_names=path_names,
        value=value,
    )


def _tree():
    """Creates a key based on given input."""
    return defaultdict(_tree)


def _insert(metrics, path_names, value):
    """Inserts any value in a leaf of the dictionary."""
    for index, path_name in enumerate(path_names):
        if index < len(path_names) - 1:
            metrics = metrics[path_name]
        elif path_name in metrics:
            # the value in this position isn't a float or int,
            # then collision occurs, throw an error.
            raise ValueError('Variable already exists: {p}'.format(p=path_name))
        else:
            metrics[path_name] = value


def _to_dict_recursive(d: Mapping) -> dict:
    """Recursively convert a nested mapping-like to a regular dict."""
    return {
        k: _to_dict_recursive(v) if isinstance(v, Mapping) else v
        for k, v in d.items()
    }


def metrics_to_dict(
    metric_messages: Iterable[metric_service_pb2.Metric],
    detokenizer: detokenize.Detokenizer | None,
) -> dict:
    """Interprets Metric messages into a hierarchical dict, by name."""
    metrics: defaultdict = _tree()
    if not detokenizer:
        _LOG.warning(
            'No metrics token database set. Metric names will be shown as '
            'tokens'
        )

    for metric_message in metric_messages:
        parsed = parse_metric(metric_message, detokenizer)
        _insert(metrics, parsed.path_names, parsed.value)

    return _to_dict_recursive(metrics)


def parse_metrics(
    rpcs: Any,
    detokenizer: detokenize.Detokenizer | None,
    timeout_s: float | None = 5,
) -> dict:
    """Retrieves all metrics using the legacy server-streaming Get RPC.

    NOTE: This function uses the legacy server-streaming MetricService.Get RPC.
    This method is suitable for transports that can support server-side
    streaming, but may be less robust on asynchronous or MTU-limited
    transports.

    The `get_all_metrics()` function (which uses the unary `Walk` RPC) is the
    generally preferred method for large metric sets that may exceed the
    transport MTU.
    """
    stream_response = rpcs.pw.metric.proto.MetricService.Get(
        metric_service_pb2.MetricRequest(), pw_rpc_timeout_s=timeout_s
    )
    if not stream_response.status.ok():
        _LOG.error('RPC failed: %s', stream_response.status)
        return {}

    metric_messages = (
        metric
        for metric_response in stream_response.responses
        for metric in metric_response.metrics
    )
    return metrics_to_dict(metric_messages, detokenizer)


def get_all_metrics(
    rpcs: Any,
    detokenizer: detokenize.Detokenizer | None,
    timeout_s: float | None = 5,
) -> dict:
    """Retrieves all metrics and inserts them into a dictionary.

    This function uses the unary, client-driven MetricService.Walk RPC.
    This RPC is suitable for asynchronous transports where the server cannot
    guarantee transport readiness. Its paginated nature also makes it ideal for
    large metric sets that may exceed the transport MTU.
    """
    # Repeatedly call the Walk RPC, passing the cursor from the previous
    # response until the server indicates the walk is complete.
    cursor = 0
    metric_messages = []
    while True:
        # The python generated proto uses WalkRequest instead of Message
        request = metric_service_pb2.WalkRequest(cursor=cursor)
        status, response = rpcs.pw.metric.proto.MetricService.Walk(
            request, pw_rpc_timeout_s=timeout_s
        )

        if not status.ok():
            _LOG.error('RPC failed: %s', status)
            break

        if response:
            metric_messages += response.metrics

            if response.done:
                break

            if not response.HasField('cursor'):
                _LOG.error('RPC response is not done but is missing a cursor')
                return {}
            cursor = response.cursor
        else:
            # Handle cases where the call was successful but the response is
            # empty
            if status.ok():
                break

            _LOG.error('RPC call failed and returned no response payload')
            break

    return metrics_to_dict(metric_messages, detokenizer)
