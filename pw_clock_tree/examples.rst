.. _module-pw_clock_tree-examples:

--------
Examples
--------
.. pigweed-module-subpage::
   :name: pw_clock_tree

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Integration into device driver
      :link: module-pw_clock_tree-example-device_driver
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Example that shows how to integrate the clock tree functionality
      into a device driver.

   .. grid-item-card:: :octicon:`code-square` Clock tree usage
      :link: module-pw_clock_tree-example-clock_tree_usage
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Example that shows how to define platform specific clock tree elements
      and how to interact with them to manage clocks of an embedded system.


.. _module-pw_clock_tree-example-device_driver:

Clock tree integration into device drivers
==========================================

The example below shows how the clock tree functionality can get integrated into a new
device driver that requires that a clock tree abstraction is present in the system.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-IntegrationIntoDeviceDriversClassDef]
   :end-before: [pw_clock_tree-examples-IntegrationIntoDeviceDriversClassDef]


.. _module-pw_clock_tree-example-clock_tree_usage:

Definition and use of clock tree elements
=========================================

For the example below we define a clock tree with a clock source ``clock_a``
which is an input into divider ``clock_divider_d``.

.. mermaid::

    flowchart LR
          A(clock_a)--> D(clock_divider_d)

.. cpp:namespace-push:: pw::clock_tree::Element

Derived ``ClockSourceExample`` class template that overrides
:cc:`DoEnable <pw::clock_tree::Element::DoEnable>` and :cc:`DoDisable
<pw::clock_tree::Element::DoDisable>` methods.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-ClockSourceExampleDef]
   :end-before: [pw_clock_tree-examples-ClockSourceExampleDef]

Derived ``ClockDividerExample`` class template that overrides
:cc:`DoEnable <pw::clock_tree::Element::DoEnable>` method.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-ClockDividerExampleDef]
   :end-before: [pw_clock_tree-examples-ClockDividerExampleDef]

Declare the clock tree elements.
``clock_divider_d`` depends on ``clock_a``.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-ClockTreeElementsDec]
   :end-before: [pw_clock_tree-examples-ClockTreeElementsDec]

Acquire a reference to ``clock_divider_d``, which will acquire a reference to the dependent source
``clock_a``. When the reference to ``clock_a`` gets acquired, ``clock_a`` gets enabled. Once the
reference to ``clock_a`` has been acquired and it is enabled, ``clock_divider_d`` gets enabled.

.. mermaid::

    flowchart LR
          A(clock_a)--> D(clock_divider_d)
          style A fill:#0f0,stroke:#333,stroke-width:4px
          style D fill:#0f0,stroke:#333,stroke-width:4px

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-AcquireClockDividerD]
   :end-before: [pw_clock_tree-examples-AcquireClockDividerD]

Set the clock divider value while the ``clock_divider_d`` is enabled.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-SetClockDividerDValue]
   :end-before: [pw_clock_tree-examples-SetClockDividerDValue]

Release the reference to the ``clock_divider_d``, which will disable ``clock_divider_d`` before
it releases its reference to ``clock_b`` that gets disabled afterward. At this point all clock
tree elements are disabled.

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-ReleaseClockSelectorC]
   :end-before: [pw_clock_tree-examples-ReleaseClockSelectorC]
