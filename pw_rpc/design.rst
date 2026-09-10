.. _module-pw_rpc-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_rpc

.. _module-pw_rpc-design-overview:

--------
Overview
--------
The semantics of ``pw_rpc`` are similar to `gRPC
<https://grpc.io/docs/what-is-grpc/core-concepts/>`_.

.. _module-pw_rpc-design-lifecycle:

RPC call lifecycle
==================
In ``pw_rpc``, an RPC begins when the client sends an initial packet. The server
receives the packet, looks up the relevant service method, then calls into the
RPC function. The RPC is considered active until the server sends a status to
finish the RPC. The client may terminate an ongoing RPC by cancelling it.
Multiple concurrent RPC requests to the same method may be made simultaneously.

Depending the type of RPC, the client and server exchange zero or more protobuf
request or response payloads. There are four RPC types:

* **Unary**. The client sends one request and the server sends one
  response with a status.
* **Server streaming**. The client sends one request and the server sends zero
  or more responses followed by a status.
* **Client streaming**. The client sends zero or more requests and the server
  sends one response with a status.
* **Bidirectional streaming**. The client sends zero or more requests and the
  server sends zero or more responses followed by a status.

.. _module-pw_rpc-design-events:

Events
------
The key events in the RPC lifecycle are:

* **Start**. The client initiates the RPC. The server's RPC body executes.
* **Finish**. The server sends a status and completes the RPC. The client calls
  a callback.
* **Request**. The client sends a request protobuf. The server calls a callback
  when it receives it. In unary and server streaming RPCs, there is only one
  request and it is handled when the RPC starts.
* **Response**. The server sends a response protobuf. The client calls a
  callback when it receives it. In unary and client streaming RPCs, there is
  only one response and it is handled when the RPC completes.
* **Error**. The server or client terminates the RPC abnormally with a status.
  The receiving endpoint calls a callback.
* **Request Completion**. The client sends a message that it would like to
  request call completion. The server calls a callback when it receives it. Some
  servers may ignore the request completion message. In client and bidirectional
  streaming RPCs, this also indicates that client has finished sending requests.

.. _module-pw_rpc-design-services:

Services
========
A service is a logical grouping of RPCs defined within a ``.proto`` file. ``pw_rpc``
uses these ``.proto`` definitions to generate code for a base service, from which
user-defined RPCs are implemented.

``pw_rpc`` supports multiple protobuf libraries, and the generated code API
depends on which is used.

Services must be registered with a server in order to call their methods.
Services may later be unregistered, which cancels calls for methods in that
service and prevents future calls to them, until the service is re-registered.

Background:

* `Protocol Buffer service
  <https://developers.google.com/protocol-buffers/docs/proto3#services>`_
* `gRPC service definition
  <https://grpc.io/docs/what-is-grpc/core-concepts/#service-definition>`_

.. _module-pw_rpc-design-status-codes:

Status codes
============
``pw_rpc`` call objects (``ClientReaderWriter``, ``ServerReaderWriter``, etc.)
use certain status codes to indicate what occurred. These codes are returned
from functions like ``Write()`` or ``Finish()``.

* ``OK`` -- The operation succeeded.
* ``UNAVAILABLE`` -- The channel is not currently registered with the server or
  client.
* ``UNKNOWN`` -- Sending a packet failed due to an unrecoverable
  :cpp:func:`pw::rpc::ChannelOutput::Send` error.


.. _module-pw_rpc-design-errata:

Errata
------
Prior to support for concurrent requests to a single method, no identifier was
present to distinguish different calls to the same method. When a "call ID"
feature was first introduced to solve this issue, existing clients and servers
(1) set this value to zero and (2) ignored this value.

When initial support for concurrent methods was added, a separate "open call ID"
was introduced to distinguish unrequested responses. However, legacy servers
built prior to this change continue to send unrequested responses with call ID
zero. Prior to `this fix
<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/192311>`_, clients
which used "open call ID" would not accept unrequested responses from legacy
servers. Clients built after that change will accept unrequested responses which
use both "open call ID" and call ID zero.

See `Issue 237418397 <https://issues.pigweed.dev/issues/237418397>`_ for more
details and discussion.

.. _module-pw_rpc-design-naming:

------
Naming
------

For upstream Pigweed services, this naming style is a requirement. Note that
some services created before this was established may use non-compliant
names. For Pigweed users, this naming style is a suggestion.

Reserved names
==============
``pw_rpc`` reserves a few service method names so they can be used for generated
classes. The following names cannot be used for service methods:

- ``Client``
- ``Service``
- Any reserved words in the languages ``pw_rpc`` supports (e.g. ``class``).

``pw_rpc`` does not reserve any service names, but the restriction of avoiding
reserved words in supported languages applies.

Service naming style
====================
``pw_rpc`` service names should use capitalized camel case and should not use
the term "Service". Appending "Service" to a service name is redundant, similar
to appending "Class" or "Function" to a class or function name. The
C++ implementation class may use "Service" in its name, however.

For example, a service for accessing a file system should simply be named
``service FileSystem``, rather than ``service FileSystemService``, in the
``.proto`` file.

.. code-block:: protobuf

   // file.proto
   package pw.file;

   service FileSystem {
       rpc List(ListRequest) returns (stream ListResponse);
   }

The C++ service implementation class may append "Service" to the name.

.. code-block:: cpp

   // file_system_service.h
   #include "pw_file/file.raw_rpc.pb.h"

   namespace pw::file {

   class FileSystemService
       : public pw_rpc::raw::FileSystem::Service<FileSystemService> {
     void List(ConstByteSpan request, RawServerWriter& writer);
   };

   }  // namespace pw::file

.. _module-pw_rpc-roadmap:

-------
Roadmap
-------
Concurrent requests were not initially supported in pw_rpc (added in `C++
<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/109077>`_, `Python
<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/139610>`_, and
`TypeScript
<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/160792>`_). As a
result, some user-written service implementations may not expect or correctly
support concurrent requests.
