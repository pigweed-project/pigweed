.. _module-pw_interrupt_zephyr:

-------------------
pw_interrupt_zephyr
-------------------
.. pigweed-module::
   :name: pw_interrupt_zephyr

This is the Zephyr RTOS backend for the ``pw_interrupt`` facade. It uses
``k_is_in_isr()`` to determine whether the current context is an interrupt
service routine.

Setup
=====
.. tab-set::

   .. tab-item:: Bazel

      Set the backend flag in your platform definition or build options:

      .. code-block:: bazel

         platform(
             name = "my_zephyr_platform",
             flags = flags_from_dict({
                 # ...
                 "@pigweed//pw_interrupt:backend": "@pigweed//pw_interrupt_zephyr:context",
             }),
         )

      Or via your project's ``.bazelrc``:

      .. code-block:: text

         build --@pigweed//pw_interrupt:backend=@pigweed//pw_interrupt_zephyr:context

   .. tab-item:: Zephyr (CMake / Kconfig)

      Enable the backend in your project's ``prj.conf``:

      .. code-block:: kconfig

         CONFIG_PIGWEED_INTERRUPT_CONTEXT=y
