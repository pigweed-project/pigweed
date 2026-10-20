.. _docs-os-stl:
.. _stl-backends:

====================================
STL (Linux, MacOS, Windows, Fuchsia)
====================================
Pigweed has support for host compilation (macOS, Windows, and Linux) using the
C++ Standard Template Library (STL). This support also extends to other
platforms that provide STL synchronization support; indeed, these backends have
some non-public production uses on non-public OS platforms.

-----------------
Host workstations
-----------------
STL backends are automatically selected when compiling for a host system using
the default host toolchains.  No third-party packages or source checkouts are
required to compile these backends.

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
The following table lists all modules related to STL.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_stl`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Chrono <../../pw_chrono_stl/docs>
   Synchronization <../../pw_sync_stl/docs>
   Threads <../../pw_thread_stl/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
