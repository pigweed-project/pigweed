.. _module-pw_interrupt_cortex_m:

---------------------
pw_interrupt_cortex_m
---------------------
.. pigweed-module::
   :name: pw_interrupt_cortex_m

Pigweed's interrupt Cortex-M module provides architecture-specific
backends for ``pw_interrupt``.

Setup
=====
.. tab-set::

   .. tab-item:: Bazel

      Add ``@pigweed//pw_interrupt_cortex_m:compatible`` to your
      platform's ``constraint_values`` and set the backend implementation flag
      in your platform ``flags`` (or via the command line):

      .. code-block:: bazel

         platform(
             name = "my_platform",
             constraint_values = [
                 # ...
                 "@pigweed//pw_interrupt_cortex_m:compatible",
             ],
             flags = flags_from_dict({
                 # ...
                 "@pigweed//pw_interrupt:backend": "//pw_interrupt_cortex_m:context",
             }),
         )

   .. tab-item:: GN

      Set the ``pw_interrupt_CONTEXT_BACKEND`` build argument in your toolchain
      or target configuration:

      .. code-block:: py

         _target_config = {
           # ...
           pw_interrupt_CONTEXT_BACKEND = "$dir_pw_interrupt_cortex_m:context"
         }

   .. tab-item:: CMake

      Call ``pw_set_backend`` to set the backend in your toolchain file or
      top-level ``CMakeLists.txt``:

      .. code-block:: cmake

         pw_set_backend(pw_interrupt.context pw_interrupt_cortex_m.context)
