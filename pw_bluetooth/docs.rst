.. _module-pw_bluetooth:

============
pw_bluetooth
============
.. pigweed-module::
   :name: pw_bluetooth

The ``pw_bluetooth`` module contains APIs and utilities for the host layer of
Bluetooth Low Energy.

--------
Host API
--------
.. attention::

   This module is still under construction, the API is not yet stable.

The headers in ``public/pw_bluetooth`` constitute generic interfaces and types
for a Bluetooth Host API. Currently, only Low Energy APIs exist.

-------------
API reference
-------------
Moved: :cc:`pw_bluetooth`

Module Configuration Options
============================
The following configurations can be adjusted via compile-time configuration of
this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

.. c:macro:: PW_BLUETOOTH_DESCRIPTOR_VEC_SIZE

  Controls the size of ``Vector<Descriptor>`` in the above APIs.

-------------------------
Emboss Packet Definitions
-------------------------
``pw_bluetooth`` contains `Emboss <https://github.com/google/emboss>`_ packet
definitions. So far, some packets and enums from the following protocols are
defined:

- HCI
- L2CAP
- H4

.. _module-pw_bluetooth-emboss-usage:

Usage
=====
1. Add Emboss to your project as described in
   :ref:`module-pw_third_party_emboss`.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      2. Add the Emboss target you require to your build target's dependency
         list - for example, ``@pigweed//pw_bluetooth:emboss_hci_group`` for HCI
         packets.

   .. tab-item:: GN
      :sync: gn

      2. Add the Emboss target you require to your build target's dependency
         list - for example, ``$dir_pw_bluetooth/emboss_hci_group`` for HCI
         packets.

   .. tab-item:: CMake
      :sync: cmake

      2. Add the Emboss target you require to your build target's dependency
         list - for example, ``pw_bluetooth.emboss_hci_group`` for HCI packets.


3. Include the generated header files you require in your source files.

.. code-block:: cpp

   #include "pw_bluetooth/hci_android.emb.h"
   #include "pw_bluetooth/hci_commands.emb.h"

.. inclusive-language: disable

4. Construct an Emboss view over a buffer as described in the `Emboss User Guide
   <https://github.com/google/emboss/blob/master/doc/guide.md>`_.

.. inclusive-language: enable

.. literalinclude:: emboss_test.cc
   :language: cpp
   :start-after: [pw_bluetooth-examples-make_view]
   :end-before: [pw_bluetooth-examples-make_view]

.. note::

   When you first add generated headers, ``clangd`` may complain that the
   generated header files do not exist. You need to build your project to
   resolve this. Similarly, you need to rebuild in order for .emb file updates
   to be reflected in the generated headers.

.. _module-pw_bluetooth-emboss-contributing:

Contributing
============
Emboss ``.emb`` files can be edited to add additional packets and enums.

Emboss files should be formatted by running the following from the Pigweed root.

.. code-block:: bash

   (export EMBOSS_PATH="environment/packages/emboss" &&
       export PYTHONPATH+=":${EMBOSS_PATH}" &&
       python3 "${EMBOSS_PATH}/compiler/front_end/format.py" pw_bluetooth/public/pw_bluetooth/*.emb)

If adding files, be sure to update the GN, Bazel, and CMake build rules.
Presubmit runs the ``emboss_test.cc`` test on all three.


.. _module-pw_bluetooth-snoop-log:

---------
Snoop Log
---------
``pw_bluetooth`` contains an HCI snoop log implementation for recording Rx/Tx
HCI traffic.

The snoop log is easy to integrate into your H4 Uart driver.

* Add `pw::bluetooth::SnoopBuffer` to your H4 Uart class.
* Add a call to `pw::bluetooth::SnoopBuffer::AddRx()` in your H4Packet Receive function.
* Add a call to `pw::bluetooth::SnoopBuffer::AddTx()` in your H4Packet Transmit function.
* Dump the snoop log via `pw::bluetooth::Snoop::Dump()`
* Dump the snoop log to the logger via `pw::bluetooth::Snoop::DumpToLog()`

.. code-block:: cpp

   class H4Uart {
    public:
     void OnH4PacketReceived(proxy::H4PacketInterface& hci_packet) {
       snoop_.addRx(packet);
     }
     void OnH4PacketTransmitted(proxy::H4PacketInterface& hci_packet) {
       snoop_.addTx(packet);
     }
     void GetSnoopLog() { snoop_.DumpToLog(); }

    private:
     pw::bluetooth::SnoopBuffer</*kTotalSize=*/4096, /*kMaxHciPacketSize*/ 128>
         snoop_{pw::chrono::VirtualSystemClock::RealClock()};
   };

Tracing
=======

If tracing is enabled (``PW_TRACE_ENABLE`` is non-zero), the snoop logs will be
captured as instant events (see :ref:`module-pw_trace`) tagged with packet
information (type, size, direction, etc).

+----------+---------+--------------------+---------+------------+--------------------------------------------+
| Type     | Field 1 | Field 2            | Field 3 | Field 4    | Field 5                                    |
+==========+=========+====================+=========+============+============================================+
|          |         |                    |         |            | Subevent Code                              |
| EVENT    |         |                    |         | Event Code |                                            |
|          |         |                    |         |            | (Only for LE Meta and Vendor Debug events, |
|          |         |                    |         |            | otherwise empty)                           |
+----------+         | Direction          |         +------------+--------------------------------------------+
| COMMAND  | Packet  |                    | Packet  | Opcode     | <EMPTY>                                    |
+----------+ Type    | (``0``: incoming,  | Size    +------------+--------------------------------------------+
| ACL_DATA |         | ``1``: outcoming)  |         | Handle     | Packet Boundary Flag                       |
+----------+         |                    |         +------------+--------------------------------------------+
| Others   |         |                    |         | <EMPTY>    | <EMPTY>                                    |
+----------+---------+--------------------+---------+------------+--------------------------------------------+


---------
HCI Utils
---------
``pw_bluetooth`` contains a header with HCI utility functions.
See :cc:`pw::bluetooth::GetHciHeaderSize` and
:cc:`pw::bluetooth::GetHciPayloadSize`.

-----------
Size Report
-----------
Delta when constructing the first packet view and reading/writing a
field. This includes the runtime library and the 4-byte buffer.

.. include:: emboss_size_report

Delta when adding a second packet view and reading/writing a field.

.. include:: emboss_size_report_diff

-------
Roadmap
-------
- Add automated Emboss file formatting to `pw format` (:bug:`331195584`)
- Create a backend for the Bluetooth API using Fuchsia's Bluetooth stack
  (Sapphire).
- Stabilize the Bluetooth API.
- Add BR/EDR APIs.
