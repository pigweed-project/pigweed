.. _module-pw_grpc:

=======
pw_grpc
=======
.. pigweed-module::
   :name: pw_grpc

``pw_grpc`` is an implementation of the gRPC over HTTP2 protocol that utilizes
``pw_rpc`` for code generation and service hosting. It provides classes that map
between pw_rpc packets and gRPC HTTP2 frames, allowing pw_rpc services to be
exposed as gRPC services.

--------
Overview
--------
The ``Connection`` class implements the gRPC HTTP2 protocol on top of a socket
like stream. Create a new instance every time a new connection is established.
It will notify when new RPC calls are started, data is received, the call is
cancelled, and when the connection stream should be closed.

The ``PwRpcHandler`` class is what maps gRPC events provided by ``Connection``
instances to ``pw_rpc`` packets. It takes a ``pw::rpc::RpcPacketProcessor``
to forward packets to.

The ``GrpcChannelOutput`` class is what handles mapping outgoing ``pw_rpc``
packets back to the ``Connection`` send methods, which will translate to gRPC
responses.

Refer to the ``test_pw_rpc_server.cc`` file for detailed usage example of how to
integrate into a ``pw_rpc`` network.

----------------------------
Thread Safety and Allocators
----------------------------
The ``Connection`` class and the ``SendQueue`` implementation typically run on
different threads (e.g., one thread driving reads via ``ProcessFrame``, and
another driving writes via ``SendQueue::Run``).

Both ``Connection`` and ``SendQueue`` require access to an allocator for managing
send buffers. To avoid data races:

* They **must** share the same allocator reference.
* This allocator **must** be externally synchronized (for example, by wrapping it
  in a :cc:`pw::allocator::SynchronizedAllocator<pw::sync::Mutex>`).
* The allocator **must** outlive both the ``Connection`` and the ``SendQueue``
  instances.

-----
Build
-----
In order to use this module, a few config override for the ``pw_rpc`` module
must be set. ``PW_RPC_COMPLETION_REQUEST_CALLBACK=1`` and
``PW_RPC_METHOD_STORES_TYPE=1``.

If you are using bazel, you can include an array providing these defines via
``load("pw_grpc:config.bzl", "PW_GRPC_PW_RPC_CONFIG_OVERRIDES");`` and add those
to your ``//pw_rpc:config_override`` target.
