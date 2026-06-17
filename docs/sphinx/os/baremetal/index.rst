.. _docs-os-baremetal:

==========
Bare metal
==========
Pigweed has support for running on target hardware directly without an
operating system or scheduler (bare metal).

----------------------
Bare metal boot & setup
----------------------
Running bare metal requires target-specific boot configurations and startup
logic to initialize the CPU and memory.  The following modules provide helper
libraries for baremetal boot:

* :ref:`module-pw_boot`
* :ref:`module-pw_boot_cortex_m`

See :ref:`baremetal-backends` for more information.

.. toctree::
   :maxdepth: 1
   :hidden:

   backends
