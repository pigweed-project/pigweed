.. _docs-workflows-build-drivers:

=======================
Workflows build drivers
=======================
Workflows build drivers are tools that translate workflow configurations in a
``workflows.json`` file into a sequence of executable actions.

------------
How it Works
------------

#. **Configuration initialization**: The Workflows launcher reads a
   ``workflows.json`` configuration file to identify existing build
   configurations. (``workflows.textproto``, ``workflows.toml``, and
   ``workflows.yaml`` are also supported, as long as only one ``workflows.*``
   file exists).
#. **Subtool identification**: The subtool requested by the user (e.g.
   ``format``) is located in the configuration, and the associated
   ``build_type`` is mapped to a registered driver.
#. **Driver invocation**: The Workflows launcher feeds the requested build
   to the driver as a ``BuildDriverRequest`` formatted as ProtoJSON.
#. **Driver response**: The driver receives the request, processes each job,
   and generates a ``BuildDriverResponse`` message that is emitted as ProtoJSON.
   This response contains a sequence of actions for each job, detailing the
   commands that should be run.
#. **Execution**: The Workflows launcher receives the ``BuildDriverResponse``,
   applies any necessary post-processing, and executes the actions in the
   specified order.

This separation of concerns allows the Workflows system to be extended with new
build systems by creating new drivers, without modifying the core launcher.

-----------------------
Driver-specific options
-----------------------
Build drivers may optionally define a ``proto`` message for driver-specific
options. These options are specified the ``driver_options`` field of a
``BuildConfig``. This allows different build types to specify more strongly
structured configuration that affects build behavior in ways beyond strictly
adding CLI arguments or setting environment variables.

Because driver options are structured as ``google.protobuf.Any`` messages, the
configuration loader will either need awareness of all supported message types,
or a way to dynamically load message definitions as needed. The easiest way to
introduce a new known message is to simply ``import`` it from any dependency of
the Workflows launcher.

------------
Bazel Driver
------------
The Bazel driver translates ``bazel`` build types to a series of actions.

When generating actions for a ``Build``, it includes:

#. Canonicalizing Bazel flags. This is done to ensure build targets don't leak
   into the list of build arguments.
#. Running ``bazelisk build`` with the specified targets and arguments.
#. Running ``bazelisk test`` with the same targets and arguments, unless
   disabled by the ``no_test`` driver option.

When generating actions for a ``Tool``, it includes:

#. Canonicalizing Bazel flags. This is done to ensure build targets don't leak
   into the list of build arguments.
#. Running ``bazelisk run`` with the specified target and arguments.

If ``args`` includes ``--lockfile_mode``, the driver also passes it before
``--`` to ``canonicalize-flags``, since that step resolves the module graph.

Bazel-specific options
======================
See :cs:`pw_build/proto/pigweed_build_driver.proto`

.. literalinclude:: proto/pigweed_build_driver.proto
   :language: protobuf
   :start-at: message BazelDriverOptions
   :end-at: }
