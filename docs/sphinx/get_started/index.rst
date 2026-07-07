.. _docs-get-started:

==========
Quickstart
==========
.. _docs-first-time-setup:

--------------------------------------------------
Configure your workstation for Pigweed development
--------------------------------------------------
Pigweed does its best to bundle all its dependencies into an isolated
environment using :ref:`module-pw_env_setup`. While this doesn't eliminate
all prerequisites, it greatly accelerates new developer onboarding.

The first-time setup guides below are required for any Pigweed-based project,
but only need to be done once per machine.

.. grid:: 2

   .. grid-item-card:: First-time setup
      :link: docs-first-time-setup-guide
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Prerequisites, first-time setup, and support notes for Linux, macOS, and
      Windows.

   .. grid-item-card:: Install Bazel
      :link: docs-install-bazel
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Recommendations on how to install Bazel.

---------------
Tour of Pigweed
---------------
Want a guided, hands-on tour of Pigweed's core features? Try our
brand new :ref:`Tour of Pigweed <showcase-sense>`!

.. grid:: 1

   .. grid-item-card:: :octicon:`rocket` Tour of Pigweed
      :link: showcase-sense
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Explore key Pigweed features, such as hermetic building, full C++
      code intelligence in VS Code, communicating with devices over RPC,
      host-side and on-device unit tests, and lots more.

------------------------------
Create a Pigweed-based project
------------------------------
.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` Bazel quickstart
      :link: https://cs.opensource.google/pigweed/quickstart/bazel
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Fork our minimal, Bazel-based starter project to create a new
      Pigweed project from scratch. The project includes a basic
      blinky LED program that runs on Raspberry Pi Picos and can
      be simulated on your development host.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Zephyr
      :link: docs-quickstart-zephyr
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn how to set up a C++-based Zephyr project that is ready to use
      Pigweed and then build the app with Zephyr's ``native_sim`` board.

   .. grid-item-card:: :octicon:`code` Examples
      :link: https://pigweed.dev/examples/index.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Check out the examples repo, a repository that outlines the recommended
      way to use Pigweed in a broader GN-based project. Note that Bazel is the
      recommended build system for new projects using Pigweed, whereas the
      examples repo uses GN.


.. grid:: 2

   .. grid-item-card:: :octicon:`code` Kudzu
      :link: docs-kudzu
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Study the code of Kudzu, a just-for-fun Maker Faire 2023 project that
      demonstrates complex Pigweed usage. This project also uses GN.

------------------------------------------
Integrate Pigweed into an existing project
------------------------------------------
Pigweed is modular: you can use as much or as little of it as you need.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Bazel quickstart
      :link: https://cs.opensource.google/pigweed/quickstart/bazel
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Fork our minimal, Bazel-based starter project to create a new
      Pigweed project from scratch. The project includes a basic
      blinky LED program that runs on Raspberry Pi Picos and can
      be simulated on your development host.



   .. grid-item-card:: :octicon:`rocket` Bazel
      :link: docs-bazel-integration
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to use Pigweed in an existing Bazel project: add Pigweed
      as a dependency, start using Pigweed modules, and set up static and
      runtime analysis.

   .. grid-item-card:: :octicon:`code-square` CMake
      :link: cmake-integration
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn how to use Pigweed in an existing CMake project: add Pigweed
      as a dependency, configure backends, and use modules.

   .. grid-item-card:: :octicon:`list-ordered` GitHub Actions
      :link: docs-github-actions
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn how to set up GitHub Actions to build and test your Bazel-based
      Pigweed project.

.. toctree::
   :maxdepth: 2
   :hidden:

   self
   First-time setup <first_time_setup>
   Install Bazel <pw://build/bazel/install.html>
   Bazel quickstart <pw://build/bazel/quickstart.html>
   Bazel integration <pw://build/bazel/integration/index.html>
   CMake integration <pw://build/cmake/integration/index.html>
   Static and runtime analysis <analysis>
   GitHub Actions <github_actions>
   Zephyr quickstart <zephyr>
