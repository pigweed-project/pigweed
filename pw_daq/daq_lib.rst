.. _module-pw_daq-daq-lib:

=======
daq-lib
=======
.. pigweed-module-subpage::
   :name: pw_daq

The ``pw_daq`` Python library provides a programmatic Python API for interacting
with a supported DAQ device.

``Daq`` wraps a pre-configured ``DaqBackend`` instance and exposes the abstract
DAQ session lifecycle (``open()`` / ``close()`` or ``with Daq(backend):``) and
commands. Any backend-specific device configuration (via
``backend.configure(...)``) must be applied to the ``DaqBackend`` before
passing it to ``Daq``.

.. note::

   ``daq-lib`` enforces the
   :ref:`abstract DAQ specification <module-pw_daq-design-detailed-design>`,
   including supported endpoints, value types, defaults, and attribute
   ``mutability`` traits, and does not expose non-spec attributes or commands.

------
Design
------
TBD
