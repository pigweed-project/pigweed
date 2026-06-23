.. _module-pw_fortifier:

============
pw_fortifier
============

This module provides tools and frameworks that both upstream Pigweed and
downstream projects can use to enhance the security posture of their project.

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

Command-line usage
==================
The tool can be run as a standalone script. It takes one or more code snippets
as arguments. Snippets can be file paths or file paths with a line range.

.. code-block:: console

   $ python3 pw_fortifier/py/pw_fortifier/find_core_owners.py \
       -r /path/to/repo \
       pw_status/public/pw_status/status.h:10-20

Arguments:

*  ``snippets``: One or more arguments in the form ``<file>[:start-end]``.
*  ``-r``, ``--root``: Root directory of the repository (defaults to ``.``).
*  ``-a``, ``--any``: Allow any owner, not just core team members.

Library usage
=============
You can also use ``CoreOwnerFinder`` as a Python library.

.. code-block:: py

   from pw_fortifier.find_core_owners import CoreOwnerFinder

   # Initialize with repository root
   finder = CoreOwnerFinder(root="/path/to/repo")

   # Add snippets to analyze
   finder.add("pw_status/public/pw_status/status.h", lines=(10, 20))
   finder.add("pw_status/status.cc")

   # Find the owner
   owner = finder.find()
   print(f"Primary core owner: {owner}")

   # Find any owner (not restricted to core owners)
   any_owner = finder.find(any_owner=True)
   print(f"Primary owner: {any_owner}")
