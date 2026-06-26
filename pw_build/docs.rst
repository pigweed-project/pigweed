.. _module-pw_build:

========
pw_build
========
.. pigweed-module::
   :name: pw_build

The ``pw_build`` module provides a wide assortment integrations, tooling, and
libraries to address many common build system needs. Whether you're trying
to provide an easy way to group builds together, convert an ELF file to a .bin
file, or compile embedded binary data into a library or firmware image,
``pw_build`` is the place to look.

This module is also home to the :ref:`module-pw_build-workflows-launcher`,
a tool designed to simplify the management of various build system
configurations and expose developer-facing tooling via easy-to-use shortcuts.
This makes it easy to quickly customize project-specific presubmit flows, add
shortcuts for tools like Pigweed's :ref:`module-pw_presubmit-format` utility,
and prescribe build configuration variants supported by your project.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Workflows Launcher
      :link: module-pw_build-workflows-launcher
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Quickly launch project-specific builds, tools, and grouped build actions.

   .. grid-item-card:: :octicon:`terminal` Project Builder
      :link: module-pw_build-project_builder
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Python API for orchestrating multi-step builds.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Bazel
      :link: module-pw_build-bazel
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Build integrations for Pigweed's primary build system.

   .. grid-item-card:: :octicon:`code-square` GN / Ninja
      :link: module-pw_build-gn
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Build integrations for Pigweed's original build system.

.. toctree::
   :hidden:
   :maxdepth: 1

   gn
   bazel
   project_builder
   python_api
   linker_scripts
   workflows_launcher
