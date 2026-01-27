.. _overview:

========
Overview
========
.. _file a bug: https://pwbug.dev
.. _talk to us on Discord: https://discord.gg/M9NSeTA
.. _Bazel for Embedded: https://blog.bazel.build/2024/08/08/bazel-for-embedded.html
.. _AOSP: https://source.android.com
.. _official supported versions: https://devguide.python.org/versions/
.. _hermetic: https://bazel.build/basics/hermeticity
.. _libusb: https://libusb.info/
.. _hermeticity: https://bazel.build/basics/hermeticity
.. _works on my machine: https://www.daytona.io/definitions/w/works-on-my-machine-syndrome
.. _ASan: https://clang.llvm.org/docs/AddressSanitizer.html
.. _TSan: https://clang.llvm.org/docs/ThreadSanitizer.html
.. _MSan: https://clang.llvm.org/docs/MemorySanitizer.html
.. _Sense: https://cs.opensource.google/pigweed/showcase/sense
.. _BUILD.bazel: https://bazel.build/concepts/build-files
.. _Bazel modules: https://bazel.build/external/module
.. _Bazel module: https://bazel.build/external/module
.. inclusive-language: disable
.. _directly supports: https://github.com/raspberrypi/pico-sdk/blob/master/MODULE.bazel
.. inclusive-language: enable
.. _can be downloaded: https://registry.bazel.build/modules/pico-sdk
.. _non-virtual interface pattern: https://en.wikipedia.org/wiki/Non-virtual_interface_pattern
.. _Bazel Central Registry: https://registry.bazel.build/
.. _Bazel platform: http://bazel.build/extending/platforms
.. _//targets/rp2/BUILD.bazel: https://cs.opensource.google/pigweed/showcase/sense/+/main:targets/rp2/BUILD.bazel
.. _pico-sdk: https://registry.bazel.build/modules/pico-sdk

This overview helps you understand Pigweed and determine if Pigweed is a
good fit for your project.

If you still have questions after reading this overview, please `file a bug`_
or `talk to us on Discord`_. If you prefer email, DM one of the Discord admins
(they're all on the Pigweed team) and they'll kick off an email thread for you.

.. _overview-what:

----------------
What is Pigweed?
----------------
Pigweed is essentially a modular platform for embedded development. By
"modular" we mean that it's possible to adopt only the parts that you need.

The following sections explain the main components of our platform in more
detail. The best way to get a deeper understanding about what Pigweed provides is to
try out the :ref:`Tour of Pigweed <showcase-sense>` and to study the
`Sense`_ repo (the example project that the tour is based off).

.. _overview-who:

---------------------------------
Who is Pigweed's target audience?
---------------------------------
Our :ref:`mission <docs-mission>` is to help large embedded development teams
create world-class products by increasing team velocity, project
sustainability, and product robustness. Having lived through many of these
projects, we know how overwhelmingly complex they can become. Pigweed is
our ambitious attempt to create step change improvements in the state of
the art of embedded device development.

Hobbyist projects are also welcome to adopt Pigweed, but many hobbyists find
Pigweed to be too complex for their needs. Large embedded development teams, on
the other hand, understand that this "complexity" is what it takes to ship
world-class products at scale. See :ref:`showcase-sense-complexity` for
further discussion.

.. note::

   In this overview, a **client** is a team that uses Pigweed in one or
   more of their projects.

.. _overview-platform:

------------------
Pigweed's platform
------------------
These are the main components of our platform.

.. _docs-concepts-embedded-development-libraries:

Embedded development libraries
==============================
Our :ref:`docs-module-guides` enable you to use modern software development
best practices without compromising performance. They are:

* Heavily optimized for resource-constrained hardware.
* Battle-tested in millions of consumer devices.
* Deployed in extreme, high-stakes environments e.g. satellites.
* Designed to work portably on both your development host (via
  :ref:`host-side simulation <overview-platform-development-environment-host>`)
  and a wide variety of embedded hardware systems.

.. _docs-concepts-build-system:

Build system integrations
-------------------------
We make it easy to integrate our modules into most of the build systems
that are popular among large embedded development teams.

Full support:

* Bazel. We recommend Bazel for all new projects because of its thriving
  developer ecosystem and strong `hermeticity`_ guarantees. The Pigweed team
  is very active in making Bazel the best build system for embedded
  development. See :ref:`seed-0111`, `Bazel for Embedded`_, and
  :ref:`docs-blog-07-bazelcon-2024.rst`.

Partial support:

* GN. Pigweed has extensive support for GN, because GN was Pigweed's primary
  build system for many years. However, Pigweed's support for GN is expected
  to decline over the coming years as we focus more on Bazel.

* CMake. We support integrating Pigweed modules into existing CMake projects.
  We don't recommend starting new projects with CMake and can't offer extensive
  support for new projects that choose CMake.

* Zephyr. Although technically CMake-derived, there is an ongoing effort
  to make Pigweed work seamlessly within Zephyr projects.

No support:

* Make.
* Buck2.
* Meson.
* PlatformIO.

.. _docs-concepts-development-environment:

Development environment
=======================
Our Bazel-based development environment reduces new teammate onboarding time
and eliminates the `works on my machine`_ problem. Onboarding time speeds up
because we dramatically simplify the setup of cross-compilation toolchains and
complex tools like ``libusb``. The "works on my machine" problem is mitigated
because of Bazel's strong `hermeticity`_ guarantees. Building your entire
project becomes a literal two-step process:

#. Clone the repo.
#. Run ``bazelisk build //...``

If your project uses one of our other supported build systems,
:ref:`module-pw_env_setup` can help speed up onboarding and
increase the reproducibility of builds, but the hermeticity
guarantees aren't as strong as what Bazel provides.

Supported operating systems
---------------------------
We have robust support for Linux and macOS. We also support Windows, but it
has more sharp edges.

.. _overview-platform-development-environment-host:

Rapid, portable firmware development
====================================
Pigweed has extensive support for host-side :ref:`simulation
<target-host-device-simulator>` and :ref:`testing <module-pw_unit_test>`. Many
Pigweed clients structure their projects in a hardware-agnostic way that allows
them to simulate and test most core business logic on their development hosts.
By the time the product hardware is ready, the only remaining task is to implement
hardware-specific wrappers around the hardware-agnostic core logic.
Benefits of this approach:

* Your software team can remain productive while they're waiting on prototypes
  from hardware teams.
* Unit tests are fast and reliable enough to run after every code iteration.
* It becomes possible to thoroughly test your software against modern code
  analysis tools like `ASan`_, `TSan`_, `MSan`_, and
  :ref:`fuzzers <module-pw_fuzzer>`. These tools usually don't work correctly in
  on-device embedded contexts.

Product lifecycle tooling
=========================
Bringing a product to mass production requires a lot more than just good
firmware. Rather than reimplementing common concerns from scratch for each new
project, Pigweed provides a solid foundation that you can reuse and extend with
much higher velocity across all your projects.

Examples:

* :ref:`module-pw_console`: An interactive console that can be extended
  and customized.
* :ref:`Factory-at-your-desk workflows <showcase-sense-tutorial-factory>`:
  Lightweight testing scripts that combine manual testing and automated
  testing. Useful for small-scale manufacturing runs or for ensuring that
  all teammates follow a precise, reproducible testing workflow.
* :ref:`module-pw_bloat`: Tools for generating size reports.
* :ref:`module-pw_console`: Utilities for creating custom CLI tools.
* :ref:`module-pw_web`: A library for creating custom web apps that
  can communicate with your embedded devices.

.. _overview-journey:

---------------------------
Typical new project journey
---------------------------
Here is the typical journey for starting a new project with Pigweed. We'll
assume that you also need to integrate a vendor SDK such as Espressif's
ESP-IDF, STMicroelectronics's STM32Cube, or Raspberry Pi's C/C++ SDK.

The following sections assume that you'll be using Bazel in your new project.

1. Fork the Sense repo
======================
The first step is to fork `Sense`_, the example project that's used in the
:ref:`Tour of Pigweed <showcase-sense>`. It's a working example of an extensive
Pigweed integration. You can remove any parts that you don't need.

2. Create Bazel files for vendor SDKs and other dependencies
============================================================
Next, add Bazel support for your vendor SDK as needed. For some SDKs, the SDK
may already have support in the Bazel developer ecosystem. For example, the
Raspberry Pi C/C++ SDK `directly supports`_ Bazel and `can be downloaded`_ from
the Bazel Central Registry. For other vendor SDKs, you may need to
create `BUILD.bazel`_ files or package the vendor SDK into a `Bazel module`_.
We encourage you to publish the Bazel module to the `Bazel Central Registry`_
so that the entire Bazel developer ecosystem can benefit from and improve on
your work.

In general, you'll also use `Bazel modules`_ system to pull in other
dependencies as needed. Make sure to check the `Bazel Central Registry`_
to see if someone else has already provided a module for your dependency.

.. _overview-journey-warppers:

3. Create Pigweed wrappers around vendor SDKs
=============================================
Next, create Pigweed wrappers that invoke your vendor SDK as needed. The primary
interfaces for most Pigweed :ref:`modules <docs-glossary-module>` are
hardware-agnostic. Sometimes, an implementation for a particular vendor SDK
already exists in :ref:`docs-glossary-upstream`. If an implementation already
exists in Upstream Pigweed, you're welcome to use that. Otherwise, you'll need
to implement the wrapper yourself. See :ref:`module-pw_spi` for an example of a
hardware-agnostic module and :ref:`pw_spi backends <module-pw_spi-backends>` for
a list of implementations. We encourage Pigweed clients to contribute their
general-purpose implementations for popular vendor SDKs to Upstream Pigweed so
that the whole Pigweed community can benefit from them and improve them. But
that is totally optional. You can keep your implementations private, if needed.

4. Set up device builds
=======================
The last step is to set up toolchains and other tools so that your project
can be built for and flashed onto your particular hardware. In general,
the cross-compilation toolchain is usually set up as a `Bazel platform`_.
See `//targets/rp2/BUILD.bazel`_ from the Sense repo for an example.

----------------------
Supported technologies
----------------------
This section provides more detail about how much (or little) we support
specific build systems, programming languages, etc.

Build system support
====================
See :ref:`docs-concepts-build-system` above.

Drivers and peripherals
=======================
Our driver and peripheral APIs are essentially C++ abstract classes that use
the `non-virtual interface pattern`_. Examples:

* :ref:`ADC <module-pw_analog>`
* :ref:`GPIO <module-pw_digital_io>`
* :ref:`HDLC <module-pw_hdlc>`
* :ref:`I2C <module-pw_i2c>`
* :ref:`RNG <module-pw_random>`
* :ref:`SPI <module-pw_spi>`
* :ref:`UART <module-pw_uart>`

If an implementation exists in :ref:`docs-glossary-upstream` you are welcome to
use that. Visit the docs for the module that provides the generic API (e.g.
:ref:`module-pw_spi`) and then navigate to that module's "backends" or
"implementations" section (e.g. :ref:`pw_spi backends <module-pw_spi-backends>`)
to determine what implementations are already available.

Vendor SDKs
===========
The level of effort required to integrate your vendor SDK into a
Bazel-based Pigweed project depends on:

* The availability of the vendor SDK as a Bazel dependency. Ideally, the
  vendor SDK is already available as a `Bazel module`_ in the
  `Bazel Central Registry`_ (BCR). If not, check if :ref:`docs-glossary-upstream`
  has a target that makes it easy to pull the vendor SDK into your project.
  E.g. :cs:`third_party/stm32cube/BUILD.bazel` provides a target to pull
  STM32Cube into a project. Otherwise, you'll need to set up a
  new Bazel module yourself or create a solution similar to the
  STM32Cube ``BUILD.bazel`` file.

* The availability of Pigweed wrappers around the vendor SDK. As mentioned in
  :ref:`overview-journey-warppers`, the primary interfaces for most Pigweed
  modules are hardware-agnostic. The implementation of that module for
  a particular vendor is usually handled in a separate module. If an
  implementation already exists in :ref:`docs-glossary-upstream`, you're
  welcome to use that. Otherwise, you'll need to roll your own implementation.

The following vendor SDKs are already well supported:

* Raspberry Pi Pico C/C++ SDK
* MCUXpresso
* STM32Cube

Bluetooth
=========
Multiple Pigweed modules provide Bluetooth-related functionality:

* :ref:`module-pw_bluetooth_sapphire`: A full central/peripheral-certified
  Bluetooth stack that has been deployed on millions of consumer devices.
  It's an AP-sized Bluetooth stack that's been made portable but isn't
  yet extensively size-optimized.

* :ref:`module-pw_bluetooth`: A BLE-only API that provides a generic
  interface for Bluetooth that can be implemented by different stacks.
  ``pw_bluetooth_sapphire`` implements this stack but there are also
  wrappers for other stacks. Not all of these other wrappers are public yet.

* :ref:`module-pw_bluetooth_proxy`: Enables proxying Bluetooth packets
  and rerouting L2CAP channels to low-power islands.

Language support
================

C++
---
Pigweed has an extensive collection of C++ libraries. See
:ref:`docs-module-guides`. All Pigweed code requires C++17 and is fully
compatible with C++20 and C++23. We expect to support C++ indefinitely.

Rust
----
Pigweed incrementally adds Rust support for any given module based on client needs.
We plan on growing our Rust support extensively over the next few years. We expect
to support Rust (alongside C++) indefinitely.

.. _docs-concepts-python-version:

Python
------
Python is Pigweed's primary language for scripting tasks. Some Pigweed modules,
such as :ref:`module-pw_console`, can be extended with custom scripts. These
scripts almost always must be written in Python.

Pigweed officially supports Python 3.10 and 3.11. Moving forward, Pigweed will
follow Python's `official supported versions`_. Pigweed will drop support for
Python versions as they reach end-of-life.

Other languages
---------------
Support for other languages are added on a case-by-case basis, depending
on client needs.

----------
Next steps
----------
* The best way to get a deeper understanding about what Pigweed provides is to
  try out the :ref:`Tour of Pigweed <showcase-sense>` and to study the `Sense`_
  (the example project that the tour is based off).

* If you still have questions, please `file a bug`_ or
  `talk to us on Discord`_. If you prefer email, DM one of the Discord
  admins (they're all on the Pigweed team) and they'll kick off an email
  thread for you.
