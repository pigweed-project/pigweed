.. _module-pw_rpc2-testing:

============
Unit testing
============
.. pigweed-module-subpage::
   :name: pw_rpc2

:cc:`pw::rpc2::test::TestHarness` is a lightweight dependency-injection fixture
for unit testing ``pw_rpc2`` services and clients with minimal boilerplate.

Constructing ``TestHarness<MyService>`` instantiates ``MyService`` (forwarding
any constructor arguments) and connects a client to it. The harness provides
both generated service clients, ``pwpb_client()`` and ``raw_client()``,
regardless of which flavor ``MyService`` implements. You can use the harness
from either side:

* **Testing a service**: Call RPCs through ``tester.pwpb_client()`` (or
  ``tester.raw_client()``) and assert on the responses or on
  ``tester.service()``.

  .. code-block:: c++

     TestHarness<MyService> tester(arg1, arg2);
     auto resp = tester.RunToCompletion(tester.pwpb_client().DoSomething(req));
     PW_TEST_ASSERT_OK(resp);
     EXPECT_EQ(tester.service().call_count(), 1u);

* **Testing client code**: Pass ``tester.pwpb_client()``,
  ``tester.raw_client()``, or ``tester.client()`` to the client code under test
  and use a test ``ServiceImpl`` to verify how the client interacts with the
  service.

  .. code-block:: c++

     TestHarness<FakeMyService> tester;
     MyClientComponent component(tester.pwpb_client());
     PW_TEST_ASSERT_OK(tester.RunToCompletion(component.FetchStatus()));
     EXPECT_TRUE(tester.service().was_queried());

.. _module-pw_rpc2-testing-setup:

-----
Setup
-----
Add the test harness and generated RPC library to your unit test target:

.. tab-set::

   .. tab-item:: Bazel

      .. code-block:: bazel

         load("//pw_unit_test:pw_cc_test.bzl", "pw_cc_test")

         pw_cc_test(
             name = "my_service_test",
             srcs = ["my_service_test.cc"],
             deps = [
                 ":my_service",
                 ":my_service_pwpb_rpc2",
                 "@pigweed//pw_rpc2/cpp:test_harness",
             ],
         )

   .. tab-item:: GN

      .. code-block:: py

         import("$dir_pw_unit_test/test.gni")

         pw_test("my_service_test") {
           sources = [ "my_service_test.cc" ]
           deps = [
             ":my_service",
             ":my_service_proto.pwpb_rpc2",
             "$dir_pw_rpc2/cpp:test_harness",
           ]
         }

   .. tab-item:: CMake

      .. code-block:: cmake

         pw_add_test(my_service_test
           SOURCES
             my_service_test.cc
           PRIVATE_DEPS
             my_service
             my_service_proto.pwpb_rpc2
             pw_rpc2.cpp.test_harness
         )

Include ``pw_rpc2/test_harness.h`` alongside your service implementation and
generated RPC header:

.. code-block:: c++

   #include "my_service.h"
   #include "my_service.pwpb.rpc2.h"
   #include "pw_rpc2/test_harness.h"
   #include "pw_unit_test/framework.h"

.. _module-pw_rpc2-testing-coro:

-----------------------
Testing with coroutines
-----------------------
Passing a coroutine lambda to ``RunToCompletion()`` lets a test ``co_await``
unary and streaming RPCs sequentially:

.. literalinclude:: cpp/test_harness_test.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_rpc2-testing-coro]
   :end-before: // DOCSTAG: [pw_rpc2-testing-coro]

.. _module-pw_rpc2-testing-futures:

------------------------------
Testing with futures and tasks
------------------------------
Pass an RPC future directly to ``RunToCompletion()``:

.. literalinclude:: cpp/test_harness_test.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_rpc2-testing-futures]
   :end-before: // DOCSTAG: [pw_rpc2-testing-futures]

To drive a hand-written :cc:`pw::async2::Task`, pass it to
``RunToCompletion()`` as an lvalue. The task stays in scope, so you can inspect
its results afterwards. Here, ``EchoUnaryClientTask`` is a task that makes one
``EchoUnary`` call and stores the response:

.. literalinclude:: cpp/test_harness_test.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_rpc2-testing-task]
   :end-before: // DOCSTAG: [pw_rpc2-testing-task]

When an RPC must pause on an external event or another stream operation before
completing, use ``RunInTaskUntilStalled()`` to advance the call and inspect its
intermediate state:

.. literalinclude:: cpp/test_harness_test.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_rpc2-testing-stalled]
   :end-before: // DOCSTAG: [pw_rpc2-testing-stalled]
