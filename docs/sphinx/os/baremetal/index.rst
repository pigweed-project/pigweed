.. _docs-os-baremetal:
.. _baremetal-backends:

==========
Bare metal
==========
Pigweed has support for running on target hardware directly without an
operating system or scheduler (bare metal).

-----------------------
Bare metal boot & setup
-----------------------
Running bare metal requires target-specific boot configurations and startup
logic to initialize the CPU and memory.  The following modules provide helper
libraries for baremetal boot:

* :ref:`module-pw_boot`
* :ref:`module-pw_boot_cortex_m`

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
The following table lists all modules related to Bare metal.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sync_baremetal`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Synchronization <../../pw_sync_baremetal/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
