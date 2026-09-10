.. _module-pw_rpc-services:
.. _module-pw_rpc-guides:
.. _module-pw_rpc-quickstart:

=================
Creating services
=================
.. pigweed-module-subpage::
   :name: pw_rpc

This guide walks through defining, implementing, and testing a new RPC service in
C++ using ``pw_rpc``.

If you are a platform engineer looking to set up the RPC server, channels, and
transports for your project, start with the :ref:`module-pw_rpc-setup` instead.

------------------------------
1. Define the service (.proto)
------------------------------
Define your RPC service and message types in a Protocol Buffer file using ``proto3``
syntax.

.. literalinclude:: examples/sensor_service.proto
   :language: protobuf
   :start-after: [pw_rpc-examples-sensor-proto]
   :end-before: [pw_rpc-examples-sensor-proto]

.. _module-pw_rpc-syntax-versions:

proto2 versus proto3 syntax
===========================
Always use ``proto3`` syntax rather than ``proto2`` for new protocol buffers.
``proto2`` protobufs can be compiled for ``pw_rpc``, but ``pw_rpc`` lacks support
for non-zero default values in ``proto2``.

If you need to distinguish between a default-valued field and a missing field,
mark the field as ``optional`` in ``proto3``:

.. code-block:: protobuf

   syntax = "proto3";

   message ConfigMessage {
     // Leaving this field unset is equivalent to setting it to 0.
     uint32 sample_rate = 1;

     // Setting this field to 0 is distinguishable from leaving it unset.
     optional uint32 timeout_ms = 2;
   }

------------------------------
2. Configure your build system
------------------------------
``pw_rpc`` automatically generates C++ service base classes from your ``.proto``
files.

Bazel
=====
Use ``nanopb_rpc_proto_library`` or ``pwpb_rpc_proto_library``:

.. literalinclude:: examples/BUILD.bazel
   :language: python
   :start-after: [pw_rpc-examples-sensor-build]
   :end-before: [pw_rpc-examples-sensor-build]

GN
==
In a ``BUILD.gn`` file, use the ``pw_proto_library`` template:

.. code-block:: python

   import("$dir_pw_protobuf_compiler/proto.gni")

   pw_proto_library("sensor_protos") {
     sources = [ "sensor_service.proto" ]
   }

   pw_source_set("sensor_service") {
     sources = [ "sensor_service.cc" ]
     deps = [
       ":sensor_protos.nanopb_rpc",  # For Nanopb
       # or :sensor_protos.pwpb_rpc  # For pw_protobuf
       # or :sensor_protos.raw_rpc   # For Raw RPC
     ]
   }

CMake
=====
In a ``CMakeLists.txt`` file, use the ``pw_proto_library`` function:

.. code-block:: cmake

   include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
   include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)

   pw_proto_library(sensor_protos
     SOURCES
       sensor_service.proto
   )

   add_library(sensor_service_impl ...)
   target_link_libraries(sensor_service_impl PUBLIC
     sensor_protos.nanopb_rpc
   )

-------------------------------------
3. Implement the service class in C++
-------------------------------------
Inherit from your generated service base class and implement the RPC methods.

Using Nanopb (Recommended)
==========================
Declare the service class:

.. literalinclude:: examples/sensor_service.h
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-service-decl]
   :end-before: [pw_rpc-examples-sensor-service-decl]

Implement the RPC methods:

.. literalinclude:: examples/sensor_service.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-service-impl]
   :end-before: [pw_rpc-examples-sensor-service-impl]

.. _module-pw_rpc-guides-headers:

Using pw_protobuf
=================
Include the generated header ``"my_project/sensor_service.rpc.pwpb.h"``:

.. code-block:: cpp

   #include "my_project/sensor_service.rpc.pwpb.h"

   class SensorServicePwpbImpl final
       : public my_project::pw_rpc::pwpb::SensorService::Service<
             SensorServicePwpbImpl> {
    public:
     pw::Status GetReading(const my_project::SensorRequest::Message& request,
                           my_project::SensorResponse::Message& response) {
       response.temperature = 22.0f;
       response.humidity = 40.0f;
       return pw::OkStatus();
     }
   };

.. _module-pw_rpc-guides-raw-fallback:

Falling back to raw methods
===========================
You can mix raw RPC methods inside a Nanopb or ``pw_protobuf`` service! This is
useful when:

#. **Handling repeated fields or callbacks:** Nanopb callbacks require functions
   to be set *before* decoding; raw RPC gives you raw bytes so you can decode manually.
#. **Zero-copy serialization:** Write fields directly into the wire buffer in-place
   using ``pw::protobuf::StreamEncoder``.
#. **Low-overhead loopback / echo benchmarking.**

To use raw methods, change the method signature to use ``pw::ConstByteSpan``
and ``pw::rpc::RawServerWriter`` / ``pw::rpc::RawUnaryResponder``:

.. literalinclude:: examples/sensor_service.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-raw-fallback]
   :end-before: [pw_rpc-examples-sensor-raw-fallback]

-------------------------------------------
4. Register the service with the RPC server
-------------------------------------------
Instantiate your service implementation and register it with the RPC server:

.. literalinclude:: examples/sensor_service.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-register]
   :end-before: [pw_rpc-examples-sensor-register]

.. _module-pw_rpc-guides-unrequested-responses:

---------------------
Unrequested responses
---------------------
``pw_rpc`` supports sending server streaming responses to RPCs that have not yet
been invoked by a client. This is useful in scenarios like a device reboot:
after rebooting, the device opens the writer object and streams status to the
host.

.. code-block:: cpp

   // Open a ServerWriter for a server streaming RPC
   auto writer = RawServerWriter::Open<pw_rpc::raw::ServiceName::MethodName>(
       server, channel_id, service_instance);

   // Send responses
   writer.Write(encoded_response_1);
   writer.Write(encoded_response_2);

   // Finish the stream
   writer.Finish(pw::OkStatus());

---------------------------
5. Unit testing the service
---------------------------
``pw_rpc`` provides test method contexts that manage the RPC lifecycle, capture
response packets, and allow simulating client requests without needing a physical
transport.

.. list-table::
   :header-rows: 1

   * - Protobuf Library
     - Test Method Context
   * - Nanopb
     - ``PW_NANOPB_TEST_METHOD_CONTEXT``
   * - pw_protobuf
     - ``PW_PWPB_TEST_METHOD_CONTEXT``
   * - Raw
     - ``PW_RAW_TEST_METHOD_CONTEXT``

Unary RPC test example
======================

.. literalinclude:: examples/sensor_service_test.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-unary-test]
   :end-before: [pw_rpc-examples-sensor-unary-test]

Streaming RPC test example
==========================

.. literalinclude:: examples/sensor_service_test.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-stream-test]
   :end-before: [pw_rpc-examples-sensor-stream-test]
