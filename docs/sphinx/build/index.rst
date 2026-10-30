.. _docs-build:

=============
Build systems
=============
Pigweed has full support for Bazel and partial support for GN and CMake.
See :ref:`docs-concepts-build-system` for more information.

-------
General
-------
.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` Overview
      :link: docs-build-systems-overview
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      A high-level explanation of Pigweed's approach to build systems.

-----
Bazel
-----
.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Install Bazel
      :link: docs-install-bazel
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Recommendations on how to install Bazel.

   .. grid-item-card:: :octicon:`code-square` Bazel quickstart
      :link: https://cs.opensource.google/pigweed/quickstart/bazel
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Fork our minimal, Bazel-based starter project to create a new
      Pigweed project from scratch. The project includes a basic
      blinky LED program that runs on Raspberry Pi Picos and can
      be simulated on your development host.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Bazel integration
      :link: docs-bazel-integration
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to use Pigweed in an existing Bazel project: add Pigweed
      as a dependency, start using Pigweed modules, and set up static and
      runtime analysis.

   .. grid-item-card:: :octicon:`rocket` Bazel build compatibility patterns
      :link: docs-bazel-compatibility
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      A deep-dive into the Bazel patterns Pigweed uses to express that a build
      target is compatible with a platform

----------
GN / Ninja
----------
.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` Pigweed's GN Python build
      :link: docs-python-build
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Details on the custom GN-based build system that Pigweed uses to
      manage its Python code.

-----
CMake
-----
.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` CMake integration
      :link: cmake-integration
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn how to use Pigweed in an existing CMake project: add Pigweed
      as a dependency, configure backends, and use modules.

.. toctree::
   :maxdepth: 2
   :hidden:

   self
   Overview <overview>
   bazel/index
   gn/index
   cmake/index

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------
Modules
-------
The following table lists all modules related to Build systems.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_build`", ":bdg-primary:`stable`", "Integrations for Bazel, GN, and CMake", "Bazel, CMake, GN, Rust"
   ":ref:`module-pw_build_info`", ":bdg-primary:`stable`", "Generate, embed, and parse build-related information", "Bazel, CMake, GN"
   ":ref:`module-pw_codegen`", ":bdg-secondary:`unstable`", "Source code generation utilities", "Python"
   ":ref:`module-pw_rust`", ":bdg-warning:`experimental`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   Build integrations <../pw_build/docs>
   Build info <../pw_build_info/docs>
   Code generation <../pw_codegen/docs>
   Rust <../pw_rust/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

.. toctree::
   :maxdepth: 1
   :hidden:

   ../size_optimizations
