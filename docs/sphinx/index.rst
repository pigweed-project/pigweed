:html_theme.sidebar_secondary.remove:

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
.. include:: changelog/2026/04.rst
   :start-after: .. changelog_highlights_start
   :end-before: .. changelog_highlights_end

And more! See :doc:`changelog/2026/04`.

----------------
What is Pigweed?
----------------
.. raw:: html

   <!-- Add a little space between the H2 and the cards. This is an edge case
        that only occurs on the homepage so it's best to just hack the fix
        locally here. -->
   <br>

.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` Libraries
      :link: docs-concepts-embedded-development-libraries
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Over 150 libraries ("modules") enabling you to use modern C++ and
      software development best practices without compromising performance,
      code size, or memory

.. grid:: 1

   .. grid-item-card:: :octicon:`tools` Automation
      :link: docs-concepts-build-system
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Easier automated building, testing, and linting for Bazel, GN,
      and CMake projects

.. grid:: 1

   .. grid-item-card:: :octicon:`container` Environments
      :link: docs-concepts-development-environment
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Hermetic and reproducible development environments for your entire team -
      no more struggling with toolchains and the "it worked on my machine" problem

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

.. toctree::
   :maxdepth: 1
   :hidden:

   overview
   get_started/index
   Tour <showcases/sense/index>
   modules
   Contributing <contributing/index>
   What's new <changelog/index>
   concepts/index
   build/index
   targets
   Toolchain <toolchain>
   size_optimizations
   os/index
   third_party_support
   showcases/index
   community/index
   Blog <blog/index>
   api/index
