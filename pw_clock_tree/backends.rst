.. _module-pw_clock_tree-impl:

===============
Implementations
===============
.. pigweed-module-subpage::
   :name: pw_clock_tree

To use the ``pw_clock_tree`` API, you need to specify in your build system an
implementation of the platform specific implementation of the ``pw_clock_tree``
virtual interface.

------------------------
Existing implementations
------------------------
The following Pigweed modules are ready-made ``pw_clock_tree`` implementations that
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

   ":ref:`module-pw_clock_tree_mcuxpresso`", ":bdg-secondary:`unstable`", "NXP MCUXpresso SDK implementation for pw_clock_tree", "C++"

.. Do not edit this toctree. To add non-module docs to this page,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   MCUXpresso <pw://pw_clock_tree_mcuxpresso/docs.html>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
