.. _module-pw_clock_tree:

================
pw_clock_tree
================
.. pigweed-module::
   :name: pw_clock_tree

   - **Constinit compatible**: Clock tree elements can be declared as ``constinit``
   - **Platform independent**: Clock tree management sits above the platform layer
   - **Handles complex topologies**: Clock tree elements can be used to model
     complex clock tree topologies

``pw_clock_tree`` provides class definitions to implement a platform
specific clock tree management solution. By passing the clock tree element
that clocks a particular device to the corresponding device driver, the
device driver can ensure that the clock is only enabled when required to
maximize power savings.

For example the clock tree functionality could be integrated into a uart device driver
like this:

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-IntegrationIntoDeviceDriversClassDef]
   :end-before: // Device constructor that optionally accepts

It could be initialized and used like this:

.. literalinclude:: examples.cc
   :language: cpp
   :linenos:
   :start-after: [pw_clock_tree-examples-IntegrationIntoDeviceDriversUsage]
   :end-before: [pw_clock_tree-examples-IntegrationIntoDeviceDriversUsage]

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Quickstart
      :link: module-pw_clock_tree-examples
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Examples that show how to integrate a clock tree into a device driver,
      and how to define and use platform specific clock tree elements.

   .. grid-item-card:: :octicon:`code-square` Reference
      :link: ../api/cc/group__pw__clock__tree.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      API references for ``pw::clock_tree::Element``,
      ``pw::clock_tree::DependentElement``, and more.


--------
Overview
--------
Pigweed's clock tree module provides the ability to represent the clock tree of an embedded device,
and to manage the individual clocks and clock tree elements.

.. cpp:namespace-push:: pw::clock_tree::Element

The :cc:`Element <pw::clock_tree::Element>` abstract base class is the root of
the pw_clock_tree class hierarchy, and represents a single node in the clock
tree.

The public (consumer) API includes two basic methods which apply to all clock tree elements:

* :cc:`Acquire <pw::clock_tree::Element::Acquire>`
* :cc:`Release <pw::clock_tree::Element::Release>`

The :cc:`Element <pw::clock_tree::Element>` abstract class implements basic
reference counting methods :cc:`IncRef <pw::clock_tree::Element::IncRef>` and
:cc:`DecRef <pw::clock_tree::Element::DecRef>`, but requires derived classes to
use the reference counting methods to properly acquire and release references to
clock tree elements.

Three derived abstract classes are defined for :cc:`Element
<pw::clock_tree::Element>`:

.. cpp:namespace-pop::

.. cpp:namespace-push:: pw::clock_tree

* :cc:`ElementBlocking <pw::clock_tree::ElementBlocking>` for clock tree
  elements that might block when acquired or released. Acquiring or releasing
  such a clock tree element might fail.
* :cc:`ElementNonBlockingCannotFail
  <pw::clock_tree::ElementNonBlockingCannotFail>` for clock tree elements that
  will not block when acquired or released. Acquiring or releasing such a clock
  tree element will always succeed.
* :cc:`ElementNonBlockingMightFail
  <pw::clock_tree::ElementNonBlockingMightFail>` for clock tree elements that
  will not block when acquired or released. Acquiring or releasing such a clock
  tree element might fail.

:cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>` and
:cc:`ElementNonBlockingMightFail <pw::clock_tree::ElementNonBlockingMightFail>`
elements can be acquired from an interrupt context.

All classes representing a clock tree element should be implemented as a
template and accept :cc:`ElementBlocking <pw::clock_tree::ElementBlocking>`,
:cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>` or
:cc:`ElementNonBlockingMightFail <pw::clock_tree::ElementNonBlockingMightFail>`
as a template argument. Only if it is known at compile time that a clock tree
element can only be either blocking or non-blocking, one can directly derive the
class from :cc:`ElementBlocking <pw::clock_tree::ElementBlocking>` or
:cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>` /
:cc:`ElementNonBlockingMightFail <pw::clock_tree::ElementNonBlockingMightFail>`,
respectively.

.. cpp:namespace-pop::

.. cpp:namespace-push:: pw::clock_tree::OptionalElement

For convenience, :cc:`OptionalElement <pw::clock_tree::OptionalElement>`
represents a null-safe pointer to an :cc:`Element <pw::clock_tree::Element>`.
This allows drivers to easily support an optional :cc:`Element
<pw::clock_tree::Element>` pointer argument.

If it does not point to an `Element`, then its :cc:`Acquire
<pw::clock_tree::OptionalElement::Acquire>` and :cc:`Release
<pw::clock_tree::OptionalElement::Release>` methods do nothing and return
``pw::OkStatus()``.

.. cpp:namespace-pop::

---------------
Synchronization
---------------
.. cpp:namespace-push:: pw::clock_tree

:cc:`Element <pw::clock_tree::Element>` objects are individually internally
synchronized.

* :cc:`ElementBlocking <pw::clock_tree::ElementBlocking>` objects use a `Mutex`.
* :cc:`ElementNonBlockingCannotFail
  <pw::clock_tree::ElementNonBlockingCannotFail>` and
  :cc:`ElementNonBlockingMightFail
  <pw::clock_tree::ElementNonBlockingMightFail>` objects use an
  `InterruptSpinLock`.

.. cpp:namespace-pop::

----------------------
Dependency enforcement
----------------------
.. cpp:namespace-push:: pw::clock_tree

The :cc:`DependentElement <pw::clock_tree::DependentElement>` class enforces the
dependency rules between the :cc:`ElementBlocking
<pw::clock_tree::ElementBlocking>`, :cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>`,
:cc:`ElementNonBlockingMightFail <pw::clock_tree::ElementNonBlockingMightFail>`
and generic :cc:`Element <pw::clock_tree::Element>` classes.

:cc:`ElementBlocking <pw::clock_tree::ElementBlocking>` can depend on an object
from any class, while :cc:`ElementNonBlockingMightFail
<pw::clock_tree::ElementNonBlockingMightFail>` can only depend on objects of
classes :cc:`ElementNonBlockingMightFail
<pw::clock_tree::ElementNonBlockingMightFail>` and
:cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>`.
:cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>` can only depend on objects of
class :cc:`ElementNonBlockingCannotFail
<pw::clock_tree::ElementNonBlockingCannotFail>`.

When one wants to pass a specific object as a source object of class
:cc:`Element <pw::clock_tree::Element>`, the dependent object must be of class
:cc:`ElementBlocking <pw::clock_tree::ElementBlocking>` to satisfy the
dependency rules.


.. cpp:namespace-pop::

.. toctree::
   :hidden:
   :maxdepth: 1

   examples
   backends
