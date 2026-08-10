.. _docs-bazel-compatibility:

==========================================
Pigweed Bazel build compatibility patterns
==========================================
This document describes the Bazel patterns Pigweed uses to express that a build
target is compatible with a platform. The main motivation is to enable
maintainable :ref:`wildcard builds <docs-bazel-compatibility-why-wildcard>` of
upstream Pigweed for non-host platforms:

.. code-block:: sh

   bazelisk build --config=rp2040 //...

The bulk of this document describes :ref:`recommended patterns
<docs-bazel-compatibility-recommended>` for expressing compatibility in various
scenarios. For context, we also discuss :ref:`alternative patterns
<docs-bazel-compatibility-not-recommended>` and why they should be avoided.
For the implementation plan, see
:ref:`docs-bazel-compatibility-implementation-plan`.

See :ref:`docs-bazel-compatibility-background` and the `Platforms documentation
<https://bazel.build/extending/platforms>`_ for more context.

-----------------
Intended audience
-----------------
This document is targeted at *upstream Pigweed developers*. The patterns
described here are suitable for downstream projects, too, but downstream
projects can employ a broader variety of approaches. Because Pigweed is
middleware that must preserve users' flexibility in configuring it, we need to
be more careful.

This document assumes you're familiar with regular Bazel usage, but perhaps not
Bazel's build configurability primitives.

.. _docs-bazel-compatibility-recommended:

----------------------------------
Recommended compatibility patterns
----------------------------------

Summary
=======
Here's a short but complete summary of the recommendations.

For library authors
-------------------
A library is anything represented as a ``cc_library``, ``rust_library``, or
similar target that other code is expected to depend on.

#. Rely on :ref:`your dependencies' constraints
   <docs-bazel-compatibility-inherited>` (which you implicitly inherit)
   whenever possible.
#. Otherwise, use one of the :ref:`well-known constraints
   <docs-bazel-compatibility-well-known>`.
#. If no well-known constraint fits the bill, introduce a :ref:`module-specific
   constraint <docs-bazel-compatibility-module-specific>`.

For facade authors
------------------
:ref:`Facade <docs-facades>` authors are library authors, too. But there are
some special considerations that apply to facades.

#. :ref:`The facade's default backend should be unspecified
   <docs-bazel-compatibility-facade-default-backend>`.
#. If you want to make it easy for users to select a group of backends for
   related facades simultaneously, :ref:`provide dicts of such backends
   <docs-bazel-compatibility-facade-backend-dict>` for their use. :ref:`Don't
   provide multiplexer targets <docs-bazel-compatibility-multiplexer>`.
#. Whenever possible, :ref:`ensure backends fully implement a facade's
   interface <docs-bazel-compatibility-facade-backend-interface>`.
#. When implementing backend-specific tests, you may :ref:`introduce a config
   setting consuming a label flag <docs-bazel-compatibility-config-setting>`.

For SDK build authors
---------------------

#. :ref:`Provide config headers through a default-incompatible label flag
   <docs-bazel-compatibility-incompatible-label-flag>`.

Patterns for library authors
============================

.. _docs-bazel-compatibility-inherited:

Inherited incompatibility
-------------------------
Targets that transitively depend on incompatible targets are themselves
considered incompatible. This implies that many build targets do not need a
``target_compatible_with`` attribute.

Example: your target uses ``//pw_stream:socket_stream`` for RPC communication,
and ``socket_stream`` requires POSIX sockets. Your target *should not* try to
express this compatibility restriction through the ``target_compatible_with``
attribute. It will automatically inherit it from ``socket_stream``!

A particularly important special case are label flags which by default point to
an always-incompatible target, often :ref:`provided by SDKs
<docs-bazel-compatibility-incompatible-label-flag>`.

Asserting compatibility
^^^^^^^^^^^^^^^^^^^^^^^
Inherited incompatibility is very convenient, but can be dangerous. A change in
the transitive dependencies can unexpectedly make a top-level target
incompatible with a platform it should build for. How to minimize this risk?

For tests, the risk is relatively low because ``bazel test`` will print a list
of SKIPPED tests as part of its output.

.. note::

   TODO: https://pwbug.dev/347752345 - Come up with a way to mitigate the risk
   of accidentally skipping tests due to incompatibility.

For final firmware images, assert that the image is compatible with the
intended platform by explicitly listing it in CI invocations:

.. code-block:: sh

   # //pw_system:system_example is a binary that should build for this
   # platform.
   bazelisk build --config=rp2040 //... //pw_system:system_example

.. _docs-bazel-compatibility-well-known:

Well-known constraints
----------------------
If we introduced a separate constraint value for every module that's not purely
algorithmic, downstream users' platforms would become needlessly verbose. For
certain well-defined categories of dependency we will introduce "well-known"
constraint values that may be reused by multiple modules.

.. tip::

   When a module's assumptions about the underlying platform are fully captured
   by one of these well-known constraints, reuse them instead of creating a
   module-specific constraint.

.. _docs-bazel-compatibility-well-known-os:

OS-specific modules
^^^^^^^^^^^^^^^^^^^
Some build targets are compatible only with specific operating systems.  For
example, ``pw_digital_io_linux`` uses Linux syscalls. Such targets should be
annotated with the appropriate canonical OS ``constraint_value`` from the
`platforms repo <https://github.com/bazelbuild/platforms>`_:

.. code-block:: python

   cc_library(
     name = "pw_digital_io_linux",
     target_compatible_with = ["@platforms//os:linux"],
   )

Cross-platform modules requiring an OS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Some build targets are only intended for use on platforms with a fully-featured
OS (i.e., not on microcontrollers, but on the developer's laptop or
workstation, or on an embedded Linux system), *but* are cross-platform and not
restricted to one *particular* OS.  Example: an integration test written in Go
that starts subprocesses using the ``os/exec`` standard library package.

For these cross-platform targets, use the ``incompatible_with_mcu`` helper:

.. code-block:: python

   load("@pigweed//pw_build:compatibility.bzl", "incompatible_with_mcu")

   go_test(
       name = "integration_test",
       target_compatible_with = incompatible_with_mcu(),
   )

.. note::

   RTOSes are not OSes in the sense of this section. See
   :ref:`docs-bazel-compatibility-rtos`.

CPU-specific modules (rare)
^^^^^^^^^^^^^^^^^^^^^^^^^^^
Some build targets are only intended for particular CPU architectures. In this
case, use the canonical CPU ``constraint_value`` from the
`platforms repo <https://github.com/bazelbuild/platforms>`_:

.. code-block:: python

   # pw_cpu_exception_cortex_m/BUILD.bazel
   cc_library(
       name = "support",
       # Compatible only with Cortex-M processors.
       target_compatible_with = select({
           "@platforms//cpu:armv6-m": [],
           "@platforms//cpu:armv7-m": [],
           "@platforms//cpu:armv7e-m": [],
           "@platforms//cpu:armv7e-mf": [],
           "@platforms//cpu:armv8-m": [],
           "//conditions:default": ["@platforms//:incompatible"],
       }),
   )

SDK-provided constraints
^^^^^^^^^^^^^^^^^^^^^^^^
If a module depends on a third-party SDK, and that SDK has ``BUILD.bazel``
files that define ``constraint_values``, feel free to use those authoritative
values to indicate target compatibility.

.. code-block:: python

   cc_library(
       name = "pw_digital_io_rp2040",
       deps = [
           # Depends on the Pico SDK.
           "@pico-sdk//src/rp2_common/pico_stdlib",
           "@pico-sdk//src/rp2_common/hardware_gpio",
       ],
       # The Pico SDK provides authoritative constraint_values.
       target_compatible_with = ["@pico-sdk//bazel/constraint:rp2040"],
   )

.. note::

   This also applies to SDKs or libraries for which Pigweed provides
   ``BUILD.bazel`` files in our ``//third_party`` directory (e.g., FreeRTOS or
   stm32cube).



.. _docs-bazel-compatibility-module-specific:

Module-specific constraints
---------------------------
Many Pigweed modules are purely algorithmic: they make no assumptions about the
underlying platform. But many modules *do* make assumptions, sometimes quite
restrictive ones. For example, the ``pw_spi_mcuxpresso`` library includes
headers from the NXP SDK and will only work for certain NXP chips.

For any library that does make such assumptions, and these assumptions are not
captured by one of the :ref:`well-known constraints
<docs-bazel-compatibility-well-known>`, the recommended pattern is to define a
"boolean" ``constraint_setting`` to express compatibility. We introduce some
syntactic sugar (:ref:`module-pw_build-bazel-boolean_constraint_value`) for
making this concise.

Example:

.. code-block:: python

   # pw_spi_mcuxpresso/BUILD.bazel
   load("@pigweed//pw_build:compatibility.bzl", "boolean_constraint_value")

   boolean_constraint_value(
     name = "compatible",
   )

   cc_library(
     name = "pw_spi_mcuxpresso",
     # srcs, deps, etc omitted
     target_compatible_with = [":compatible"],
   )

Usage in platforms
^^^^^^^^^^^^^^^^^^
To use this module, a platform must include the constraint value:

.. code-block:: python

   platform(
     name = "downstream_platform",
     constraint_values = ["@pigweed//pw_spi_mcuxpresso:compatible"],
   )

If the library happens to be a facade backend, then the platform will have to
*both* point the label flag to the backend and list the ``constraint_value``.

.. code-block:: python

   platform(
     name = "downstream_platform",
     constraint_values = ["@pigweed//pw_sys_io_stm32cube:backend"],
     flags = [
       "--@pigweed//pw_sys_io:backend=@pigweed//pw_sys_io_stm32cube",
     ],
   )

.. tip::

   Just because a library is a facade backend doesn't mean it has any
   compatibility restrictions. Many backends (e.g., ``pw_assert_log``) have no
   such restrictions, and many others rely only on the well-known constraints.
   So, the number of ``constraint_values`` that need to be added to the typical
   downstream platform is substantially smaller than the number of configured
   backends.

Special case: host-compatible platform specific modules
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Some modules may require platforms to explicitly assert that they support them,
but also work on host platforms by default. An example of this is
``pw_stream:socket_stream``. Use the following pattern:

.. code-block:: python

   # pw_stream/BUILD.bazel
   load("@pigweed//pw_build:compatibility.bzl", "boolean_constraint_value", "incompatible_with_mcu")

   boolean_constraint_value(
     name = "socket_stream_compatible",
   )

   cc_library(
     name = "socket_stream",
     # Compatible with host platforms, and any platform that explicitly
     # lists `@pigweed//pw_stream:socket_stream_compatible` among its
     # constraint_values.
     target_compatible_with = incompatible_with_mcu(unless_platform_has=":socket_stream_compatible"),
   )

Patterns for facade authors
===========================

.. _docs-bazel-compatibility-facade-default-backend:

Don't provide default facade backends for device
------------------------------------------------
If the facade has no host-compatible backend, its default backend should be
``//pw_build:unspecified_backend``:

.. code-block:: python

   label_flag(
     name = "backend",
     build_setting_default = "//pw_build:unspecified_backend",
   )

Otherwise, use the following pattern:

.. code-block:: python

   load("@pigweed//pw_build:compatibility.bzl", "host_backend_alias")

   label_flag(
     name = "backend",
     build_setting_default = ":unspecified_backend",
   )

   host_backend_alias(
     name = "unspecified_backend",
     # "backend" points to the target implementing the host-compatible backend.
     backend = ":host_backend",
   )

This ensures that:

* If the target platform did not explicitly set a backend for a facade, that
  facade (and any target that transitively depends on it) is considered
  incompatible.
* *Except for the host platform*, which receives the host backend
  by default.

Following this pattern implies that we don't need a Bazel equivalent of GN's
``enable_if = pw_chrono_SYSTEM_CLOCK_BACKEND != ""`` (:cs:`example
<afef6c3c7de6f5a84465aad469a89556d0b34fbb:pw_i2c/BUILD.gn;l=136-145>`).
In Bazel, every build target is "enabled" if and only if all facades it
transitively depends on have a backend set.

Providing multiplexer targets is an alternative way to set default facade
backends, but :ref:`is not recommended in upstream Pigweed
<docs-bazel-compatibility-multiplexer>`. One exception is if your facade needs
a different default host backend depending on the OS. So, the following
is OK:

.. code-block:: python

   # Host backend that's OS-specific.
   alias(
     name = "host_backend",
     actual = select({
       "@platforms//os:macos": ":macos_backend",
       "@platforms//os:linux": ":linux_backend",
       "@platforms//os:windows": ":windows_backend",
       "//conditions:default": "//pw_build:unspecified_backend",
     }),
   )

.. _docs-bazel-compatibility-facade-backend-dict:

Provide default backend collections as dicts
--------------------------------------------
In cases like RTOS-specific backends, where the user is expected to want to set
all of them at once, provide a dict of default backends for them to include in
their platform definition:

.. code-block:: python

   #//pw_build/backends.bzl (in upstream Pigweed)

   # Dict of typical backends for FreeRTOS.
   FREERTOS_BACKENDS = {
     "@pigweed//pw_chrono:system_clock_backend": "@pigweed//pw_chrono_freertos:system_clock",
     "@pigweed//pw_chrono:system_timer_backend": "@pigweed//pw_chrono_freertos:system_timer",
     # etc.
   }

   # User's platform definition (in downstream repo)
   load("@pigweed//pw_build/backends.bzl", "FREERTOS_BACKENDS", "merge_flags")

   platform(
     name = "my_freertos_device",
     flags = merge_flags(
       base = FREERTOS_BACKENDS,
       overrides = {
         # Override one of the default backends.
         "@pigweed//pw_chrono:system_clock_backend": "//src:my_device:pw_system_clock_backend",
         # Provide additional backends.
         "@pigweed//pw_sys_io:backend": "//src:my_device:pw_sys_io_backend",
       },
     ),
   )

.. _docs-bazel-compatibility-facade-backend-interface:

Guard backend-dependent interfaces with constraints
---------------------------------------------------

What's a backend-dependent interface?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
We :ref:`officially define a facade <docs-facades>` as "an API contract of a
module that must be satisfied at compile-time", and a backend as merely "an
implementation of a facade’s contract." However, a small number of facades do
not fit this definition, and expose APIs that vary based on the backend
selected (!!!).  Examples:

* ``pw_thread: Thread::join()`` :ref:`may or may not be available
  <module-pw_thread-detaching-joining>` depending on the selected backend.
* ``pw_async2``: The ``EPollDispatcher`` offers different APIs from other
  ``pw_async2::Dispatcher`` backends, and parts of :ref:`module-pw_channel`
  (:cpp:class:`pw::EpollChannel`) rely on those APIs.

This breaks the invariant that a facade's APIs either are available (if it has
a backend) or are not available (if it has no backend, in which case targets
that depend on the facade are incompatible). These facades might have a backend
and yet (parts of) their APIs are unavailable!

What to do instead?
^^^^^^^^^^^^^^^^^^^

Fix the class structrure
........................
If possible, reorganize the class structure so that the facade's API is
backend-independent, and users who need the additional functionality must
depend directly on the specific backend that provides this.

This is the correct fix for the ``EPollDispatcher`` case; see `b/342000726
<https://pwbug.dev/342000726>`__ for more details.

Express the backend-dependent capability through a constraint
.............................................................
If the backend-dependent interface cannot be refactored away, guard it using a
custom constraint.

Let's discuss :ref:`module-pw_thread` as a specific example. The
backend-dependence of the interface is that ``Thread::join()`` may or may not
be provided by the backend.

To expose this to the build system, introduce a corresponding constraint:

.. code-block:: python

   # //pw_thread/BUILD.bazel
   constraint_setting(
     name = "joinable",
     # Default appropriate for the autodetected host platform.
     default_constraint_value = ":threads_are_joinable",
   )

   constraint_value(
     name = "threads_are_joinable",
     constraint_setting = ":joinable",
   )

   constraint_value(
     name = "threads_are_not_joinable",
     constraint_setting = ":joinable",
   )

Platforms can declare whether threads are joinable or not by including the
appropriate constraint value in their definitions:

.. code-block:: python

   # //platforms/BUILD.bazel
   platform(
     name = "my_device",
     constraint_values = [
       "@pigweed//pw_thread:threads_are_not_joinable",
     ],
   )

Build targets that unconditionally call ``Thread::join()`` (not within a ``#if
PW_THREAD_JOINING_ENABLED=1``) should be marked compatible with the
``"@pigweed//pw_thread:threads_are_joinable"`` constraint value:

.. code-block:: python

   cc_library(
     name = "my_library_requiring_thread_joining",
     # This library will be incompatible with "//platforms:my_device", on
     # which threads are not joinable.
     target_compatible_with = ["@pigweed//pw_thread:threads_are_joinable"],
   )

If your library will compile both with and without thread joining (either
because it doesn't call ``Thread::join()``, or because all such calls are
guarded by ``#if PW_THREAD_JOINING_ENABLED=1``), you don't need any
``target_compatible_with`` attribute.

Configuration-dependent interfaces
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Some facades have interfaces that depend not just on the choice of backend, but
on their :ref:`module-structure-compile-time-configuration`. We don't have a
good pattern for these libraries yet.

.. note::

   TODO: https://pwbug.dev/234872811 - Establish such a pattern.

Patterns for SDK build authors
==============================
This section discusses patterns useful when providing a Bazel build for a
pre-existing library or SDK.

.. _docs-bazel-compatibility-incompatible-label-flag:

Provide config headers through label flags
------------------------------------------
Many libraries used in embedded projects expect configuration to be provided
through a header file at a predefined include path. For example, FreeRTOS
expects the user to provide a configuration header that will be included via
``#include "FreeRTOSConfig.h``. How to handle this when writing a *generic*
``BUILD.bazel`` file for such a library?

Use the following pattern:

.. code-block:: python

   # @freertos//BUILD.bazel

   cc_library(
     name = "freertos",
     # srcs, hdrs omitted.
     deps = [
       # freertos has a dependency on :freertos_config.
       ":freertos_config",
     ],
   )

   # Label flag that points to the cc_library target providing FreeRTOSConfig.h.
   label_flag(
       name = "freertos_config",
       build_setting_default = ":unspecified",
   )

   cc_library(
       name = "unspecified",
       # The default config is not compatible with any configuration: you can't
       # build FreeRTOS without choosing a config.
       target_compatible_with = ["@platforms//:incompatible"],
   )

Why is this recommended?

#. The configuration header to use can be selected as part of platform
   definition, by setting the label flag. This gives the user a lot of
   flexibility: they can use different headers when building different targets
   within the same repo.
#. Any target (test, library, or binary) that depends on the ``freertos``
   ``cc_library`` will be considered incompatible with the target platform
   unless that platform explicitly configured FreeRTOS by setting the
   ``freertos_config`` label flag. So, if your target's only assumption about
   the platform is that it supports FreeRTOS, including ``freertos`` in your
   ``deps`` is all you need to do to express this.

This pattern is not useful in upstrem Pigweed itself, because Pigweed uses a
more elaborate  configuration pattern at the C++ source level. See
:ref:`module-structure-compile-time-configuration`.

.. _docs-bazel-compatibility-not-recommended:

--------------------------------------
Alternative patterns (not recommended)
--------------------------------------

This section describes alternative build compatibility patterns that we've used
or considered in the past. They are **not recommended**. We'll work to remove
their instances from Pigweed, replacing them with the recommended patterns.

.. _docs-bazel-compatibility-per-facade-constraint-settings:

Per-facade constraint settings (not recommended)
================================================
This approach was once recommended, although it was `never fully rolled out
<https://pwbug.dev/272090220>`_:

#. For **every facade**, introduce a ``constraint_setting`` (e.g.,
   ``@pigweed//pw_foo:backend_constraint_setting``). This would be done by
   whoever defines the facade; if it's an upstream facade, upstream Pigweed
   should define this setting.
#. For every backend, introduce a corresponding constraint_value (e.g.,
   ``//backends/pw_foo:board1_backend_constraint_value``). This should be done
   by whoever defines the backend; for backends defined in downstream projects,
   it's done in that project.
#. Mark the backend ``target_compatible_with`` its associated ``constraint_value``.

Why is this not recommended
---------------------------
The major difference between this and :ref:`what we're recommending
<docs-bazel-compatibility-recommended>` is that *every* backend was associated
with a *unique* ``constraint_value``, regardless of whether the backend imposed
any constraints on its platform or not. This implied downstream platforms that
set N backends would also have to list the corresponding N
``constraint_values``.

The original motivation for per-facade constraint settings is now obsolete.
They were intended to allow backend selection via multiplexers before
platform-based flags became available. `More details for the curious
<https://docs.google.com/document/d/1O4xjnQBDpOxCMhlyzsowfYF3Cjq0fOfWB6hHsmsh-qI/edit?resourcekey=0-0B-fT2s05UYoC4TQIGDyvw&tab=t.0#heading=h.u62b26x3p898>`_.

Where they still exist in upstream Pigweed, these constraint settings will be
removed (see :ref:`docs-bazel-compatibility-implementation-plan`).

.. _docs-bazel-compatibility-config-setting:

Config setting from label flag (not recommended except for tests)
=================================================================
`This pattern <https://pwbug.dev/342691352#comment3>`_ was an attempt to keep
the central feature of per-facade constraint settings (the selection of a
particular backend can be detected) without forcing downstream users to list
``constraint_values`` explicitly in their platforms. A ``config_setting`` is
defined that detects if a backend was selected through the label flag:

.. code-block:: python

   # pw_sys_io_stm32cube/BUILD.bazel

   config_setting(
       name = "backend_setting",
       flag_values = {
           "@pigweed//pw_sys_io:backend": "@pigweed//pw_sys_io_stm32cube",
       },
   )

   cc_library(
     name = "pw_sys_io_stm32cube",
     target_compatible_with = select({
       ":backend_setting": [],
       "//conditions:default": ["@platforms//:incompatible"],
     }),
   )

Why is this not recommended
---------------------------
#. We're really insisting on setting the label flag directly to the backend. In
   particular, we disallow patterns like "point the ``label_flag`` to an
   ``alias`` that may resolve to different backends based on a ``select``"
   (because `the config_setting in the above example will be false in that case
   <https://github.com/bazelbuild/bazel/issues/21189>`_).
#. It's a special pattern just for facade backends. Libraries which need to
   restrict compatibility but are not facade backends cannot use it.
#. Using the ``config_setting`` in ``target_compatible_with`` requires the
   weird ``select`` trick shown above. It's not very ergonomic, and definitely
   surprising.

When to use it anyway
---------------------
We may resort to defining private ``config_settings`` following this pattern to
solve special problems like `b/336843458 <https://pwbug.dev/336843458>`_ |
"Bazel tests using pw_unit_test_light can still rely on GoogleTest" or
:cs:`pw_malloc tests
<96313b7cc138b0c49742e151927e0d3a013f8b47:pw_malloc/BUILD.gn;l=190-191>`.

In addition, some tests are backend-specific (directly include backend
headers). The most common example are tests that depend on
:ref:`module-pw_thread` but directly ``#include "pw_thread_stl/options.h"``.
For such tests, we will define *private* ``config_settings`` following this
pattern.

.. _docs-bazel-compatibility-board-chipset:

Board and chipset constraint settings (not recommended)
=======================================================
Pigweed has historically defined a :cs:`board
<main:pw_build/constraints/board/BUILD.bazel>` ``constraint_setting``, and this
setting was used to indicate that some modules are compatible with particular
boards.

Why is this not recommended
---------------------------
This is a particularly bad pattern: hardly any Pigweed build targets are only
compatible with a single board. Modules which have been marked as
``target_compatible_with = ["//pw_build/constraints/board:mimxrt595_evk"]`` are
generally compatible with many other RT595 boards, and even with other NXP
chips. We've already run into cases in practice where users want to use a
particular backend for a different board.

The :cs:`chipset <main:pw_build/constraints/chipset/BUILD.bazel>`
``constraint_setting`` has the same problem: the build targets it was applied to
don't contain assembly code, and so are not generally compatible with only a
particular chipset. It's also unclear how to define chipset values in a
vendor-agnostic manner.

These constraints will be removed (see
:ref:`docs-bazel-compatibility-implementation-plan`).

.. _docs-bazel-compatibility-rtos:

RTOS constraint setting (not recommended)
=========================================
Some modules include headers provided by an RTOS such as embOS, FreeRTOS or
Zephyr. If they do not make additional assumptions about the platform beyond
the availability of those headers, they could just declare themselves
compatible with the appropriate value of the ``//pw_build/constraints/rtos:rtos``
``constraint_setting``. Example:

.. code-block:: python

   # pw_chrono_embos/BUILD.bazel

   cc_library(
     name = "system_clock",
     target_compatible_with = ["//pw_build/constraints/rtos:embos"],
   )

Why is this not recommended
---------------------------
At first glance, this seems like a pretty good pattern: RTOSes kind of like
OSes, and OSes :ref:`have their "well-known" constraint
<docs-bazel-compatibility-well-known-os>`. So why not RTOSes?

RTOSes are *not* like OSes in an important respect: the dependency on them is
already expressed in the build system! A library that uses FreeRTOS headers
will have an explicit dependency on the ``@freertos`` target. (This is in
contrast to OSes: a library that includes Linux system headers will not get
them from an explicit dependency.)

So, we can push the question of compatibility down to that target: if FreeRTOS
is compatible with your platform, then a library that depends on it is (in
general) compatible, too. Most (all?) RTOSes require configuration through
``label_flags`` (in particular, to specify the port), so platform compatibility
can be elegantly handled by setting the default value of that flag to a target
that's ``@platforms//:incompatible``.

.. _docs-bazel-compatibility-multiplexer:

Multiplexer targets (not recommended)
=====================================
Historically, Pigweed selected default backends for certain facades based on
platform constraint values. For example, this was done by
``//pw_chrono:system_clock``:

.. code-block:: python

   label_flag(
       name = "system_clock_backend",
       build_setting_default = ":system_clock_backend_multiplexer",
   )

   cc_library(
       name = "system_clock_backend_multiplexer",
       visibility = ["@pigweed//targets:__pkg__"],
       deps = select({
           "//pw_build/constraints/rtos:embos": ["//pw_chrono_embos:system_clock"],
           "//pw_build/constraints/rtos:freertos": ["//pw_chrono_freertos:system_clock"],
           "//pw_build/constraints/rtos:threadx": ["//pw_chrono_threadx:system_clock"],
           "//conditions:default": ["//pw_chrono_stl:system_clock"],
       }),
   )

Why is this not recommended
---------------------------
This pattern made it difficult for the user defining a platform to understand
which backends were being automatically set for them (because this information
was hidden in the ``BUILD.bazel`` files for individual modules).

What to do instead
------------------
Platforms should explicitly set the backends of all facades they use via
platform-based flags. For users' convenience, backend authors may :ref:`provide
default backend collections as dicts
<docs-bazel-compatibility-facade-backend-dict>` for explicit inclusion in the
platform definition.

.. _docs-bazel-compatibility-implementation-plan:

-----------------
Are we there yet?
-----------------
As of this writing, upstream Pigweed does not yet follow the best practices
recommended below.  `b/344654805 <https://pwbug.dev/344654805>`__ tracks fixing
this.

Here's a high-level roadmap for the recommendations' implementation:

#. Implement the "syntactic sugar" referenced in the rest of this doc:
   ``boolean_constraint_value``, ``incompatible_with_mcu``, etc.

#. `b/342691352  <https://pwbug.dev/342691352>`_ | "Platforms should set
   backends for Pigweed facades through label flags". For each facade,

   * Remove the :ref:`multiplexer targets
     <docs-bazel-compatibility-multiplexer>`.
   * Remove the :ref:`per-facade constraint settings
     <docs-bazel-compatibility-per-facade-constraint-settings>`.
   * Remove any :ref:`default backends
     <docs-bazel-compatibility-facade-default-backend>`.

#. `b/343487589 <https://pwbug.dev/343487589>`_ | Retire the :ref:`Board and
   chipset constraint settings <docs-bazel-compatibility-board-chipset>`.


.. _docs-bazel-compatibility-background:

--------------------
Appendix: Background
--------------------

.. _docs-bazel-compatibility-why-wildcard:

Why wildcard builds?
====================
Pigweed is generic microcontroller middleware: you can use Pigweed to
accelerate development on any microcontroller platform. In addition, Pigweed
provides explicit support for a number of specific hardware platforms, such as
the :ref:`target-rp2040` or :ref:`STM32f429i Discovery Board
<target-stm32f429i-disc1-stm32cube>`. For these specific platforms, every
Pigweed module falls into one of three buckets:

*  **works** with the platform, or,
*  **is not intended to work** with the platform, because the platform lacks the
   relevant capabilities (e.g., the :ref:`module-pw_spi_mcuxpresso` module
   specifically supports NXP chips, and is not intended to work with the
   Raspberry Pi Pico).
*  **should work but doesn't yet**; that's a bug or missing feature in Pigweed.

Bazel's wildcard builds provide a nice way to ensure each Pigweed build target
is known to fall into one of those three buckets. If you run:

.. code-block:: sh

   bazelisk build --config=rp2040 //...

Bazel will attempt to build all Pigweed build targets for the specified
platform, with the exception of targets that are explicitly annotated as not
compatible with it. Such `incompatible targets will be automatically skipped
<https://bazel.build/extending/platforms#skipping-incompatible-targets>`_.

Challenge: designing ``constraint_values``
==========================================
As noted above, for wildcard builds to work we need to annotate some targets as
not compatible with certain platforms. This is done through the
`target_compatible_with attribute
<https://bazel.build/reference/be/common-definitions#common.target_compatible_with>`_,
which is set to a list of `constraint_values
<https://bazel.build/reference/be/platforms-and-toolchains#constraint_value>`_
(essentially, enum values). For example, here's a target only compatible with
Linux:

.. code-block:: python

   cc_library(
     name = "pw_digital_io_linux",
     target_compatible_with = ["@platforms//os:linux"],
   )

If the platform lists all the ``constraint_values`` that appear in the target's
``target_compatible_with`` attribute, then the target is compatible; otherwise,
it's incompatible, and will be skipped.

If this sounds a little abstract, that's because it is! Bazel is not very
opinionated about what the constraint_values actually represent. There are only
two sets of canonical ``constraint_values``, ``@platforms//os`` and
``@platforms//cpu``.  Here are some possible choices---not necessarily good
ones, but all seen in the wild:

*  A set of constraint_values representing RTOSes:

   * ``@pigweed//pw_build/constraints/rtos:embos``
   * ``@pigweed//pw_build/constraints/rtos:freertos``

*  A set of representing individual boards:

   * ``@pigweed//pw_build/constraints/board:mimxrt595_evk``

*  A pair of constraint values associated with a single module:

   * ``@pigweed//pw_spi_mcuxpresso:compatible`` (the module is by definition compatible with any platform containing this constraint value)
   * ``@pigweed//pw_spi_mcuxpresso:incompatible``

There are many more possible structures.

What about ``constraint_settings``?
===================================
Final piece of background: we mentioned above that ``constraint_values`` are a bit
like enum values. The enums themselves (groups of ``constraint_values``) are called
``constraint_settings``.

Each ``constraint_value`` belongs to a ``constraint_setting``, and a platform
may specify at most one value from each setting.

Guiding principles
==================
These are the principles that guided the selection of the :ref:`recommended
patterns <docs-bazel-compatibility-recommended>`:

* **Be consistent.** Make the patterns for different use cases as similar to
  each other as possible.
* **Make compatibility granular.** Avoid making assumptions about what sets of
  backends or HAL modules will be simultaneously compatible with the same
  platforms.
* **Minimize the amount of boilerplate** that downstream users need to put up
  with.
* **Support the autodetected host platform.** That is, ensure ``bazel build
  --platforms=@bazel_tools//tools:host_platform //...`` works. This is necessary
  internally (for google3) and arguably more convenient for downstream users
  generally.
