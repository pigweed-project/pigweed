.. _docs-root:

=======
Pigweed
=======
*Sustained, robust, and rapid embedded product development for large teams*

-----------
Get started
-----------
.. raw:: html

   <!-- Add a little space between the H2 and the cards. This is an edge case
        that only occurs on the homepage so it's best to just hack the fix
        locally here. -->
   <br>

.. grid:: 2

   .. grid-item-card:: :octicon:`info` Overview
      :link: overview
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Understand the core components of Pigweed and determine whether Pigweed
      is a good fit for your project.

   .. grid-item-card:: :octicon:`rocket` Tour of Pigweed
      :link: showcase-sense
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Explore key Pigweed features, such as hermetic building, full C++
      code intelligence in VS Code, communicating with devices over RPC,
      host-side and on-device unit tests, and lots more in a guided
      walkthrough.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Bazel quickstart
      :link: https://cs.opensource.google/pigweed/quickstart/bazel
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Fork our minimal, Bazel-based starter project to create a new
      Pigweed project from scratch. The project includes a basic
      blinky LED program that runs on Raspberry Pi Picos and can
      be simulated on your development host.

   .. grid-item-card:: :octicon:`list-ordered` More get started guides
      :link: docs-get-started
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Set up a C++-based Zephyr project, set up GitHub Actions,
      integrate Pigweed into an existing Bazel project, and more.

.. _docs-root-changelog:

----------------------
What's new in Pigweed?
----------------------
.. include:: changelog/2026/09.rst
   :start-after: .. changelog_highlights_start
   :end-before: .. changelog_highlights_end

And more! See :doc:`changelog/2026/09`.

-----------------
Platform overview
-----------------
Pigweed :ref:`modules <docs-module-guides>` are building blocks and infrastructure
that enable faster and more reliable embedded development.

.. toctree::
   :maxdepth: 1
   :hidden:

   overview
   get_started/index
   Tour <showcases/sense/index>
   platform
   Contributing <contributing/index>
   community/index
   api/index

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

.. grid:: 3

   .. grid-item-card:: :octicon:`broadcast` Comms
      :link: comms/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Remote procedure calls (RPC), channels, and communication streams

   .. grid-item-card:: :octicon:`eye` Observability
      :link: observability/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Structured logging, tokenization, metrics, crash handling, and tracing

   .. grid-item-card:: :octicon:`sync` Async & concurrency
      :link: async/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Cooperative multitasking, futures, and asynchronous primitives

.. grid:: 3

   .. grid-item-card:: :octicon:`file-binary` Encoding & serialization
      :link: encoding/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Protocol Buffers, JSON, Varint, Base64, and wire format serialization

   .. grid-item-card:: :octicon:`cpu` OS abstraction layers
      :link: os/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Synchronization, threading, and portable RTOS abstractions

   .. grid-item-card:: :octicon:`server` Kernel
      :link: kernel/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Embedded kernels, preemptive scheduling, and userspace isolation

.. grid:: 3

   .. grid-item-card:: :octicon:`database` Memory & storage
      :link: memory/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Memory allocators, buffers, persistent flash storage, and key-value stores

   .. grid-item-card:: :octicon:`plug` HALs
      :link: hal/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Hardware abstraction layer interfaces for timers, I2C, SPI, UART, and GPIO

   .. grid-item-card:: :octicon:`package-dependencies` Hardware support
      :link: hw/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Board support, hardware drivers, and SDK integrations for target platforms

.. grid:: 3

   .. grid-item-card:: :octicon:`code` Data structures & utilities
      :link: core/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Embedded-safe collections, spans, strings, status, and error handling

   .. grid-item-card:: :octicon:`checklist` Testing
      :link: test/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Unit testing frameworks, mocking, compilation tests, and fuzzing

   .. grid-item-card:: :octicon:`workflow` Build systems
      :link: build/index
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Bazel, GN, and CMake integrations and rules

And more! See :ref:`docs-module-guides`.

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

--------------------
Who's using Pigweed?
--------------------
.. _satellites: https://www.spinlaunch.com/
.. _autonomous aerial drones: https://www.flyzipline.com/

.. TODO: b/358432838 - Update this section once the banned word list is fixed.

Pigweed has shipped in millions of devices, including Google's suite of Pixel
devices, Nest thermostats, `satellites`_, and `autonomous aerial drones`_.

--------------------
Showcase: pw_console
--------------------
:ref:`module-pw_console` is our multi-purpose, pluggable REPL and log viewer.
It's designed to be a complete development and manufacturing solution for
interacting with hardware devices via :ref:`module-pw_rpc` over a
:ref:`module-pw_hdlc` transport. Gone are the days of hacking together a REPL
and log viewer for each new project!

.. figure:: https://www.gstatic.com/pigweed/pw_console/python_completion.png
   :alt: Using pw_console to interact with a device

   Using pw_console to interact with a device

----------
Talk to us
----------
.. raw:: html

   <!-- Add a little space between the H2 and the cards. This is an edge case
        that only occurs on the homepage so it's best to just hack the fix
        locally here. -->
   <br>

.. grid:: 1

   .. grid-item-card:: :octicon:`comment-discussion` Chat room
      :link: https://discord.gg/M9NSeTA
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      For real-time discussion with the Pigweed team, head over to our Discord.

.. grid:: 2

   .. grid-item-card:: :octicon:`device-camera-video` Monthly community meeting
      :link: https://groups.google.com/g/pigweed
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      .. pigweed-live::

   .. grid-item-card:: :octicon:`bug` Issues
      :link: https://pwbug.dev
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Found a bug? Got a feature request? Please create a new issue in our
      tracker.
