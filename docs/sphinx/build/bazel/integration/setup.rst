.. _docs-bazel-integration-add-pigweed-as-a-dependency:

========================================
Set up Pigweed as an external dependency
========================================
Pigweed only supports `bzlmod
<https://bazel.build/external/overview#bzlmod>`__ based projects.

Add Pigweed as a ``bazel_dep`` with a ``git_override``:

.. code-block:: python

   bazel_dep(name = "pigweed")

   git_override(
         module_name = "pigweed",
         commit = "c00e9e430addee0c8add16c32eb6d8ab94189b9e",
         remote = "https://pigweed.googlesource.com/pigweed/pigweed.git",
   )

You can find the latest tip-of-tree commit in the **History** tab in
`CodeSearch <https://cs.opensource.google/pigweed/pigweed>`__.

Pigweed is not yet published to the `Bazel Central Registry
<https://registry.bazel.build/>`__. If this is a pain point for you,
please reach out to us on `chat <https://discord.gg/M9NSeTA>`__.

Alternative: Add Pigweed as Git submodule
=========================================
If you manage your dependencies as submodules, you can add Pigweed as a
submodule, too, and then add it to your ``MODULE.bazel`` as a
`local_path_override
<https://bazel.build/rules/lib/globals/module#local_path_override>`__:

.. code-block:: python

   local_path_override(
         module_name = "pigweed",
         path = "third_party/pigweed",
   )

.. note::

   If you put external Bazel modules like Pigweed as submodules within
   your source tree (e.g. under ``third_party/``), remember to add them
   to your `.bazelignore <https://bazel.build/run/bazelrc#bazelignore>`__
   file so that Bazel knows they're third-party!

Set the required Bazel flags
============================
Pigweed projects need to set certain flags in their ``.bazelrc``. These
generally pre-adopt Bazel features that will become default in the future and
improve cache performance, disambiguate Python imports, etc. These flags are
listed below.  Unfortunately there's no way to automatically import them, see
:bug:`353750350`.

.. literalinclude:: ../../../pw_build/pigweed.bazelrc

Set the recommended Bazel flags
===============================
While these flags are not required to use Pigweed, they significantly improve
Bazel's usability. Turn these on, selectively tuning them to your needs.
Unfortunately there's no way to automatically import them, see
:bug:`353750350`.

.. literalinclude:: ../../../pw_build/pigweed_recommended.bazelrc

Next steps
==========
Start :ref:`using Pigweed modules <docs-bazel-integration-modules>` or
:ref:`static and runtime analysis <docs-automated-analysis>`!
