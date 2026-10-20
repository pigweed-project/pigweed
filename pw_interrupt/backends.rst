.. _module-pw_interrupt-backends:

========
Backends
========
Pigweed provides several backends for the ``pw_interrupt`` facade:

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

   ":ref:`module-pw_interrupt_cortex_m`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_interrupt_freertos`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_interrupt_zephyr`", ":bdg-secondary:`unstable`", "", ""

.. Do not edit this toctree. To add non-module docs to this page,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   Cortex-M <../pw_interrupt_cortex_m/docs>
   FreeRTOS <pw://pw_interrupt_freertos/docs.html>
   Zephyr <pw://pw_interrupt_zephyr/docs.html>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

.. toctree::
   :maxdepth: 1

   Fake (test-only) <fake_backend>
