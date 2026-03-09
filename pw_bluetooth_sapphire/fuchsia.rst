.. _module-pw_bluetooth_sapphire-fuchsia:

===================
Fuchsia development
===================
.. pigweed-module-subpage::
   :name: pw_bluetooth_sapphire

``//pw_bluetooth_sapphire/fuchsia`` currently contains the Fuchsia build
targets for building, running, and testing the ``bt-host`` and
``bt-hci-virtual`` packages.

Fuchsia's Sapphire Bluetooth stack integration uses the ``bt-host`` core
package which is built from Pigweed CI, uploaded to CIPD, and rolled
automatically. The ``bt-host`` package is assembled into products in any
Fuchsia build which uses Bluetooth. The ``bt-hci-virtual`` package is included
in some builds for testing. See `bt-host README
<https://fuchsia.googlesource.com/fuchsia/+/refs/heads/main/src/connectivity/bluetooth/core/bt-host/README.md>`__
for more details on product assembly.

.. note::
   Every ``bazelisk`` invocation needs ``--config=fuchsia`` whenever the target
   is a fuchsia-specific target.
   These fuchsia-specific targets are disabled (marked incompatible with the
   target platform) by default to avoid polluting/conflicting with non-fuchsia
   pigweed builds.
   Specifying ``--config=fuchsia`` also allows ``@fuchsia_sdk`` backends to be
   specified for Pigweed dependencies.

----------------------------------------
Accessing ffx from a Pigweed environment
----------------------------------------
The ``ffx`` command is available as a subcommand of ``pw``:

.. code-block:: console

   pw ffx help

-------
Testing
-------

Running tests
=============
First, ensure the emulator is running or hardware running Fuchsia is
connected. Then run a test package with:

.. code-block:: console

   bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia/host/l2cap:test_pkg

Flags can also be passed to the test binary:

.. code-block:: console

   bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia/host/l2cap:test_pkg \
     -- --gtest_filter="*Example" --severity=DEBUG

.. note::
   If the test is unable to connect to the emulator, run ``pw ffx target
   remove --all`` first to clean your machine's target list.

You can also run the presubmit step that will start an emulator and run
all tests, but this is slow:

.. code-block:: console

   pw presubmit --step bthost_package

Emulator
========
To start the emulator, use the following command:

.. code-block::

   bazelisk run --config=fuchsia @fuchsia_products//:core.x64.emu -- --headless

To stop the running emulator, use the following command:

.. code-block::

   pw ffx emu stop --all

--------
Building
--------
.. note::
   See the main :ref:`Building <module-pw_bluetooth_sapphire-building>` section
   for instructions on building for your host machine (producing Linux/macOS
   test binaries).

To build the ``bt-host`` package, use one of the following commands:

.. tab-set::

   .. tab-item:: arm64

      .. code-block::

         bazelisk build --config=fuchsia //pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64

   .. tab-item:: x64

      .. code-block::

         bazelisk build --config=fuchsia //pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64

The ``bt-host.far`` package will end up in a Bazel build directory that will be
printed in the command output. For example:
``bazel-out/aarch64-fastbuild-e2b/bin/pw_bluetooth_sapphire/fuchsia/bt_host/bt-host.far``.
Note that ``bazel-out`` is symlinked from the root Pigweed directory.

Use the prebuilt in fuchsia.git
===============================
fuchsia.git developers can copy/link the ``bt-host.far`` file to
``//prebuilt/connectivity/bluetooth/bt-host/<arch>/`` and rename it to
``bt-host`` to replace the prebuilt that Fuchsia uses.

Note that copying the ``far`` file to fuchsia.git does not copy the debug symbols which
means that crashes won't be symbolicated. If you need debug symbols you can run the
following command in the Pigweed repository to register the symbols.

.. tab-set::

      .. tab-item:: arm64

         .. code-block:: console

            $ bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64.debug_symbols

      .. tab-item:: x64

          .. code-block:: console

             $ bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64.debug_symbols

Using a local Fuchsia SDK
=========================
If you are making changes to the Fuchsia SDK itself (e.g., modifying FIDL
definitions in a local ``fuchsia.git`` checkout), you can point Pigweed to use
your local SDK instead of the prebuilt one from CIPD.

1. **Build the SDK in your Fuchsia checkout:**

   .. code-block:: console

      fx build //sdk:final_fuchsia_sdk

2. **Find the canonical name of the SDK repository:**

   Because Pigweed uses Bzlmod, the repository name ``fuchsia_sdk`` might be
   mangled internally. To find the exact name you need to override, run:

   .. code-block:: console

      bazel mod dump_repo_mapping pigweed | grep fuchsia_sdk | python -m json.tool | jq .fuchsia_sdk

   This will return a mapping like ``fuchsia_sdk -> @@+_repo_rules5+fuchsia_sdk``.
   The canonical name is everything after the ``@@`` (e.g.,
   ``+_repo_rules5+fuchsia_sdk``).

3. **Override the SDK repository in Pigweed:**

   Use the ``--override_repository`` flag with the canonical name and an
   **absolute path** (Bazel does not expand ``~``):

   .. code-block:: console

      bazelisk build --config=fuchsia \
        --override_repository=+_repo_rules5+fuchsia_sdk=/home/user/code/fuchsia/out/default/obj/sdk/final_fuchsia_sdk \
        //pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64

4. **(Optional) Persistent configuration:**

   To avoid passing the flag every time, you can add it to a ``user.bazelrc``
   file in your Pigweed root:

   .. code-block:: none

      # user.bazelrc
      build --override_repository=+_repo_rules5+fuchsia_sdk=/home/user/code/fuchsia/out/default/obj/sdk/final_fuchsia_sdk

   .. note::
      If you encounter version mismatches with the rules themselves, you may
      also need to override ``rules_fuchsia`` using its canonical name (found
      via the same ``dump_repo_mapping`` process) and pointing it to
      ``/path/to/fuchsia/scripts/sdk/bazel``.

--------------------
Working with devices
--------------------

Inspect
=======
To query the current state of the ``bt-host`` component Inspect hierarchy, run:

#. ``pw ffx inspect list | grep bt-host`` to find the component's ``<moniker>``

#. ``pw ffx inspect show "<moniker>"``

   * Note that the full moniker from step 2 should be in quotations, e.g.
     ``pw ffx inspect show "core/bluetooth-core/bt-host-collection\:bt-host_000"``.

   * Wildcards can be passed into the selector as needed, e.g.
     ``pw ffx inspect show "core/bluetooth-core/bt-host-collection*"``.

--------------------
Editor configuration
--------------------

Clangd setup
============
to enable C/C++ code intelligence in your editor, Generate commands databases
(``compile_commands.json``) with the following command:

.. code-block:: console

   $ bazelisk run //:refresh_compile_commands_for_fuchsia_sdk

This will create a ``.compile_commands`` directory with a number of
subdirectories. For example:

* ``k8-fastbuild``: The default host configuration (linux), used when you run
  a test like ``bazelisk test //pw_status:status_test``. For macOS hosts,
  this is named ``darwin_arm64-fastbuild``.
* ``k8-opt-exec``: Host "exec" configuration. This is used to build tools
  like ``protoc`` that are required for various stages of the build. For macOS
  hosts, this is named ``darwin_arm64-opt-exec``.
* ``fuchsia_x64-fastbuild-ST-89a3eb73aaef``: Code intelligence for Fuchsia
  targets in ``pw_bluetooth_sapphire``.

Each of these directories provide a different view of the build graph. This
ensures ``#if`` conditions and defines correctly match the configuration, giving
you the most accurate code intelligence possible.

.. note::

   Not all configurations cover the same libraries, tests, and binaries. For
   example, the ``//:update_host_googletest_compile_commands`` command will
   cover much more of Pigweed (and ``pw_bluetooth_proxy``), but is configured
   specifically for host+Googletest.

Select one of these configurations by directing ``clangd`` to use the associated
directory:

.. code-block:: console

   --compile-commands-dir=/path/to/pigweed/.compile_commands/fuchsia_x64-fastbuild-ST-89a3eb73aaef

.. note::

   `b/469437909 <https://pwbug.dev/469437909>`__ The hex sequence at the end of
   the Fuchsia platform name is unfortunately unstable, and can change
   whenever different flags are passed to Bazel.

--------------
Infrastructure
--------------

Run Fuchsia presubmit tests
===========================
Presubmits for ``bt-host`` are captured in a dedicated separate builder,
``pigweed-linux-bazel-bthost``, rather than existing ones such as
``pigweed-linux-bazel-noenv``.

On the builder invocation console, there are a number of useful artifacts for
examining the environment during test failures. Here are some notable examples:

* ``bt_host_package`` ``stdout``: Combined ``stdout``/``stderr`` of the entire test orchestration and execution.
* ``subrunner.log``: Combined test ``stdout``/``stderr`` of test execution only.
* ``target.log``: The ``ffx`` target device's logs.
* ``ffx_config.txt``: The ``ffx`` configuration used for provisioning and testing.
* ``ffx.log``: The ``ffx`` host logs.
* ``ffx_daemon.log``: The ``ffx`` daemon's logs.
* ``env.dump.txt``: The environment variables when test execution started.
* ``ssh.log``: The SSH logs when communicating with the target device.

These presubmits can be also be replicated locally with the following command:

.. code-block::

   bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia:infra.test_all

.. note::
   Existing package servers must be stopped before running this command. To
   check for any existing package servers run ``lsof -i :8083`` and make sure
   each of those processes are killed.

.. note::
   You do not need to start an emulator beforehand to to run all tests this way.
   This test target will automatically provision one before running all tests.

Add a test to presubmit
=======================
Fuchsia test packages are those defined with a Fuchsia SDK rule like
``fuchsia_unittest_package``. All Fuchsia test packages need to be added to the
Fuchsia presubmit step or they will not be tested.

To add new Fuchsia test packages to presubmit, add the test package targets to
``//pw_bluetooth_sapphire/fuchsia/BUILD.bazel``.

Example:

.. code-block::

   # pw_bluetooth_sapphire/fuchsia/BUILD.bazel

   qemu_tests = [
       "//pw_bluetooth_sapphire/fuchsia/bt_host:integration_test_pkg",
       ...
   ]

Uploading to CIPD
=================
Pigweed infrastructure uploads ``bt-host`` artifacts to
`fuchsia/prebuilt/bt-host`_ and `fuchsia/prebuilt/bt-hci-virtual`_ via the
`pigweed-linux-bazel-bthost`_ builder by building the top level infra target:

.. code-block::

   # Ensure all dependencies are built.
   bazelisk build --config=fuchsia //pw_bluetooth_sapphire/fuchsia:infra

   # Get builder manifest file.
   bazelisk build --config=fuchsia --output_groups=builder_manifest //pw_bluetooth_sapphire/fuchsia:infra

The resulting file contains a ``cipd_manifests`` JSON field which references a
sequence of JSON files specifying the CIPD package path and package file
contents.

.. _fuchsia/prebuilt/bt-host: https://chrome-infra-packages.appspot.com/p/fuchsia/prebuilt/bt-host
.. _fuchsia/prebuilt/bt-hci-virtual: https://chrome-infra-packages.appspot.com/p/fuchsia/prebuilt/bt-hci-virtual
.. _pigweed-linux-bazel-bthost: https://ci.chromium.org/ui/p/pigweed/builders/pigweed.ci/pigweed-linux-bazel-bthost

.. _module-pw_bluetooth_sapphire-fuchsia-zxdb:

Using Zxdb: the Fuchsia debugger
================================

`Zxdb is a console-mode debugger`_ for native code running on Fuchsia.

To run Zxdb in pw_bluetooth_sapphire:

#. Start the emulator.

#. Run:

   .. code-block:: console

      # Connect to the debugger
      $ pw ffx debug connect

      # Attach test package from test output url (e.g. fuchsia-pkg://bazel.test.pkg.publish.anonymous/bt_host_fidl_tests_bazel#meta/fidl_test.cm)
      $ [zxdb] attach <test-package> (e.g. attach fidl_test.cm)

      # Set a breakpoint
      $ [zxdb] break <file.cc>:<line-number> (e.g. break fidl/profile_server.cc:384)

#. Run your test in a new terminal:

   .. code-block:: console

      $ bazelisk run --config=fuchsia //pw_bluetooth_sapphire/fuchsia/host/fidl:test_pkg

Once you have successfully connected to the debugger, see `further capabilities`_.

.. _Zxdb is a console-mode debugger: https://fuchsia.dev/fuchsia-src/development/debugger
.. _further capabilities: https://fuchsia.dev/fuchsia-src/development/debugger?hl=en#working_with_zxdb
