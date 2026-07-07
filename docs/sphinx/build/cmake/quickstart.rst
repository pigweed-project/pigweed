.. _CMake quickstart: https://pigweed.googlesource.com/pigweed/quickstart/cmake
.. _FetchContent: https://cmake.org/cmake/help/latest/module/FetchContent.html

.. _cmake-quickstart:

================
CMake quickstart
================
This tutorial shows you how to build and run our `CMake quickstart`_ project.
The project is a minimal, complete example of a CMake-built firmware application
that pulls in Pigweed as a dependency and implements RPC via
:ref:`module-pw_rpc` and :ref:`module-pw_system`. The firmware app functions as
an RPC server that listens on a local TCP socket for RPC requests. You will be
able to simulate this firmware on your development host via
:ref:`target-host-device-simulator`. Using :ref:`module-pw_console` you will
send RPC requests to the simulated firmware. All RPC requests and responses are
structured as protobufs. Protobuf codegen happens automatically as part of the
CMake build.

.. caution::

   In general Pigweed does not recommend CMake for new projects and won't be
   able to provide extensive support for new projects that choose CMake. See
   :ref:`docs-concepts-build-system` for more information.

.. _cmake-quickstart-limitations:

Limitations
===========
* This example has only been verified to work on Debian.

* The example is only set up to build a host-side simulation of the firmware.
  It does not set up a cross-compilation toolchain or provide flashing tools.

* The firmware build intentionally avoids a :ref:`bootstrap
  <module-pw_env_setup-bootstrap>` to demonstrate manual setup of Pigweed
  modules in a CMake project. Most of the toolchain must be installed globally
  and available on the system path. The project uses a bootstrap to simplify the
  client-side manual testing but this is only to minimize the project's
  complexity. It's possible to implement the demonstrated client without a
  bootstrap.

--------------------
Install dependencies
--------------------
.. tab-set::

   .. tab-item:: Linux

      .. code-block:: console

         sudo apt-get install -y build-essential cmake \
             ninja-build protobuf-compiler python3 \
             python3-protobuf python3-serial

---------------
Set up the repo
---------------
1. Clone the repo.

   .. code-block:: console

      git clone https://pigweed.googlesource.com/pigweed/quickstart/cmake quickstart

2. ``cd`` into the root dir of the repo.

   .. code-block:: console

      cd quickstart

Source code summary
===================
The project only has a handful of source code files. Here's an explanation of
each one.

* ``//CMakeLists.txt``: Fetches dependencies, configures Pigweed backends,
  sets up C++ and Python protobuf codegen, and defines the ``rpc_demo``
  executable target.

* ``//main.cc``: Initializes ``pw_system`` and implements ``UserAppInit`` to
  register the custom ``MathService``.

* ``//math_service.proto``: Defines the protobuf service ``MathService`` with
  an ``Add`` method.

* ``//math_service.h``: Implements the ``MathService`` defined in
  ``math_service.proto``.

* ``//run_console.py``: Sets up the Python path to include generated protos
  and launches an RPC client.

-----------------
Build the project
-----------------
1. Configure the CMake build:

   .. code-block:: console

      cmake -B build -S . -G Ninja

   Aside from getting ready to build the firmware, a few other non-obvious
   important things happen during this step:

   * The Pigweed and Nanopb repos are fetched as dependencies. See the
     `FetchContent`_ invocations in ``CMakeLists.txt``.

   * Pigweed :ref:`backend <docs-facades>` variables are configured.

   * Python and C++ protobuf codegen is configured.

   See the comments in ``CMakeLists.txt`` for more context.

2. Run the CMake build:

   .. code-block:: console

      cmake --build build --target rpc_demo

Generated files summary
=======================
The following files are generated during the CMake build.

* ``//build/rpc_demo``: The firmware binary.

* ``//build/math_service/nanopb/math_service.pb.*``: Nanopb-generated C code
  for the ``math_service.proto`` messages.

* ``//build/math_service/nanopb_rpc/math_service.rpc.pb.h``: Pigweed RPC
  generated C++ header for the ``math_service.proto`` service.

* ``//build/generated_python/python/math_service_pb2.py``: Python protobuf
  module generated from ``math_service.proto``, used by ``run_console.py``.

* ``//build/python_packages/``: Python protobuf modules generated from
  Pigweed internal protos, required by Pigweed RPC plugins.

------------
Run the demo
------------
1. Open a console tab and run ``./build/rpc_demo``. The simulated firmware
   device boots up and starts listening on ``localhost:33000``.

2. Open another console tab and ``cd`` into the root of the upstream Pigweed
   repo.

   .. code-block:: console

      cd third_party/pigweed

3. Bootstrap an environment for ``pw_console``.

   .. code-block:: console

      . bootstrap.sh

   .. note::

      As mentioned in :ref:`cmake-quickstart-limitations`, bootstrap is only
      used here to simplify and speed up the manual testing part of the demo.
      It's possible to use ``pw_console`` without a bootstrap.

4. Start a ``pw_console`` session.

   .. code-block:: console

      python3 ../../../run_console.py --socket-addr localhost:33000

5. In the Python REPL of ``pw_console`` invoke the ``Echo`` RPC.

   .. code-block:: pycon

      >>> device.rpcs.pw.rpc.EchoService.Echo(msg="hello")
      (Status.OK, pw.rpc.EchoMessage(msg="hello"))

   You should see the server respond with a tuple containing ``Status.OK``
   followed by a response containing the same message that you sent.

6. Now try invoking the ``Add`` RPC. You should see the server respond
   with a tuple containing ``Status.OK`` again followed by the correct sum.

   .. code-block:: pycon

      >>> device.rpcs.rpc.math.MathService.Add(a=5, b=3)
      (Status.OK, rpc.math.AddResponse(result=8))

7. Go back to the tab running the simulated firmware device. You
   should see the server logging messages.

   .. code-block:: text

      Awaiting connection on port 33000
      Client connected
      INF  pw_system initialized, main thread sleeping...
      INF  System init
      INF  Registering RPC services
      INF  Starting threads
      INF  Running RPC server
      INF  Registering custom MathService
      INF  Server received Add: 5 + 3
      INF  Simulated device is still alive

----------
Next steps
----------
* Talk to us in the ``#cmake-build`` channel of our :ref:`Discord
  <docs-community>`.

* Learn more about Pigweed's :ref:`CMake support <cmake>`.
