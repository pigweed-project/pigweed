.. _module-pw_sys_io_zephyr:

================
pw_sys_io_zephyr
================
.. pigweed-module::
   :name: pw_sys_io_zephyr

--------
Overview
--------
This sys I/O backend implements the ``pw_sys_io`` facade. It's controlled by
``choice PIGWEED_SYS_IO_BACKEND`` (see :ref:`zephyr-backend-selection` for
how these choices work) and defaults to "none": the backend registers
itself with ``SYS_INIT``, so selecting it always links it in and
initializes the console at boot.

* ``CONFIG_PIGWEED_SYS_IO=y`` -- link Pigweed's Zephyr backend. I/O
  operations are routed to Zephyr's console. Additionally, it is possible
  to enable the USB subsystem by setting ``CONFIG_PIGWEED_SYS_IO_USB=y``.
* ``CONFIG_PIGWEED_SYS_IO_NONE=y`` -- don't link a backend.

Select ``CONFIG_PIGWEED_SYS_IO=y`` if you use ``pw_sys_io``, directly or
through a module such as :ref:`module-pw_hdlc`; otherwise CMake fails the
build naming the ``pw_sys_io`` facade.

The I/O backend initializes during Zephyr's ``APPLICATION`` level and uses
``CONFIG_PIGWEED_SYS_IO_INIT_PRIORITY`` to set the priority level. This config
value defaults to 1, but is configurable via Kconfig.
