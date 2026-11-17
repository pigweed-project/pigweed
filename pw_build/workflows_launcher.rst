.. _module-pw_build-workflows-launcher:

==================
Workflows Launcher
==================
.. pigweed-module-subpage::
   :name: pw_build

The workflows launcher is a command-line tool for running tools, builds, and
other workflows defined in a ``workflows.json`` file. It provides a centralized
and discoverable way to execute common development tasks.

.. admonition:: Note
   :class: warning

   `b/425973227 <https://pwbug.dev/425973227>`__: This tool is currently under
   active development, and will change significantly as the experience is
   refined.

.. _module-pw_build-workflows-launcher-getting-started:

-----------
Get Started
-----------

New project setup
=================

#. Add a ``native_binary`` entry point to your root ``BUILD.bazel`` file:

   .. code-block:: python

      load("@bazel_skylib//rules:native_binary.bzl", "native_binary")

      native_binary(
          name = "pw",
          src = "@pigweed//pw_build/py:workflows_launcher",
      )

#. Add a ``workflows.json`` file to the root of your project.

   .. code-block:: console

      $ echo {} >> workflows.json

#. Copy or symlink the helper ``pw`` entry point to the root of your project.

Now you're ready to start exploring the Workflows tool.

General usage (in Pigweed)
==========================
To see available commands, launch the ``pw`` entry point at the root of
the Pigweed repository.

.. code-block:: console

   $ ./pw

The launcher will automatically find and load a workflows config file in the
current directory or any parent directory. It looks for any of the following
filenames, and requires exactly one of them be present in a directory. This doc
will use JSON in examples.

* ``workflows.json``
* ``workflows.textproto``
* ``workflows.toml``
* ``workflows.yaml``

.. note::

   The Workflows tool does not yet offer any GN or CMake build integration.
   Please use :ref:`module-pw_env_setup` and :ref:`module-pw_presubmit`.

--------
Commands
--------
The Workflows launcher provides a set of built-in commands, and will later
support commands generated from the ``workflows.json`` file.

Built-in Commands
=================

``describe``
------------
The ``describe`` command prints a human-readable description of a ``Tool``,
``Build``, ``BuildConfig``, or ``TaskGroup`` from the ``workflows.json`` file.
This is useful for inspecting the configuration of a specific workflow, and is
particularly helpful for debugging programmatically-generated configurations.

Example:

.. code-block:: console

   $ ./pw describe format

This will print the configuration for the ``format`` tool:

.. code-block::

   ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
    ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
    ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
    ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
    ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀

   name: "format"
   description: "Find and fix code formatting issues"
   use_config: "bazel_default"
   target: "@pigweed//:format"
   analyzer_friendly_args: "--check"

``build``
---------
The ``build`` command launches the build for the requested build name.

Example:

.. code-block:: console

   $ ./pw build all_host

.. note::

   `b/425973227 <https://pwbug.dev/425973227>`__: This doesn't offer any
   configurability or additional argument handling today.

Generated Commands
==================
The workflows launcher generates commands from the ``tools`` and ``groups``
defined in the ``workflows.json`` configuration file.

Tools
-----
For each tool in the ``tools`` list, a command is created with the same name.
Running this command will execute the tool's specified command.

From the :ref:`example workflows.json <module-pw_build-workflows-launcher-example-configuration>`,
the following command is created:


.. code-block:: console

   $ ./pw format

This will launch a ``bazel run`` invocation of Pigweed's code formatter tool.

Groups
------
For each group in the ``groups`` list, a command is created with the same name.
Running this command will execute all the workflows in the group in sequence.

From the :ref:`example workflows.json <module-pw_build-workflows-launcher-example-configuration>`,
the following command is created:

.. code-block:: console

   $ ./pw presubmit


This will launch a series of builds followed by code health check tooling as
enumerated by the group named ``presubmit``.

-------------
Configuration
-------------
The Workflows launcher is configured via
`ProtoJSON <https://protobuf.dev/programming-guides/json/>`__. The schema
lives in ``workflows.proto``:

.. dropdown:: Configuration schema

   .. literalinclude:: proto/workflows.proto
      :language: protobuf
      :start-after: syntax = "proto3";

The workflows launcher searches for a ``workflows.json`` file from the current
working directory, traversing through parent directories as needed. When a
``workflows.json`` file is found, it is loaded as a ``WorkflowSuite`` Protobuf
message. This configuration is then used to drive the rest of the launcher's
behavior.

.. _module-pw_build-workflows-launcher-example-configuration:

.. dropdown:: Example ``workflows.json``

   .. literalinclude:: py/workflows_launcher_test.py
      :language: json
      :start-after: _EXAMPLE_CONFIG = """
      :end-before: """

.. _module-pw_build-workflows-launcher-mixins:

Mix-ins
=======
A mix-in is a shared build configuration that is layered on top of another
configuration, so builds that need one or two extra options don't have to copy
the whole configuration. Mark it with ``"mixin": true`` and list it in a build's
or tool's ``use_config_mixins``:

.. code-block:: json

   {
     "configs": [
       {"name": "host", "build_type": "bazel", "args": ["--config=host"]},
       {"name": "lint", "mixin": true, "args": ["--config=lint"]}
     ],
     "builds": [
       {
         "name": "host_lint",
         "use_config": "host",
         "use_config_mixins": ["lint"],
         "targets": ["//..."]
       }
     ]
   }

``host_lint`` builds with a configuration named ``host+lint`` and the arguments
``--config=host --config=lint``. Mix-ins are applied in the order listed:

* ``args`` are appended, so for single-value flags the last mix-in wins.
* ``env`` variables set by a mix-in replace earlier values.
* ``driver_options`` are merged with Protobuf ``MergeFrom`` semantics and must
  all use the same message type.
* ``build_type`` may be omitted on a mix-in unless it sets ``driver_options``;
  if set, it must match the base.
* The merged configuration's name, and therefore its output directory, is the
  base name followed by each mix-in name, joined with ``+``.

A mix-in can only add or override values. Because of Protobuf default values,
it can't, for example, set ``no_test`` back to ``false``.

The following are reported as errors when the configuration is loaded:

* A listed mix-in doesn't exist, isn't a mix-in, or is listed more than once.
* A mix-in is used as ``use_config`` or as an inline ``build_config``.
* A mix-in's ``build_type`` or ``driver_options`` type doesn't match, or the
  ``driver_options`` type isn't known to any build driver.
* A mix-in sets ``driver_options`` but not ``build_type``.
* A merged name matches another build configuration's name, or the merged name
  of a different combination.

.. _module-pw_build-workflows-launcher-commands:

Build programming
=================
The definition of how build configurations become a series of actions is defined
by :ref:`docs-workflows-build-drivers`.

.. toctree::
   :hidden:
   :maxdepth: 1

   workflows_build_drivers
