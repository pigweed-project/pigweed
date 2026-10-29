.. _docs-support-policy:

================
Support policies
================
This page documents Pigweed's version support policies for key build tools and
languages.

.. _docs-bazel-version-support:

-----
Bazel
-----
.. LINT.IfChange(bazel_compatibility)

Pigweed currently supports **Bazel 8.5.0** through the latest release of
**Bazel 9**. The minimum supported version is declared via
``bazel_compatibility`` in Pigweed's :cs:`MODULE.bazel` file.

.. LINT.ThenChange(//MODULE.bazel:bazel_compatibility)

We strongly recommend using the latest version of Bazel that Pigweed supports.

Pigweed will quickly ensure that we support the latest active version of Bazel,
which most of the time just involves trivial changes if any.

.. _Foundational C++ Support Policy: https://opensource.google/documentation/policies/cplusplus-support

Version support policy
======================
In alignment with Google's `Foundational C++ Support Policy`_, Pigweed will
support the latest Bazel release (excluding pre-releases), which Bazel calls
their `Active release <https://bazel.build/release>`__, as well as the previous
major version release for 12 months after it transitions into maintenance mode.

Pigweed will attempt to maintain compatibility with all minor versions of Bazel
for each major release. However, this may not be possible, particularly with
old versions of the prior/maintenance release. The primary constraints are from
the Bazel versions supported by our Bazel dependencies, most of which we have
little control over.

Pigweed will be constantly updating to the latest versions of our dependencies,
to keep up with changes to them, and to pick up bug fixes. Core dependencies
(including ``protobuf``, ``rules_cc``, ``rules_python``, and ``rules_rust`` to
name a few) may use newer Bazel features, requiring us to raise our minimum
supported Bazel version on little or no notice, when we encounter CI failures
with that version.

.. _September 22, 2026 Protocol Buffers announcement: https://protobuf.dev/news/2026-09-22/

Notice of upcoming version support changes
==========================================
- Early in 2027, Pigweed will upgrade to ``protobuf`` v38. Per the
  `September 22, 2026 Protocol Buffers announcement`_, that version of
  ``protobuf`` will no longer support Bazel 8, and as a result Pigweed will
  also no longer be able to support Bazel 8. At that point Pigweed would
  expect to support Bazel 10 (pending release), as well as Bazel 9 (which would
  then be in maintenance mode).

- In 2028, almost certainly due to a similar upgrade to ``protobuf``, Pigweed
  will drop Bazel 9 support, and only support Bazel 11 and 10.

.. _official supported versions: https://devguide.python.org/versions/
.. _docs-concepts-python-version:

------
Python
------
Pigweed officially supports Python **3.11** (minimum supported version) through
**3.14** and follows Python's `official supported versions`_ and drops
support for Python versions as they reach end-of-life.

.. list-table::
   :header-rows: 1

   * - Python Version
     - Supported Build Systems
   * - 3.11 (minimum)
     - GN
   * - 3.12
     - Bazel
   * - 3.13
     - GN, Planned: Bazel
   * - 3.14
     - Planned: GN, Bazel
   * - 3.15
     - Planned: GN, Bazel
