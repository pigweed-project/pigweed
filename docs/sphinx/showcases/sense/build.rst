.. _showcase-sense-tutorial-build:

===============
3. Build an app
===============
You can start building right away. There's no need to manually
install dependencies or toolchains. Bazel automates dependency and
toolchain management. Try building the ``blinky`` bringup app now:

.. _task: https://code.visualstudio.com/docs/editor/tasks

.. tab-set::

   .. tab-item:: VS Code
      :sync: vsc

      #. In **Bazel Targets** right-click the **//apps/blinky** folder
         and select **Build Package Recursively**.

         .. figure:: https://www.gstatic.com/pigweed/sense/build_package_recursively_v4.png

         A `task`_ launches in a VS Code terminal.
         Bazel builds all targets that it finds within the ``//apps/blinky``
         directory. If there were targets in subdirectories, those would
         get built, too.

         A successful build looks like this:

         .. code-block:: console

            INFO: Found 17 targets...
            INFO: Elapsed time: 174.103s, Critical Path: 13.34s
            INFO: 2375 processes: 480 internal, 1895 linux-sandbox.
            INFO: Build completed successfully, 2375 total actions

         .. tip::

            When you want to build just a single target, you can use
            **Build Target** instead. This is useful when you know you
            only need to build a single target (such as compiling a binary
            for a specific platform) and want to do it quickly. Here we
            had you build all the ``blinky`` targets in one go because you'll
            be using a lot of them in later parts of the tutorial anyways.

      #. Once the build finishes, press any key to close the task's terminal.

   .. tab-item:: CLI
      :sync: cli

      Run the following command:

      .. code-block:: console

         bazelisk build //apps/blinky/...

      A successful build looks similar to this:

      .. code-block:: console

         INFO: Analyzed 17 targets (464 packages loaded, 28991 targets configured).
         INFO: From Linking external/rules_libusb~~libusb~libusb/libusb-1.0.so:
         # ...
         INFO: Found 17 targets...
         INFO: Elapsed time: 314.300s, Critical Path: 26.73s
         INFO: 2496 processes: 582 internal, 1914 linux-sandbox.
         INFO: Build completed successfully, 2496 total actions

      .. tip::

         Pigweed recommends always running ``bazelisk`` rather than ``bazel``
         because ``bazelisk`` ensures that you always run the correct version
         of Bazel, as defined in a project's ``.bazelversion`` file. In some
         cases ``bazel`` also does the right thing, but it's easier to remember
         to just always use ``bazelisk``.

.. admonition:: Troubleshooting

   * **Warnings during the build**. As long as you see ``Build completed
     successfully`` you should be able to complete the rest of the
     tutorial. We generally work to remove all these warnings but they
     pop up from time-to-time as the Sense codebase and its dependencies
     evolve.

   * **Long build times**. 5-10 minutes is typical for the first
     build. Pigweed builds a lot of dependencies from source, such as
     the Protocol Buffer compiler, ``libusb``, and more.

.. _showcase-sense-tutorial-build-summary:

-------
Summary
-------
You've now got some familiarity with how to build binaries in Bazel-based
projects.

Next, head over to :ref:`showcase-sense-tutorial-intel` to learn how to
use the Pigweed extension for VS Code to navigate a codebase that
supports multiple hardware platforms. If you're not using VS Code you
can skip ahead to :ref:`showcase-sense-tutorial-hosttests` because this
code intelligence feature is currently only supported in VS Code.
