.. _target-android:

=======
Android
=======
The Android target supports building Pigweed on devices using an Android
Native Development Kit (NDK).

.. warning::
  This target is under construction, not ready for use, and the documentation
  is incomplete.

-----
Setup
-----
You must first download and unpack a copy of the `Android NDK`_ and let Pigweed
know where that is located using the ``pw_android_toolchain_NDK_PATH`` build
arg.

.. _Android NDK: https://developer.android.com/ndk

You can set Pigweed build options using ``gn args out``.

--------
Building
--------
To build for this Pigweed target, simply build the top-level "android" Ninja
target. You can set Pigweed build options using ``gn args out`` or by running:

.. code-block:: console

   $ gn gen out --args="
     pw_android_toolchain_NDK_PATH=\"/path/to/android/ndk\"
     "

On a Windows machine it's easier to run:

.. code-block:: console

   $ gn args out

That will open a text file where you can paste the args in:

.. code-block:: text

   pw_android_toolchain_NDK_PATH = "/path/to/android/ndk"

Save the file and close the text editor.

Then build with:

.. code-block:: console

   $ ninja -C out android

This will build Pigweed for all supported Android CPU targets at the default
optimization level, currently arm, arm64, x64, and x86.

To build for a specific CPU target only, at the default optimization level:

.. code-block:: console

   $ ninja -C out arm64_android

Or to build for a specific CPU target and optimization level:

.. code-block:: console

   $ ninja -C out arm64_android_size_optimized

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
The following table lists all modules related to Android.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_android_toolchain`", ":bdg-secondary:`unstable`", "GN toolchains for the Android NDK", "GN"
   ":ref:`module-pw_build_android`", ":bdg-primary:`stable`", "Utilities for using Pigweed in Android platform", "C++"
   ":ref:`module-pw_chre`", ":bdg-warning:`experimental`", "Android Context Hub Runtime Environment backend", "C++"
   ":ref:`module-pw_log_android`", ":bdg-primary:`stable`", "Android logging backend", "C++"

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   NDK toolchains <../../pw_android_toolchain/docs>
   Build utilities <../../pw_build_android/docs>
   CHRE <../../pw_chre/docs>
   Logging <../../pw_log_android/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
