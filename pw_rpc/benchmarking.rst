.. _module-pw_rpc-benchmarking:

============
Benchmarking
============
.. pigweed-module-subpage::
   :name: pw_rpc

``pw_rpc`` provides an RPC service and Python utilities for measuring throughput,
latency, and resilience of an RPC deployment over its underlying transport.

------------------------
pw.rpc.Benchmark service
------------------------
The Benchmark service provides a low-level RPC interface for sending bulk data
between client and server. The service is defined in ``pw_rpc/benchmark.proto``.

A raw RPC implementation of the benchmark service is provided, suitable for any
system running ``pw_rpc``.

To use the benchmark service in C++:

.. code-block:: cpp

   #include "pw_rpc/benchmark.h"
   #include "pw_rpc/server.h"

   constexpr pw::rpc::Channel kChannels[] = {/* ... */};
   static pw::rpc::Server server(kChannels);

   static pw::rpc::BenchmarkService benchmark_service;

   void RegisterServices() { server.RegisterService(benchmark_service); }

Benchmark service definition
============================
.. literalinclude:: benchmark.proto
   :language: protobuf
   :lines: 14-

--------------
Stress testing
--------------
The Benchmark service is also used to fuzz and stress-test the ``pw_rpc`` module
across multiple concurrent threads and fluctuating channel conditions.

To run the client-server fuzz stress test:

.. code-block:: bash

   bazelisk test //pw_rpc/fuzz:cpp_client_server_fuzz_test
