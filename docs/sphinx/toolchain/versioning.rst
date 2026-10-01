.. _toolchain-versioning:

================================
Versioning, rolls, and stability
================================
Pigweed Toolchain tracks tip-of-tree LLVM. Rolling the toolchain on a regular
cadence amortizes upgrade overhead over time, accelerates access to new
compiler features, and provides immediate feedback to upstream LLVM
developers. Pigweed's infrastructure builds new toolchain packages
continuously and an automated roller updates the pinned revision in Pigweed
after it passes CI.

-------------------------
How the version is pinned
-------------------------
.. list-table::
   :header-rows: 1

   * - Build system
     - Where the version is pinned
   * - Bazel
     - The ``LLVM_VERSION`` CIPD tag (a ``git_revision:`` of ``llvm-project``)
       in Pigweed's :cs:`MODULE.bazel`, consumed by the ``pw_cxx_toolchain``
       module extension.
   * - GN and CMake
     - The ``fuchsia/third_party/clang`` entry in your project's CIPD
       manifest, for example
       :cs:`pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json`.

------------------------
What this means for you
------------------------
* **Your toolchain version is pinned by the Pigweed revision you depend on.**
  Pinning the ``git_override`` commit (Bazel) or the CIPD tag (GN) gives you a
  byte-identical toolchain on every machine and every CI run.
* **You choose when to roll.** Updating the toolchain is a deliberate act of
  updating that pin. Toolchain changes land with the Pigweed changes needed to
  stay compatible with them, so rolling Pigweed and rolling the toolchain
  together is the supported path.
* **Rolling frequently is cheaper than rolling rarely.** Tip-of-tree LLVM
  moves quickly; the longer you wait, the more diagnostics and behavior
  changes you absorb at once.

.. _toolchain-targets-stability:

---------
Stability
---------
.. warning::

   Pigweed's upstream toolchain definitions are subject to change without
   notice. Toolchain target names, flags, and defaults may be renamed or
   removed between Pigweed revisions. Pin the Pigweed revision that you depend
   on and expect to make small updates when you roll it. If you need stronger
   guarantees, talk to us (:ref:`toolchain-support`).
