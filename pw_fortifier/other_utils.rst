.. _module-pw_fortifier-other_utils:

===============
Other utilities
===============
``pw_fortifier`` includes some utilities that it uses as part of its scanners
that are also useful by themselves.

.. _module-pw_fortifier-other_utils-find_core_owners:

----------------
find_core_owners
----------------
``find_core_owners.py`` is a tool to identify the primary owner(s) of specific
files or line ranges by analyzing git history.

What is a "core owner"?
=======================
A "core owner" is defined as a user listed directly in the global ``OWNERS``
file in the repository root. Users included from other files (such as
``EXTENDED_OWNERS``) or defined in nested ``OWNERS`` files are not considered
core owners.

.. _module-pw_fortifier-other_utils-find_core_owners-cli:

Command-line usage
==================
The tool can be run as a standalone script or via Bazel. It takes one or more
code snippets as arguments. Snippets can be file paths or file paths with a line
range.

.. tab-set::

   .. tab-item:: Python
      :sync: python

      .. code-block:: console

         $ cd path/to/pigweed/pw_fortifier/py

         $ python3 -m pw_fortifier.find_core_owners \
             -r /path/to/repo \
             pw_status/public/pw_status/status.h:10-20

   .. tab-item:: Bazel
      :sync: bazel

      .. code-block:: console

         $ bazelisk run @pigweed//pw_fortifier/py:find_core_owners -- \
             -r /path/to/repo \
             pw_status/public/pw_status/status.h:10-20

Arguments:

*  ``snippets``: One or more arguments in the form ``<file>[:start-end]``.
*  ``-r``, ``--root``: Root directory of the repository (defaults to ``.``).
*  ``-a``, ``--any``: Allow any owner, not just core team members.

.. _module-pw_fortifier-other_utils-find_core_owners-lib:

Library usage
=============
You can also use ``CoreOwnerFinder`` as a Python library.

.. code-block:: py

   from pw_fortifier.find_core_owners import CoreOwnerFinder
   from pw_fortifier.git_utils import ReadOnlyGitWorkspace

   # Initialize with a read-only git workspace
   repo = ReadOnlyGitWorkspace(project_dir="/path/to/repo")
   finder = CoreOwnerFinder(repo=repo)

   # Add snippets to analyze
   finder.add("pw_status/public/pw_status/status.h", lines=(10, 20))
   finder.add("pw_status/status.cc")

   # Find the owner
   owner = finder.find()

   # Find any owner (not restricted to core owners)
   if owner is None:
       owner = finder.find(any_owner=True)

   print(f"Primary owner: {owner}")
