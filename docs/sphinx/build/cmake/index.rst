.. _cmake:

=====
CMake
=====
.. grid:: 1

   .. grid-item-card:: :octicon:`rocket` Quickstart
      :link: cmake-quickstart
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      This tutorial shows you how to build and run our CMake quickstart
      project.  The project is a minimal, complete example of a CMake-built
      firmware application that pulls in Pigweed as a dependency and implements
      RPC via ``module-pw_rpc`` and ``module-pw_system``. The firmware app
      functions as an RPC server that listens on a local TCP socket for RPC
      requests. You will be able to simulate this firmware on your development
      host via Host Device Simulator. Using ``module-pw_console`` you will send
      RPC requests to the simulated firmware. All RPC requests and responses
      are structured as protobufs. Protobuf codegen happens automatically as
      part of the CMake build.

.. toctree::
   :maxdepth: 2
   :hidden:

   Quickstart <quickstart>
   Integration guide <integration/index>
   API reference <api>
