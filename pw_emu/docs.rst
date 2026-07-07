.. _module-pw_emu:

.. rst-class:: with-subtitle

======
pw_emu
======
.. pigweed-module::
   :name: pw_emu

* **Declarative**. Define emulation targets in JSON. A target encapsulates the
  emulated machine, tools, and host channels configuration.
* **Flexible**. Manage multiple emulator instances over a CLI or Python API.
* **Unopinionated**. Use QEMU or Renode, or extend ``pw_emu`` to support your
  favorite emulator.
* **Configurable**. Expose channels for debugging and monitoring through
  configurable host resources like sockets.

Declaratively configure an emulation target like this:

.. code-block:: json

   {
     "targets": {
       "qemu-lm3s6965evb": {
         "gdb": [
           "arm-none-eabi-gdb"
         ],
         "qemu": {
           "executable": "qemu-system-arm",
           "machine": "lm3s6965evb",
           "channels": {
             "chardevs": {
               "serial0": {
                 "id": "serial0"
               }
             }
           }
         }
       }
     }
   }

Then run a binary like this!

.. code-block:: console

   pw emu run --args=-no-reboot qemu-lm3s6965evb \
       out/lm3s6965evb_qemu_gcc_size_optimized/obj/pw_snapshot/test/cpp_compile_test

.. pw_emu-nav-start

.. grid:: 1

   .. grid-item-card:: :octicon:`rocket` Get started & guides
      :link: module-pw_emu-guide
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      How to set up and use ``pw_emu``

.. grid:: 2

   .. grid-item-card:: :octicon:`terminal` CLI reference
      :link: module-pw_emu-cli
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Reference details about the ``pw_emu`` command line interface

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: module-pw_emu-api
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Reference details about the ``pw_emu`` Python API

.. grid:: 2

   .. grid-item-card:: :octicon:`gear` Configuration
      :link: module-pw_emu-config
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Reference details about ``pw_emu`` declarative configuration

   .. grid-item-card:: :octicon:`stack` Design
      :link: module-pw_emu-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Design details about ``pw_emu``

.. grid:: 2

   .. grid-item-card:: :octicon:`comment-discussion` SEED-0108: Emulators Frontend
      :link: seed-0108
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      The RFC explaining the initial design and motivations for ``pw_emu``

   .. grid-item-card:: :octicon:`code-square` Source code
      :link: seed-0108
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Source code for ``pw_emu``

.. pw_emu-nav-end

.. toctree::
   :hidden:
   :maxdepth: 1

   guide
   cli
   api
   config
   design
   SEED-0108 <pw://seed/0108.html>
