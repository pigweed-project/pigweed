.. _module-pw_ide-bootstrap-editors:

======================
Legacy support via CLI
======================
.. pigweed-module-subpage::
   :name: pw_ide

The :ref:`pw_ide CLI<module-pw_ide-bootstrap>` provides an alternative method
of configuring Visual Studio Code for :ref:`bootstrap Pigweed projects<module-pw_ide-contributing-design-projects-bootstrap>`
that doesn't rely on the use of an extension. This page documents that
functionality.

.. tip::

   Are you working on a :ref:`Bazel Pigweed project<module-pw_ide-contributing-design-projects-bazel>`?
   These instructions won't work for you. Use the :ref:`extension<module-pw_ide-bazel>`!

-----
Usage
-----
Running ``pw ide sync`` will automatically generate settings for Visual Studio
Code. ``pw_ide`` comes with sensible defaults for Pigweed projects, but those
can be augmented or overridden at the project level or the user level using
``pw_project_settings.json`` and ``pw_user_settings.json`` respectively. The
generated ``settings.json`` file is essentially a build artifact and shouldn't
be committed to source control.

.. note::

   You should treat ``settings.json`` as a build artifact and avoid editing it
   directly. However, if you do make changes to it, don't worry! The changes
   will be preserved after running ``pw ide sync`` if they don't conflict with
   with the settings that command sets.

The same pattern applies to ``tasks.json``, which provides Visual Studio Code
tasks for ``pw_ide`` commands. Access these by opening the command palette
:kbd:`Ctrl+Shift+P` (:kbd:`Cmd+Shift+P` on Mac), selecting ``Tasks: Run Task``,
then selecting the desired task.

.. tip::

   The default tasks that ``pw ide sync`` generates can serve as an example of
   how to launch your own tasks within an activated environment.

The same pattern also applies to ``launch.json``, which is used to define
configurations for running and debugging your project. Create a
``pw_project_launch.json`` with configurations that conform to the Visual Studio
Code `debugger configuration format <https://code.visualstudio.com/docs/editor/debugging>`_.

Commands
========
These commands are actually `tasks <https://code.visualstudio.com/docs/editor/tasks>`_,
so you run them by running the ``Tasks: Run Task`` command, then selecting the
task.

.. describe:: Pigweed: Sync IDE

   Runs ``pw ide sync`` without you needing to type it into an activated shell.

.. describe:: Pigweed: Set C++ Target Toolchain

   Allows you to select the target toolchain to use for code intelligence from
   a dropdown list of available options.

.. describe:: Pigweed: Set Python Virtual Environment

   Allows you to provide the path to the Python virtual environment. This is
   normally configured automatically to use the Pigweed environment's Python,
   but for unusual project structures, you may need to select another Python
   environment.
