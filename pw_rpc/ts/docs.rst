.. _module-pw_rpc-ts:

-----------------
TypeScript client
-----------------
.. pigweed-module-subpage::
   :name: pw_rpc

Pigweed TypeScript client provides two ways to call RPCs. The :ref:`Device <module-pw_web-device>` API is
easier to work with if you are using the RPC via HDLC over WebSerial.

If the ``device`` abstraction is not a good fit, Pigweed provides the ``pw_rpc`` module,
which makes it possible to call Pigweed RPCs from TypeScript. The module includes
a client library to facilitate handling RPCs.

Creating an RPC Client
======================
The RPC client is instantiated from a list of channels and a set of protos.

.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-create-client]
   :end-before: [pw_rpc-ts-create-client]

To generate a ProtoSet/ProtoCollection from your own ``.proto`` files, use
``pw_proto_compiler`` in your ``package.json``:

.. code-block:: javascript

   "scripts": {
     "build-protos": "pw_proto_compiler -p protos/rpc1.proto -p protos/rpc2.proto --out dist/protos"
   }

This will generate a `collection.js` file which can be passed to ``Client.fromProtoSet``.

Finding an RPC Method
=====================
Once the client is instantiated with the correct proto library, the target RPC
method is found by searching based on the full name:
``{packageName}.{serviceName}.{methodName}``

.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-find-method]
   :end-before: [pw_rpc-ts-find-method]

The four possible RPC stubs are ``UnaryMethodStub``,
``ServerStreamingMethodStub``, ``ClientStreamingMethodStub``, and
``BidirectionalStreamingMethodStub``. Note that ``channel.methodStub()``
returns a general stub. Since each stub type has different invoke
parameters, the general stub should be typecast before using.

Invoke an RPC with callbacks
============================

All RPC methods can be invoked with a set of callbacks that are triggered when
either a response is received, the RPC is completed, or an error occurs. The
example below demonstrates registering these callbacks on a Bidirectional RPC:

.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-callback-invocation]
   :end-before: [pw_rpc-ts-callback-invocation]

Server streaming and bidirectional streaming methods can receive many responses
from the server. The client limits the maximum number of responses it stores for
a single RPC call to avoid unbounded memory usage in long-running streams. Once
the limit is reached, the oldest responses will be replaced as new ones arrive.
By default, the limit is set to ``DEFAULT_MAX_STREAM_RESPONSES (=16384)``, but
this can be configured on a per-call basis.

Open an RPC: ignore initial errors
==================================
``open`` allows you to start and register an RPC without throwing on initial errors. This
is useful for starting an RPC before the server is ready (for instance, starting
a logging RPC while the device is booting):

.. code-block:: typescript

   open(request?: Message,
       onNext: Callback = () => {},
       onCompleted: Callback = () => {},
       onError: Callback = () => {}): Call

Blocking RPCs: promise API
==========================
Each MethodStub type provides a ``call()`` / ``finishAndWait()`` method that allows
sending requests and awaiting responses through promises. The timeout field is optional;
if no timeout is specified, the RPC will wait indefinitely.

Unary RPC
---------
.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-unary-promise]
   :end-before: [pw_rpc-ts-unary-promise]

Server Streaming RPC
--------------------
.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-server-streaming-promise]
   :end-before: [pw_rpc-ts-server-streaming-promise]

Client Streaming RPC
--------------------
.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-client-streaming-promise]
   :end-before: [pw_rpc-ts-client-streaming-promise]

Bidirectional Streaming RPC
---------------------------
.. literalinclude:: docs_example.ts
   :language: typescript
   :start-after: [pw_rpc-ts-bidi-streaming-promise]
   :end-before: [pw_rpc-ts-bidi-streaming-promise]
