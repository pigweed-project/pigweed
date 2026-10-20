.. _docs-category-comms:

=====
Comms
=====
.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------
Modules
-------
The following table lists all modules related to Comms.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_rpc2`", ":bdg-warning:`experimental`", "Next gen, zero-copy async RPC with end-to-end backpressure", "C++"
   ":ref:`module-pw_transport`", ":bdg-warning:`experimental`", "Asynchronous datagram-oriented transport interface", "C++"
   ":ref:`module-pw_channel`", ":bdg-danger:`deprecated`", "Async, zero-copy API for sending and receiving bytes or datagrams", "C++"
   ":ref:`module-pw_grpc`", ":bdg-secondary:`unstable`", "pw_rpc over gRPC", "C++"
   ":ref:`module-pw_hdlc`", ":bdg-primary:`stable`", "Simple, robust, and efficient serial communication", "Python, C++, TypeScript"
   ":ref:`module-pw_router`", ":bdg-warning:`experimental`", "Transport-agnostic packet routing", "C++"
   ":ref:`module-pw_rpc`", ":bdg-primary:`stable`", "Efficient, low-code-size RPC system for embedded devices", "C++, Java, JavaScript, Python, TypeScript"
   ":ref:`module-pw_rpc_transport`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_stream`", ":bdg-primary:`stable`", "A foundational interface for streaming data", "C++, Rust"
   ":ref:`module-pw_transfer`", ":bdg-primary:`stable`", "", ""

---------
Bluetooth
---------
Dual-mode Bluetooth Host stack, HCI controllers, and profiles

Learn more: :doc:`/bluetooth/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_bluetooth`", ":bdg-secondary:`unstable`", "Host-layer Bluetooth Low Energy APIs and utilities", "C++"
   ":ref:`module-pw_bluetooth_hci`", ":bdg-primary:`stable`", "Bluetooth Host Controller Interface utilities", "C++"
   ":ref:`module-pw_bluetooth_profiles`", ":bdg-secondary:`unstable`", "Bluetooth profiles built on top of pw_bluetooth", "C++"
   ":ref:`module-pw_bluetooth_proxy`", ":bdg-secondary:`unstable`", "Lightweight proxy for augmenting Bluetooth functionality", "C++"
   ":ref:`module-pw_bluetooth_sapphire`", ":bdg-secondary:`unstable`", "Battle-tested Bluetooth with rock-solid reliability", "C++"

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   RPC2 <../pw_rpc2/docs>
   Transport <../pw_transport/docs>
   Bluetooth <../bluetooth/index>
   Channels <../pw_channel/docs>
   gRPC <../pw_grpc/docs>
   HDLC <../pw_hdlc/docs>
   Packet routing <../pw_router/docs>
   RPC1 <../pw_rpc/docs>
   RPC transport <../pw_rpc_transport/docs>
   Streams <../pw_stream/docs>
   Data transfer <../pw_transfer/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
