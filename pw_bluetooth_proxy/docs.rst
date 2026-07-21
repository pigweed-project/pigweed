.. _module-pw_bluetooth_proxy:

==================
pw_bluetooth_proxy
==================
.. pigweed-module::
   :name: pw_bluetooth_proxy

The ``pw_bluetooth_proxy`` module provides a lightweight proxy host that
can be placed between a Bluetooth host and Bluetooth controller to add
additional functionality or inspection. All without modifying the host or the
controller.

An example use case could be offloading some functionality from a main host
located on the application processor to instead be handled on the MCU (to reduce
power usage).

The proxy acts as a proxy of all host controller interface (HCI) packets between
the host and the controller.

:cc:`pw::bluetooth::proxy::ProxyHost` acts as the main coordinator for
proxy functionality.

.. literalinclude:: proxy_host_test.cc
   :language: cpp
   :start-after: [pw_bluetooth_proxy-examples-basic]
   :end-before: [pw_bluetooth_proxy-examples-basic]

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get Started
      :link: module-pw_bluetooth_proxy-getstarted
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      How to set up in your build system

   .. grid-item-card:: :octicon:`code-square` API Reference
      :link: module-pw_bluetooth_proxy-reference
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Reference information about the API

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Roadmap
      :link: module-pw_bluetooth_proxy-roadmap
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Upcoming plans

   .. grid-item-card:: :octicon:`code-square` Code size analysis
      :link: module-pw_bluetooth_proxy-size-reports
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand code footprint and savings potential


.. _module-pw_bluetooth_proxy-getstarted:

-----------
Get started
-----------
.. repository: https://bazel.build/concepts/build-ref#repositories

1. Add Emboss to your project as described in
   :ref:`module-pw_third_party_emboss`.

.. tab-set::

   .. tab-item:: Bazel

      Bazel isn't supported yet.

   .. tab-item:: GN
      :selected:

      2. Then add ``$dir_pw_bluetooth_proxy`` to
      the ``deps`` list in your ``pw_executable()`` build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             "$dir_pw_bluetooth_proxy",
             # ...
           ]
         }

   .. tab-item:: CMake

      2. Then add ``pw_bluetooth_proxy`` to
      the ``DEPS`` list in your cmake target:

Module configuration
====================
This module has configuration options that globally affect the behavior of the
:cc:`pw::bluetooth::proxy::ProxyHost` via compile-time configuration. See the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

Module configuration options include:

- :cc:`PW_BLUETOOTH_PROXY_ASYNC`: This module supports two modes of operation,
  both of which provide thread-safe APIs. When this option is zero (the
  default), it will use synchronization primitives to allow for parallel
  execution. When this option is non-zero, it will asynchronously execute tasks
  using a provided :cc`pw::async2::Dispatcher`. When using this mode of
  operation, an allocator and a dispatcher must be provided that outlive the
  :cc:`pw::bluetooth::proxy::ProxyHost`.
- :cc:`PW_BLUETOOTH_PROXY_MULTIBUF_ALLOCATOR_SIZE`: The size of the temporary
  internal multibuf allocator, which is used by the ChannelProxy and RFCOMM
  APIs. Configure this value to be large enough to allocate multiple RFCOMM
  packets if you are using RFCOMM.
- :cc:`PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES`: Whether
  :cc:`pw::bluetooth::proxy::ProxyHost` emits incremental state update
  callbacks on dynamic credit mutations (defaults to 1). Disabling reduces
  power consumption for high-throughput streams.

Clock Facade
============
This module uses a facade for time-related operations to allow overriding the
clock implementation. By default, the facade uses :cc:`pw::chrono::SystemClock`
and is backed by :cc:`pw::async2::GetSystemTimeProvider`.

To override the backend, set the appropriate variable in your build system:
- **GN:** ``pw_bluetooth_proxy_CLOCK_BACKEND``
- **CMake:** ``pw_bluetooth_proxy.clock_BACKEND``
- **Bazel:** Use the ``clock_backend`` label flag.

HCI Event Filtering
===================
``ProxyHost`` supports runtime configuration of HCI event and LE Meta subevent
filtering. This allows the container to block specific events from reaching the
host.

By default, no events are blocked.

To block or unblock an event, use:
- :cc:`pw::bluetooth::proxy::ProxyHost::SetEventBlocked`
- :cc:`pw::bluetooth::proxy::ProxyHost::SetLeSubeventBlocked`

To query the block status of an event, use:
- :cc:`pw::bluetooth::proxy::ProxyHost::IsEventBlocked`
- :cc:`pw::bluetooth::proxy::ProxyHost::IsLeSubeventBlocked`

To reset all filters (unblock all events), use:
- :cc:`pw::bluetooth::proxy::ProxyHost::ResetFilters`

.. note::
   Blocking the parent ``LE_META_EVENT`` will block all LE Meta subevents,
   regardless of their individual block status.

.. note::
   Resetting the proxy via :cc:`pw::bluetooth::proxy::ProxyHost::Reset` does
   *not* reset the filters. To reset filters, you must explicitly call
   :cc:`pw::bluetooth::proxy::ProxyHost::ResetFilters`.

.. _module-pw_bluetooth_proxy-reference:

-------------
API reference
-------------
Moved: :cc:`pw_bluetooth_proxy`

.. _module-pw_bluetooth_proxy-size-reports:

------------------
Code size analysis
------------------
Delta when constructing a proxy and just sending packets through.

.. include:: use_passthrough_proxy_size_report

.. _module-pw_bluetooth_proxy-roadmap:

-------
Roadmap
-------
- ACL flow control
- Sending GATT notifications
- CMake support
- Receiving GATT notifications
- Taking ownership of a L2CAP channel
- Bazel support
- And more...
