.. _docs-bazel-integration:

==========================================
Using Pigweed in an existing Bazel project
==========================================
This guide explains how to start using Pigweed in your existing Bazel-based C or
C++ project. We'll assume you're familiar with the build system at the level of
the `Bazel tutorial <https://bazel.build/start/cpp>`__.

.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` Setup
      :link: docs-bazel-integration-add-pigweed-as-a-dependency
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Add Pigweed as an external dependency and configure the required Bazel
      flags.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Use Pigweed modules
      :link: docs-bazel-integration-modules
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Use Pigweed modules in an existing project.

   .. grid-item-card:: :octicon:`code` Static and runtime analysis
      :link: docs-automated-analysis
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Improve your project's code quality by enabling static analysis
      and runtime sanitizers.

------------------------
Supported Bazel versions
------------------------
See :ref:`docs-bazel-version-support` for Pigweed's supported Bazel versions and
version support policy.

.. toctree::
   :maxdepth: 1
   :hidden:

   Add Pigweed as a dependency <setup>
   Use Pigweed modules <modules>
