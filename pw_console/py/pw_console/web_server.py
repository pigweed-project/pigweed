# Copyright 2022 The Pigweed Authors
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
"""Console HTTP Log Server functions."""

import asyncio
import datetime
import email.utils
import logging
import mimetypes
from pathlib import Path
import secrets
import socket
from threading import Thread
from urllib.parse import urlsplit
import webbrowser
from typing import Any, Callable

from aiohttp import web, WSMsgType

from pw_console.web_kernel import WebKernel

_LOG = logging.getLogger(__package__)


def aiohttp_server(handler: Callable) -> web.AppRunner:
    app = web.Application()
    app.add_routes([web.get('/', handler), web.get('/{name}', handler)])
    runner = web.AppRunner(app)
    return runner


def find_available_port(start_port=8080, max_retries=100) -> int:
    """Finds the next available port starting from `start_port`."""
    for port in range(start_port, start_port + max_retries):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(('127.0.0.1', port))
                return port
            except OSError:
                pass  # Port is already in use, try the next one

    raise RuntimeError(
        (
            'No available port found within the range '
            f'{start_port}-{start_port + max_retries}'
        )
    )


def pw_console_http_server(
    start_port: int,
    html_files: dict[str, str],
    kernel_params: dict[str, Any] | None = None,
) -> None:
    try:
        handler = WebHandler(html_files=html_files, kernel_params=kernel_params)
        handler.start_web_socket_streaming_responder_thread()
        runner = aiohttp_server(handler.handle_request)
        port = find_available_port(start_port)
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(runner.setup())
        host = '127.0.0.1'
        site = web.TCPSite(runner, host, port)
        loop.run_until_complete(site.start())
        url = f'http://{host}:{port}'
        print(url)
        webbrowser.open(url)
        loop.run_forever()
    except KeyboardInterrupt:
        _LOG.info('Shutting down...')
        handler.stop_web_socket_streaming_responder_thread()
        loop.stop()


class WebHandler:
    """Request handler that serves files from pw_console.html package data."""

    def __init__(
        self,
        html_files: dict[str, str],
        kernel_params: dict[str, Any] | None = None,
    ) -> None:
        self.html_files = html_files
        self.date_modified = email.utils.formatdate(
            datetime.datetime.now().timestamp(), usegmt=True
        )
        self.kernel_params: dict[str, Any] = {}
        if kernel_params:
            self.kernel_params = kernel_params

        # The /ws endpoint can execute arbitrary Python via WebKernel. Require a
        # per-process secret on every WebSocket handshake so that other origins
        # cannot connect. The token is injected into the served console.html
        # below and never logged.
        self._ws_auth_token = secrets.token_urlsafe(32)
        if '/console.html' in self.html_files:
            self.html_files['/console.html'] = self.html_files[
                '/console.html'
            ].replace('"/ws"', f'"/ws?token={self._ws_auth_token}"')

        self.web_socket_streaming_responder_loop = asyncio.new_event_loop()

    # Hostnames a browser may legitimately present in the Origin header when
    # loaded from this server (which only ever binds to localhost).
    _ALLOWED_WS_ORIGIN_HOSTS = frozenset(('localhost', '127.0.0.1', '::1'))

    def _websocket_request_authorized(self, request: web.Request) -> bool:
        """Authorize a /ws upgrade request.

        Rejects the request unless it (a) presents no Origin header or one
        whose host is a loopback name, and (b) carries the per-process auth
        token as a ``token`` query parameter. Browsers always attach an Origin
        header to WebSocket handshakes and do not allow page scripts to override
        it, so (a) blocks cross-origin and DNS-rebinding attacks while still
        admitting non-browser local clients that omit Origin.
        """
        origin = request.headers.get('Origin')
        if origin is not None:
            try:
                origin_host = urlsplit(origin).hostname
            except ValueError:
                origin_host = None
            if origin_host not in self._ALLOWED_WS_ORIGIN_HOSTS:
                return False
        token = request.query.get('token', '')
        return secrets.compare_digest(token, self._ws_auth_token)

    def _web_socket_streaming_responder_thread_entry(self):
        """Entry point for the web socket logging handlers thread."""
        asyncio.set_event_loop(self.web_socket_streaming_responder_loop)
        self.web_socket_streaming_responder_loop.run_forever()

    def start_web_socket_streaming_responder_thread(self):
        """Start thread for handling log messages to web socket responses."""
        thread = Thread(
            target=self._web_socket_streaming_responder_thread_entry,
            args=(),
            daemon=True,
        )
        thread.start()

    def stop_web_socket_streaming_responder_thread(self):
        self.web_socket_streaming_responder_loop.call_soon_threadsafe(
            self.web_socket_streaming_responder_loop.stop
        )

    async def handle_request(
        self, request: web.Request
    ) -> web.Response | web.WebSocketResponse:
        _LOG.debug(
            '%s: %s',
            request.remote,
            request.raw_path,
        )

        path = request.path
        if path == '/':
            path = '/console.html'

        if path == '/ws':
            if not self._websocket_request_authorized(request):
                _LOG.warning('Rejected unauthorized WebSocket handshake on /ws')
                return web.Response(status=403, text='Forbidden')
            return await self.handle_websocket(request)

        if path not in self.html_files:
            return web.Response(status=404, text='File not found')

        content: bytes = self.html_files[path].encode('utf-8')
        content_type = 'application/octet-stream'
        mime_guess, _ = mimetypes.guess_type(Path(path).name)
        if mime_guess:
            content_type = mime_guess

        return web.Response(
            body=content,
            content_type=content_type,
            headers={
                'Content-Length': str(len(content)),
                'Last-Modified': self.date_modified,
            },
        )

    async def handle_websocket(
        self, request: web.Request
    ) -> web.WebSocketResponse:
        """Handle a websocket connection request by creating a new kernel."""
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        kernel = WebKernel(
            ws, self.kernel_params, self.web_socket_streaming_responder_loop
        )
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    if msg.data == 'close':
                        return ws
                    response = await kernel.handle_request(msg.data)
                    await ws.send_str(response)
                elif msg.type == WSMsgType.ERROR:
                    _LOG.warning(
                        'ws connection closed with exception: %s',
                        ws.exception(),
                    )
        finally:
            kernel.handle_disconnect()
            await ws.close()

        return ws
