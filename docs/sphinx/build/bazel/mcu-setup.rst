.. _docs-bazel-mcu-setup:

=====================
Set up a MCU platform
=====================
This tutorial introduces the overarching steps required to set up a working
Bazel build that targets an MCU.

------------------
Declare a platform
------------------
Declaring a `platform <https://bazel.build/extending/platforms>`__ gives Bazel
the information required to build for a particular device. Declare a platform in
any ``BUILD.bazel`` file with the `platform
<https://bazel.build/reference/be/platforms-and-toolchains#platform>`__ builtin:

Example
=======
**BUILD.bazel**

.. code-block:: py

   platform(
       name = "my_mcu",
   )

This platform doesn't do much right now. To make it useful,
:ref:`docs-bazel-mcu-setup-platform-options`.

Build with the platform
=======================
There are two ways to build something using the newly created platform.

Option 1: Explicitly request the platform with a Bazel flag
-----------------------------------------------------------
This method is useful when building an arbitrary target using a
specific platform configuration. Use the ``--platforms`` flag to specify
which platform to target:

.. code-block:: console

   $ bazelisk build --platforms=//:my_mcu //path/to:thing_to_build


Option 2: Encode the platform in the build graph
------------------------------------------------
For things that developers frequently build, embed a request to build one or
more targets in the build graph. To gain access to this feature, first add
``rules_platforms`` as a ``bazel_dep`` in the project's ``MODULE.bazel`` file,
then add a ``platform_data`` shim in any ``BUILD.bazel`` file:

**BUILD.bazel**

.. code-block:: py

   load("@rules_platform//platform_data:defs.bzl", "platform_data")

   platform_data(
       name = "foo_for_my_mcu",
       platform = "//:my_mcu",
       target = "//path/to:foo",
   )

When building this target, there's no need to specify ``--platforms`` for it
to build using the custom MCU configuration. Anyone can just normally build
the target like this:

.. code-block:: console

   $ bazelisk build //:foo_for_my_mcu

While ``target`` may only point to a single thing in the build graph, multiple
targets can be collected together into a single target with ``filegroup`` and
``test_suite`` rules. Unfortunately, there's no way to encode `wildcard build
patterns <https://bazel.build/run/build#specifying-build-targets>`__ in the
build graph.

.. tip::

   Enable ``--experimental_platform_in_output_dir`` to use the platform's name
   in the ``bazel-out/`` directory. This makes it much easier to find build
   artifacts.

.. _docs-bazel-mcu-setup-platform-options:

-----------------------------------
Configure platform-specific options
-----------------------------------
Platforms are not useful until they specify constraint values and flags that
properly define the configuration space Bazel operates in.

.. _docs-bazel-mcu-setup-constraints:

Constraint settings and values
==============================
`Constraint settings <https://bazel.build/extending/platforms#constraints>`__
are multiple-choice options that drive conditional build logic and build target
compatibility. Build target compatibility is largely used to create guardrails
that prevent inherently incompatible contexts from being evaluated. Later you'll
learn how to :ref:`docs-bazel-mcu-setup-conditional-build-logic`.

Constraints have the following special properties:

* Constraints may only be set in a ``platform``.
* When the ``platform`` changes, all constraints are reset to their default
  value (which may be ``None``) if they are not explicitly set by the new
  platform.


Example
-------
This example configures a MCU with an ARMv7-M CPU and no OS:

**BUILD.bazel**

.. code-block:: py

   platform(
       name = "my_mcu",
       constraint_values = [
           # Value for the multiple choice option declared at @platforms//cpu:cpu.
           "@platforms//cpu:armv7-m",
           # Value for the multiple choice option declared at @platforms//os:os.
           "@platforms//os:none",
       ],
   )

These don't trigger magical internal behaviors in Bazel, they just influence any
parts of build files that explicitly express conditional logic guided by
``@platforms//cpu:cpu`` and ``@platforms//os:os``.

See Pigweed's :ref:`module-pw_build-bazel_constraints` for the most critical
constraints offered by Pigweed.

Flags
=====
Flags differ significantly from :ref:`docs-bazel-mcu-setup-constraints`. Flags
offer more free-form values, and Bazel `rule implementations
<https://bazel.build/extending/rules>`__ can directly consume the value of
flags. While flags cannot directly influence conditional build logic,
`config_setting
<https://bazel.build/docs/configurable-attributes#custom-flags>`__ expressions
declare conditions that can guide conditional build logic in the same
way as a constraint.

Flags have the following special properties:

* Flags may be set via the command line, ``.bazelrc`` file, or a ``platform``.
* When the ``platform`` changes, the incoming platform inherits the prior state
  of all flags that it does not explicitly override. This is true even when
  multiple ``platform`` changes are chained together.

Example
-------
This example configures link-time dependencies required for every ``cc_binary``
and the unit testing implementation to use for ``pw_unit_test``:

**BUILD.bazel**

.. code-block:: py

   platform(
       name = "my_mcu",
       flags = [
           # When linking cc_binary targets, always link against this additional
           # library.
           "--@bazel_tools//tools/cpp:link_extra_libs=//:my_link_dependencies",

           # Use Pigweed's embedded-friendly unit test implementation for
           # pw_cc_test targets.
           "--@pigweed//pw_unit_test:backend=@pigweed//pw_unit_test:light",
       ],
   )

Pigweed has many flags. Look for module-specific flags to control the behavior
of various Pigweed modules.

.. _docs-bazel-mcu-setup-conditional-build-logic:

----------------------------------------
Add conditional build logic where needed
----------------------------------------
Conditional build logic in Bazel build files are not guided by ``if``
statements. Instead, all possible branches of a condition are packed into a
single `select
<https://bazel.build/docs/configurable-attributes#select-and-dependencies>`__
object. This allows Bazel to defer the conditional evaluation until later stages
in the build process. It may be helpful to think of ``select`` objects like
lambdas that are passed to later stages of the build.

Create a ``select`` using a dictionary that maps conditions to the resulting
value:

.. code-block:: py

   is_linux = select({
       # When @platforms//os:os is @platforms//os:linux, this branch is selected
       # and produces the value True.
       "@platforms//os:linux": True,
       # A special "else" fallthrough condition that produces the value False.
       "//conditions:default": False,
   })

Accepted conditions (keys of the dictionary) for a ``select`` statement are
labels pointing to ``constraint_setting`` or ``config_setting`` targets.

Selects values may be any standard type: strings, labels, booleans, lists, etc.
However, because their values are opaque until later stages of the build,
``select`` objects may only be passed to an attribute of a rule.

When the select is evaluated, the dictionary keys are evaluated **in order** as
a series of ``if`` and ``elif`` expressions, with ``//conditions:default``
representing ``else``.

For attributes that accept a ``list`` of values, it's possible to chain
``select`` statements.

See `Configurable Build Attributes
<https://bazel.build/docs/configurable-attributes>`__ for a more comprehensive
explanation of how to use these constructs.

Example
=======
This example illustrates a classic "Hello, world!" binary that has two pieces of
conditional logic: a different ``.cc`` file if a boolean flag is set a certain
way, and an extra dependency for Windows targets.

.. code-block:: py

   load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
   load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

   # A flag that can be set in a platform, or via the command-line when building.
   bool_flag(
       name = "greet_the_whole_universe",
       build_setting_default = False,
   )

   # A constraint that is true when --//:greet_the_whole_universe=True.
   config_setting(
       name = "greet_the_whole_universe_is_true",
       # Only evaluates to True if ALL flags match the expected values (logical
       # AND).
       flag_values = {
           ":greet_the_whole_universe": "True",
       }
   )

   cc_binary(
       name = "hello_everyone",
       srcs = select({
           # If the greet_the_whole_universe flag is true, as dictated by the
           # greet_the_whole_universe_is_true setting, use a special
           # hello_universe.cc file.
           ":greet_the_whole_universe_is_true": ["hello_universe.cc"],

           # Else, just use the default hello_world.cc file.
           "//conditions:default": ["hello_world.cc"],
       }),
       deps = [
           # This dependency is always added.
           "//common:helpers",
       ] + select({
           # If this is built for windows, add an extra dependency.
           "@platforms//os:windows": ["//:special_windows_sauce"],

           # For all other configurations, don't add anything extra.
           "//conditions:default": [],
       }),
   )

.. _docs-bazel-mcu-setup-target-compatibility:

------------------------------
Constrain target compatibility
------------------------------
To prevent certain targets from building under certain configurations, populate
``target_compatible_with`` on various build targets. This will do two things:

#. When wildcard (``...``) build/test patterns are expanded, targets that
   are incompatible **or targets that depend on something that is incompatible**
   are skipped.
#. If an incompatible target is explicitly requested (either as a dependency, or
   directly), an error is raised.

Example: Using a constraint setting
===================================
Each ``constraint_value`` in ``target_compatible_with`` adds an additional
requirement that must be satisfied for the target to be deemed compatible. The
example below requires that the CPU is an ARMv7-M CPU:

.. code-block:: py

   cc_library(
       name = "armv7m_platform_support",
       srcs = ["armv7m_stubs.cc"],
       target_compatible_with = [
           "@platforms//cpu:armv7-m",
       ],
   )

Example: Using a config setting
===============================
Unlike a ``constraint_setting``, a ``config_setting`` can't be used directly
in ``target_compatible_with``. The workaround is to use a ``select`` to achieve
the same result. The example below marks ``fpu_math`` as incompatible unless
``--//:my_mcu_has_a_fpu=True``:

.. code-block:: py

   load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
   load("@rules_cc//cc:cc_library.bzl", "cc_library")

   bool_flag(
       name = "my_mcu_has_a_fpu",
       build_setting_default = False,
   )

   config_setting(
       name = "my_mcu_has_a_fpu_is_true",
       flag_values = {
           ":my_mcu_has_a_fpu": "True",
       },
   )

   cc_library(
       name = "fpu_math",
       srcs = ["fpu_math.cc"],
       target_compatible_with = select({
           ":my_mcu_has_a_fpu_is_true": [],
           "//conditions:default": ["@platforms//:incompatible"],
       }),
   )

Example: Combining constraint settings and config settings
==========================================================
When multiple targets are provided to ``target_compatible_with``, they are
treated as a logical AND. The target is only considered compatible if the
current configuration matches ALL of the provided constraints. Since
``config_setting`` evaluations have additional restrictions,

.. code-block:: py

   load("@rules_cc//cc:cc_library.bzl", "cc_library")

   load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
   load("@rules_cc//cc:cc_library.bzl", "cc_library")

   bool_flag(
       name = "my_mcu_has_a_fpu",
       build_setting_default = False,
   )

   config_setting(
       name = "my_mcu_has_a_fpu_is_true",
       flag_values = {
           ":my_mcu_has_a_fpu": "True",
       },
   )

   cc_library(
       name = "armv7m_with_fpu_platform_support",
       srcs = ["armv7m_with_fpu_helpers.cc"],

       # Logical AND that mixes a config_setting and constraint_value.
       target_compatible_with = [
           "@platforms//cpu:armv7-m",
       ] + select({
           ":my_mcu_has_a_fpu_is_true": [],
           "//conditions:default": ["@platforms//:incompatible"],
       }),
   )

   cc_library(
       name = "armv7m_or_fpu_platform_support",
       srcs = ["armv7m_or_fpu_helpers.cc"],

       # Logical OR that mixes a config_setting and constraint_value.
       target_compatible_with = select({
           "@platforms//cpu:armv7-m": [],
           ":my_mcu_has_a_fpu_is_true": [],
           "//conditions:default": ["@platforms//:incompatible"],
       }),
   )

--------------------
Configure toolchains
--------------------
In Bazel, toolchain configuration is much more complex than setting a series
something like ``CC`` to point to a binary and ``CC_FLAGS`` to build up a series
of arguments.

Bazel uses `toolchains
<https://bazel.build/extending/toolchains#registering-building-toolchains>`__ as
a boundary mechanism between generalized rule behaviors and rather complex
implementation details that are specific to the platform being targeted, and
where the toolchain tools are running. This is necessary to support many
languages, enable remote execution, and improve build efficiency. Since this is
a very deep topic, only the user-facing side of toolchain configuration will be
covered here.

Rather than a platform dictating which toolchains to use, Bazel relies on
a somewhat magical toolchain selection process. This works roughly as follows:

* Using the `toolchain
  <https://bazel.build/reference/be/platforms-and-toolchains#toolchain>`__ rule,
  a toolchain is declared with:

  - ``toolchain_type`` to declare what category of toolchain is represented
    (e.g. Rust, Go, C/C++).
  - ``target_compatible_with`` to
    :ref:`docs-bazel-mcu-setup-target-compatibility`.
  - ``exec_compatible_with`` to constrain compatibility against the host
    machine that will be executing toolchain binaries.
  - ``toolchain``, pointing to the actual definition of the toolchain.

* Projects register toolchains in their ``MODULE.bazel`` file using
  ``register_toolchains()``. Any ``register_toolchains()`` call that **does
  not** have ``dev_dependency = True`` is inherited by downstream Bazel
  modules.
* When Bazel is building for a given platform, it looks up a toolchain
  with the required ``toolchain_type``, and picks the first one that satisfies
  both the target and exec compatibility expressions. This is an order-sensitive
  selection process that is documented in Bazel's `toolchain resolution
  <https://bazel.build/extending/toolchains#toolchain-resolution>`__ process.

This is vastly different from how most other build systems configure toolchains.

.. tip::

    Build with ``--toolchain_resolution_debug=.*`` to diagnose why Bazel isn't
    selecting the toolchain you're expecting it to select.

One way to short-circuit this process is to create a universal toolchain for
each language, and register it first in your ``MODULE.bazel`` file:

**BUILD.bazel**

.. code-block:: py

   # Platforms must explicitly set this flag to point to a cc_toolchain
   # implementation to have a working C/C++ toolchain.
   label_flag(
       name = "cc_toolchain_implementation",
   )

   toolchain(
       name = "universal_cc_toolchain",
       # No compatibility constraints, so it's always selected.
       target_compatible_with = [],
       exec_compatible_with = [],
       toolchain = ":cc_toolchain_implementation",
       toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
   )

You may use :ref:`module-pw_toolchain-bazel-upstream-pigweed-toolchains` by
registering them and adding the architecture-specific
:ref:`module-pw_build-bazel_constraints` to your ``platform``.

`Pigweed's clang toolchain for Cortex-M <https://cs.opensource.google/pigweed/pigweed/+/main:pw_toolchain/arm_clang/BUILD.bazel?q=%22arm_clang_toolchain_cortex-m%22>`__
is a helpful resource for declaring a C/C++ toolchain for ARM Cortex-M MCUs.

-----------------------------------------------
Derive additional configurations from platforms
-----------------------------------------------
Sometimes, bootloaders or adjacent cores for a given MCU may require derived
variants of the same platform that only have one or two flags that are
different. Use a `transition
<https://bazel.build/rules/lib/builtins/transition>`__ to create a parallel
build graph with a slightly different set of flags.

Remember, a transition cannot directly change ``constraint_setting`` values.
Those only change when the ``platform`` changes.

.. caution::

   This is quite expensive, and can be very error prone. Only use this when
   there is no other alternative option.

---------
Resources
---------

* `Platforms <https://bazel.build/extending/platforms>`__
* `Configurability <https://bazel.build/docs/configurable-attributes>`__
* `Toolchains (generic) <https://bazel.build/extending/toolchains>`__
* `C/++ toolchain declaration API <https://github.com/bazelbuild/rules_cc/blob/main/docs/toolchain_api.md>`__
* `Transitions <https://bazel.build/rules/lib/builtins/transition>`__
