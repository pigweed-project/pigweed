.. _docs-os-stl:

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

See :ref:`stl-backends` for more information.

.. toctree::
   :maxdepth: 1
   :hidden:

   backends
