.. _module-pw_rpc2-python-api:

====================
Python API reference
====================
.. pigweed-module-subpage::
   :name: pw_rpc2

Generated API reference for the ``pw_rpc2`` Python package
(``//pw_rpc2/py:pw_rpc2``). For usage guides and examples, see
:ref:`Python client <module-pw_rpc2-python>`.

-------
pw_rpc2
-------
.. automodule:: pw_rpc2

Client and services
===================
.. autoclass:: pw_rpc2.Client
   :members:

.. autoclass:: pw_rpc2.Services
   :members:

.. autoclass:: pw_rpc2.Package
   :members:

.. autoclass:: pw_rpc2.ServiceClient
   :members:

.. autoclass:: pw_rpc2.MethodClient
   :members:
   :inherited-members:
   :special-members: __call__

.. autoclass:: pw_rpc2.MethodType
   :members:
   :undoc-members:

.. autodata:: pw_rpc2.ProtoSource

.. autodata:: pw_rpc2.DEFAULT_HANDSHAKE_TIMEOUT

Calls
=====
.. autoclass:: pw_rpc2.Call
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.Reader
   :members:
   :inherited-members:
   :special-members: __iter__

.. autoclass:: pw_rpc2.Writer
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.UnaryCall
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.ServerStreamingCall
   :members:
   :inherited-members:
   :special-members: __iter__

.. autoclass:: pw_rpc2.ClientStreamingCall
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.BidirectionalStreamingCall
   :members:
   :inherited-members:
   :special-members: __iter__

Errors and status codes
=======================
.. autoclass:: pw_rpc2.RpcStatus
   :members:

.. autoexception:: pw_rpc2.RpcError
   :members:

.. autoexception:: pw_rpc2.RpcTimeout
   :members:

.. autoexception:: pw_rpc2.HandshakeError
   :members:

.. autoexception:: pw_rpc2.StreamClosedError
   :members:

.. autoexception:: pw_rpc2.DecodeError
   :members:

Transports
==========
``pw_rpc2.SocketClosed`` is re-exported from
:py:class:`pw_transport.SocketClosed`. Socket interfaces and implementations
(:py:class:`pw_transport.ReliableSocket`, :py:mod:`pw_transport.framed_tcp`, and
:py:mod:`pw_transport.forwarding_socket`) are documented in the
:ref:`pw_transport Python reference <module-pw_transport-python>`.

-----------
pw_rpc2.aio
-----------
.. automodule:: pw_rpc2.aio

Client and services
===================
.. autoclass:: pw_rpc2.aio.Client
   :members:

.. autoclass:: pw_rpc2.aio.MethodClient
   :members:
   :inherited-members:
   :special-members: __call__

Calls
=====
.. autoclass:: pw_rpc2.aio.Call
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.aio.Reader
   :members:
   :inherited-members:
   :special-members: __aiter__

.. autoclass:: pw_rpc2.aio.Writer
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.aio.UnaryCall
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.aio.ServerStreamingCall
   :members:
   :inherited-members:
   :special-members: __aiter__

.. autoclass:: pw_rpc2.aio.ClientStreamingCall
   :members:
   :inherited-members:

.. autoclass:: pw_rpc2.aio.BidirectionalStreamingCall
   :members:
   :inherited-members:
   :special-members: __aiter__
