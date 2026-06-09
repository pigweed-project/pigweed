.. _module-pw_elf:

.. cpp:namespace-push:: pw::elf

======
pw_elf
======
.. pigweed-module::
   :name: pw_elf

``pw_elf`` provides support for interact with Executable and Linkable Format
(ELF) files.

.. note::

   This module is currently very limited, primarily supporting other Pigweed
   modules. Additional functionality (e.g. iterating sections, segments) may be
   added in the future.

------
Guides
------

Read an ELF section into a buffer
=================================

.. literalinclude:: examples/reader.cc
   :language: cpp
   :linenos:
   :lines: 15-

-------------
API reference
-------------
Moved: :cc:`pw_elf`

------
Python
------
The ``pw_elf`` Python package provides utilities to programmatically build
ELF32 and ELF64 binary files from scratch. See :ref:`module-pw_elf-python` for
details.

.. toctree::
   :hidden:
   :maxdepth: 1

   python
