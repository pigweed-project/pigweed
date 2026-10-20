.. TODO: b/331286149 - Rename this file

.. _module-pw_i2c-impl:

===============
Implementations
===============
.. pigweed-module-subpage::
   :name: pw_i2c

To use the ``pw_i2c`` API, you need to specify in your build system an
implementation of the ``pw_i2c`` virtual interface.

------------------------
Existing implementations
------------------------
The following Pigweed modules are ready-made ``pw_i2c`` implementations that
you can use in your projects:

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_i2c_linux`", ":bdg-primary:`stable`", "Linux userspace implementation for pw_i2c", "C++"
   ":ref:`module-pw_i2c_mcuxpresso`", ":bdg-primary:`stable`", "NXP MCUXpresso SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_i2c_rp2040`", ":bdg-primary:`stable`", "Raspberry Pi Pico SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_i2c_zephyr`", ":bdg-warning:`experimental`", "Zephyr implementation for pw_i2c", ""

.. Do not edit this toctree. To add non-module docs to this page,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   Linux <../pw_i2c_linux/docs>
   MCUXpresso <pw://pw_i2c_mcuxpresso/docs.html>
   Pico SDK <pw://pw_i2c_rp2040/docs.html>
   Zephyr RTOS <pw://pw_i2c_zephyr/docs.html>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

See :ref:`module-pw_i2c-quickstart` for build system configuration examples.

------------------------------
Create your own implementation
------------------------------
If the available implementations don't meet your needs, you can create your
own:

.. _common_pico.cc: https://pigweed.googlesource.com/pigweed/kudzu/+/refs/heads/main/applications/app_common_impl/common_pico.cc

.. note:: :ref:`module-pw_i2c_rp2040` uses the deprecated
   :cc:`pw::i2c::Initiator` API.

   See :ref:`module-pw_i2c_mcuxpresso` and :ref:`module-pw_i2c_mcuxpresso`
   for example implementations of ``DoTransferFor(span<Message>, ...)``.

#. Implement the :cc:`pw::i2c::Initiator` interface. See
   :ref:`module-pw_i2c_rp2040` for an example Raspberry Pi Pico SDK
   implementation and `common_pico.cc`_ for example usage of ``pw_i2c_rp2040``.

#. In your build system, take a dependency on the ``pw_i2c`` API headers. See
   :ref:`module-pw_i2c-quickstart` for build system configuration examples.
