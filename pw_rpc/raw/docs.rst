.. _module-pw_rpc-raw:

================
C++ with raw RPC
================
.. pigweed-module-subpage::
   :name: pw_rpc

**Raw RPC** allows C++ service methods and clients to send and receive raw,
unparsed byte buffers (``pw::ConstByteSpan`` / ``pw::span<const std::byte>``)
directly, without passing them through generated protobuf structs or classes.

-------------------
When to use raw RPC
-------------------
* **Zero-copy serialization:** Encode fields directly into wire buffers in-place
  using :ref:`module-pw_protobuf` stream encoders.
* **Large or dynamic payloads:** Handle variable-length binary payloads, chunked
  firmware images, or pass-through proxy data without intermediate buffer copies.
* **Low-overhead benchmarking:** Implement echo and throughput test services
  with minimal CPU cycles (see :ref:`module-pw_rpc-benchmarking`).
* **Mixing with Nanopb or pw_protobuf:** Fall back to raw methods on individual
  performance-critical RPCs within a Nanopb or ``pw_protobuf`` service.

-----
Usage
-----
In your build file, depend on the ``raw_rpc`` variant of your proto library:

.. code-block:: python

   # Bazel: raw_rpc_proto_library
   # GN: :my_protos.raw_rpc

Include the generated header ``"my_project/sensor_service.raw_rpc.pb.h"``:

.. code-block:: cpp

   #include "my_project/sensor_service.raw_rpc.pb.h"
   #include "pw_bytes/span.h"
   #include "pw_rpc/raw/server_reader_writer.h"

-----------
Server-side
-----------
Implement services by inheriting from the generated ``pw_rpc::raw`` service base:

.. literalinclude:: ../examples/raw_service.cc
   :language: cpp
   :start-after: [pw_rpc-raw-service-impl]
   :end-before: [pw_rpc-raw-service-impl]

--------------------------------
Mixing raw methods into services
--------------------------------
You do **not** need to declare an entire service as raw. You can implement individual
methods as raw inside a :ref:`Nanopb <module-pw_rpc_nanopb>` or
:ref:`pw_protobuf <module-pw_rpc_pw_protobuf>` service:

.. literalinclude:: ../examples/raw_service.cc
   :language: cpp
   :start-after: [pw_rpc-raw-mixed-service]
   :end-before: [pw_rpc-raw-mixed-service]

-----------
Client-side
-----------
Raw clients allow sending and receiving raw byte spans directly:

.. literalinclude:: ../examples/raw_service.cc
   :language: cpp
   :start-after: [pw_rpc-raw-client-call]
   :end-before: [pw_rpc-raw-client-call]
