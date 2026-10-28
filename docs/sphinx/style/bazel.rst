.. _docs-pw-style-bazel:

===========
Bazel style
===========
In general, Pigweed follows the `Bazel Style Guide
<https://bazel.build/build/style-guide>`__. This document provides some
additional guidance.

------
Naming
------

General
=======
All names in build and Starlark files with the exception of
`Providers <https://bazel.build/rules/lib/builtins/Provider>`__ should use
snake_case naming style.

Providers use CamelCase and always end with the ``Info`` suffix.

Constants
=========
Constants exported by ``.bzl`` files should be all uppercase and prefixed with
``PW_``. Private constants must be prefixed with an underscore ``_``, but do
not need any ``PW_`` prefix.

.. _docs-pw-style-bazel-naming-rules:

Rules
=====
All rules should begin with a ``pw_`` prefix. When possible, name rules as nouns
describing the resulting artifact rather than verbs.

.. admonition:: Bad names
   :class: error

   * ``pw_sign_binary``: Named like a verb.
   * ``generated_header``: Missing ``pw_`` prefix.

.. admonition:: Good names
   :class: checkmark

   * ``pw_signed_binary``: Clearly intended to produce a new artifact rather
      than suggesting in-place modification.
   * ``pw_generated_header``: Properly ``pw_`` prefixed to reduce name
      collisions.

Macros
======
Macros that wrap rules should follow :ref:`docs-pw-style-bazel-naming-rules`.
Macros that are effectively Starlark helper methods may be named as verbs when
appropriate.

Sub-target naming
=================
If a macro generates multiple targets, it should follow the following rules:

* Public sub-targets should be documented in the macro docstring and use a
  dot ``.`` to separate the base rule name from the sub-target name.

  Example: ``my_firmware_bundle.metadata``

* Private sub-targets should NOT be documented, and should be named with a
  dot and underscore ``._`` to separate the base rule name from the sub-target
  name.

  Example: ``my_firmware_bundle._hidden_metadata``

Starlark files (.bzl)
=====================
Starlark files should be named exactly after the primary rule exported by the
file.

Example: ``pw_flipped_table.bzl`` that exports a rule named ``pw_flipped_table``.

-----------
Build files
-----------

Do not reference ``@pigweed`` from Pigweed
==========================================
All labels and ``load()`` statements within Pigweed that reference other things
in Pigweed should **not** use ``@pigweed`` in the label name. Instead, just use
the project-absolute path to the ``.bzl`` file to load.

Example: ``load("//pw_build:pw_cc_binary.bzl", "pw_cc_binary")``

Reasoning: Referencing things in the current repository via the repository
name is an antipattern, and can introduce subtle bugs in ``WORKSPACE``
projects.

Avoid ``glob`` when possible
============================
Pigweed only permits ``glob()`` for cases where the collection of inputs is
truly variable, or infeasible to exhaustively express. The biggest example of
this is ``BUILD`` files for external dependencies (which may have different
source file paths/names for different versions) or binary packages (e.g.
toolchains, which have many supporting files).

Reasoning: The ``glob()`` function in Bazel is quite expensive at scale, so
it's best to avoid it. More background can be found in the Bazelcon talk
`How Bazel handles globs <https://youtu.be/ZrevTeuU-gQ?si=RheUpWGHldLqvuZ3>`__.

Do not use ``$(location :foo)``
===============================
The ``location`` keyword is error-prone. Use ``execpath`` for actions that are
only relevant to the build (e.g. ``run_binary``). Use ``rlocationpath`` when
exposing paths to tools that will be loaded via runfiles libraries. ``rootpath``
should be used when exposing a file path to a binary via arguments or
environment variable (particularly for ``native_binary`` and ``*_test`` rules).

Do not ``load`` from dev dependencies in public Build files
===========================================================
If a ``BUILD.bazel`` file contains any target with ``visibility =
["//visibility:public"]``, it may not contain ``load`` statements referencing
dev dependencies.

Instead, do one of the following, as appropriate:

#. Mark all all build targets in the ``BUILD.bazel`` file as ``visibility =
   "//:__subpackages__"`` (to make is clear that they cannot be used by downstream
   projects).
#. Move the targets requiring the ``load`` to a different
   ``BUILD.bazel`` file.
#. Remove the ``dev_dependency`` attribute from the dependency. Do not take this
   step lightly: it will require all Pigweed downstream projects to provide this
   dependency, which may require work if they vendor their dependencies.

Reasoning: Bzlmod allows `bazel_dep
<https://bazel.build/rules/lib/globals/module#bazel_dep>`__ targets to be
registered with ``dev_dependency=True``. These dependencies are ignored by
downstream projects. This is good because it trims their dependency graph.
But it means downstream projects can't use any functionality that the dev
dependencies were used for in upstream.

In particular, a downstream project cannot reference *any target* in an upstream
``BUILD.bazel`` file that loads a macro or a rule from a dev dependency.

To make this restriction clear to anyone working in upstream Pigweed, explicitly
express it through the ``visibility`` attribute.

Always tag deprecated targets ``manual``
========================================
Build targets with a ``deprecation`` message will emit warnings whenever they
are built. This means two things:

1. Targets should only have a ``deprecation`` message when *nobody* should be
   using them anymore. **NOT** as an introduction to a migration.
2. Tagging the target with ``manual`` makes it clear when the deprecated target
   is no longer being explicitly referenced.

This means that nothing in Pigweed should directly reference any targets with
a ``deprecation`` message, and clean builds in Pigweed should not emit
deprecation warnings.

Reasoning: Warnings are a double-edged sword. When used sparingly, they can
allow soft transitions that signal to users when action needs to be taken. If
used too often, though, they can pollute build output and users will become
blind to the warnings. For this reason, if you aren't willing to mark a target
with ``tags = ["manual"]`` for fear of it needing build coverage, you likely
aren't fully finished with a migration. Typically, a completed migration means
the deprecated target is merely is an ``alias`` that points to the new target.

---------------------
Starlark files (.bzl)
---------------------

Avoid monolithic Starlark files
===============================
In general, Starlark files should export a single rule or macro and be named
identically to the exported rule. A Starlark file may export multiple symbols
if they are closely related helper functions, or in rare cases adjacent rules
that are *very* closely related. Avoid creating or extending "grab bag" Starlark
files that contain collections of vaguely similar rules and macros.

Reasoning: Monolithic Starlark files can quickly pull in many external
dependencies accidentally, which prevents Bazel from efficiently fetching
only the external repositories that are actually used by build rules that need
to be evaluated.

Rule-wrapper macro signatures
=============================
* Macros that wrap other rules should always begin with an asterisk ``*`` as
  the first argument to prevent rules from being declared with positional
  arguments.
* The second argument should always be ``name``.
* Nearly all rule-wrapper macros should capture and forward ``**kwargs`` to the
  underlying rules to properly support
  `Attributes common to all build rules <https://bazel.build/reference/be/common-definitions#common-attributes>`__.

Example:

.. code-block:: py

   def pw_generated_header(*, name, src, dest, **kwargs):
       # Implementation...

Hard-coded labels in macros
===========================
Hard-coded labels in macro definitions are permitted, but they should usually be
wrapped in a `Label() <https://bazel.build/rules/lib/builtins/Label#Label>`__
constructor.

Reasoning: When a macro stamps out a string intended to become a label, the
string is evaluated **relative to the site where the macro is used**. This means
that ``"//:foo"`` has different meanings based on where the macro is stamped
out: ``@pigweed//:foo`` if used in Pigweed and ``@some_project//:foo`` if used
in another project. Using ``Label("//:foo")`` forces the label to be evaluated
relative to the location of the ``.bzl`` file that the hard-coded string/label
is written in. This ensures the label always points to a consistent location. In
almost all cases, hard-coded labels should be wrapped in ``Label()``.

---------------------
C++ specific patterns
---------------------

Use ``strip_include_prefix`` rather than ``includes``
=====================================================
Nearly all ``cc_*`` libraries should introduce include paths via
``strip_include_prefix``. Typically, uses of ``includes`` should be considered
bugs.

Note that ``strip_include_prefix`` doesn't work with ``textual_hdrs`` (see
`bazelbuild/bazel#12424 <https://github.com/bazelbuild/bazel/issues/12424>`__),
so ``textual_hdrs`` may use ``includes`` in cases where ``textual_hdrs`` is
strictly necessary.

Reasoning: The unfortunately-named ``includes`` attribute of ``cc_*`` rules
is always intended to resolve to ``-isystem`` include directories which does two
things:

1. It affects include ordering in unintended ways.
2. It masks any warnings that originate in headers covered by the ``-isystem``
   include.

To get a include directory to resolve to ``-I``, ``strip_include_prefix`` must
be used. This has an added benefit of creating a virtual include directory that
provides stronger correctness guarantees. More information can be found at
https://pwbug.dev/378564135.

.. _docs-pw-style-bazel-implementation-deps:

Keep ``deps`` minimal and use ``implementation_deps`` for private ones
======================================================================
List in ``deps`` only the targets whose headers the library's own public
headers include. Dependencies that only source files or private headers use
belong in ``implementation_deps``.

Reasoning: ``deps`` are transitive, so every entry is exposed to every
dependent's include path and build graph. Keeping private dependencies in
``implementation_deps`` keeps the public interface honest, speeds up builds,
and prevents dependents from accidentally relying on headers that are not part
of the library's API.

Use ``pw_cc_binary`` and ``pw_cc_test``
=======================================
Use ``pw_cc_binary`` instead of ``cc_binary`` and ``pw_cc_test`` instead of
``cc_test``.

Reasoning: While these wrappers are very similar to their native counterparts,
Pigweed has some requirements that must be applied to every single one of these,
so copying those requirements across Pigweed is not scalable.

Downstream projects may choose whether or not to use these wrappers, there is
no strict requirement that the wrappers are used by any downstream user of
Pigweed.

Always load symbols from ``@rules_cc`` before using them
========================================================
When using any native ``cc_*`` rule, always load the symbol from
``rules_cc`` first.

Reasoning: Bazel's C/C++ rules are migrating from native Java implementations
to Starlark definitions. Loading from ``rules_cc`` may be absolutely necessary
in the future, so it's good practice to start doing it now.


------------------------
Python specific patterns
------------------------

Use ``pw_py_binary`` and ``pw_py_test``
=======================================
Use ``pw_py_binary`` instead of ``py_binary`` and ``pw_py_test`` instead of
``py_test``.

Reasoning: While these wrappers are very similar to their native counterparts,
Pigweed has some requirements that must be applied to every single one of these,
so copying those requirements across Pigweed is not scalable.

Downstream projects may choose whether or not to use these wrappers, there is
no strict requirement that the wrappers are used by any downstream user of
Pigweed.


Always load symbols from ``@rules_python`` before using them
============================================================
When using any native ``py_*`` rule, always load the symbol from
``rules_python`` first.

Reasoning: The native Python Bazel rules are subtly different from the rules
loaded from ``@rules_python``. Forgetting to load from ``@rules_python`` can
result in subtle but confusing breakages at best, and things silently working
at worst.
