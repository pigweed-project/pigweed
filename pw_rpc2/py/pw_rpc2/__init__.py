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
"""Python client for pw_rpc2.

:py:class:`Client` is a synchronous, thread-safe client;
:py:class:`pw_rpc2.aio.Client` is its asyncio counterpart.
"""

from __future__ import annotations

import pkgutil

# The enum modules generated from pw_rpc2's TOML enum definitions are in a
# separate directory, so extend the path before importing anything.
__path__ = pkgutil.extend_path(__path__, __name__)

from pw_transport import SocketClosed

from pw_rpc2 import aio
from pw_rpc2._call import (
    BidirectionalStreamingCall,
    Call,
    ClientStreamingCall,
    Reader,
    RpcError,
    RpcTimeout,
    ServerStreamingCall,
    StreamClosedError,
    UnaryCall,
    Writer,
)
from pw_rpc2._client import (
    Client,
    MethodClient,
    Package,
    ServiceClient,
    Services,
)
from pw_rpc2._connection import DEFAULT_HANDSHAKE_TIMEOUT, HandshakeError
from pw_rpc2._descriptors import MethodType, ProtoSource
from pw_rpc2._packets import DecodeError, RpcStatus

__all__ = [
    'BidirectionalStreamingCall',
    'Call',
    'Client',
    'ClientStreamingCall',
    'DEFAULT_HANDSHAKE_TIMEOUT',
    'DecodeError',
    'HandshakeError',
    'MethodClient',
    'MethodType',
    'Package',
    'ProtoSource',
    'Reader',
    'RpcError',
    'RpcStatus',
    'RpcTimeout',
    'ServerStreamingCall',
    'ServiceClient',
    'Services',
    'SocketClosed',
    'StreamClosedError',
    'UnaryCall',
    'Writer',
    'aio',
]
