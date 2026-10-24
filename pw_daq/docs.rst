.. _module-pw_daq:

======
pw_daq
======
.. pigweed-module::
   :name: pw_daq

--------
Overview
--------
The ``pw_daq`` module provides libraries and interfaces for interacting with
Data Acquisition (DAQ) devices via a common, abstract interface.

"DAQ" is a common term for hardware which enables measuring (and often
interacting with) physical signals from a device under test (DUT). They are
often used in hardware-in-the-loop (HIL) test setups or during hardware and/or
software development.

Some examples of popular DAQ devices are
`National Instruments <https://www.ni.com/en/shop/data-acquisition.html>`_ or
`LabJack <https://labjack.com/collections/daq-devices>`_.

.. grid:: 2

   .. grid-item-card:: :octicon:`terminal` Using daq-cli
      :link: module-pw_daq-daq-cli
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **For DAQ Users.**
      How to get started with a DAQ by using ``daq-cli`` -- for manual or agent
      interactions with hardware.

   .. grid-item-card:: :octicon:`code-square` Using daq-lib
      :link: module-pw_daq-daq-lib
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **For DAQ Users.**
      How to get started with a DAQ programmatically -- for writing tests or
      other automation.

.. grid:: 2

   .. grid-item-card:: :octicon:`info` Module Design
      :link: module-pw_daq-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      **For Backend Developers & Advanced Users.**
      Details on the design of the abstract DAQ.

   .. grid-item-card:: :octicon:`info` Backend Design
      :link: module-pw_daq-backends
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      **For Backend Developers & Advanced Users.**
      Details on the design of the DAQ concrete backends.

.. toctree::
   :hidden:
   :maxdepth: 1

   Design <design>
   daq_cli
   daq_lib
   Backends <backends/docs>
