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

--------------
freshness_scan
--------------
``freshness_scan.py`` is a tool to scan project dependencies and report their
freshness. It identifies out-of-date third-party packages by comparing the
currently used version with newer versions available upstream.

.. note::

   While ``freshness_scan.py`` is configured for upstream Pigweed
   out-of-the-box, the underlying framework is generic. Downstream
   projects can easily use it as a starting point by registering a
   different set of scanners tailored to their specific dependency
   types.

How it works
============
The tool utilizes a registry-based architecture defined in
``package_scanner.py``:

1. **Registry Initialization**: A ``PackageScannerRegistry`` is initialized
   with a root directory to scan.
2. **Scanner Registration**: Concrete implementations of ``PackageScanner`` are
   registered with the registry. Each scanner defines a filename pattern (e.g.,
   ``**/Cargo.toml`` for Cargo packages) it is interested in.
3. **Directory Walking**: The registry walks the directory tree starting from the
   configured root directory, excluding pruned directories (such as build output
   or environment directories).
4. **Pattern Matching**: For each file found, the registry matches its path
   against the patterns of all registered scanners.
5. **Scanning**: When a match is found, the registry passes the relative path
   of the file to the matching scanner's ``scan()`` method. The scanner parses
   the file to extract dependency details (current version, package name),
   queries upstream sources (e.g., crates.io) for release history, and
   determines the earliest version that is still considered "fresh".
6. **Reporting**: The registry aggregates ``FreshnessScanResult`` objects from
   all scanners, and ``freshness_scan.py`` formats and outputs these results as
   CSV to stdout.

Included Scanners
=================
The following scanners are included in ``pw_fortifier``:

*  ``CargoScanner``: Scans Rust Cargo dependencies declared in ``Cargo.toml``
   files. It extracts the package names and versions, resolves them against a
   corresponding ``Cargo.lock`` file to determine the exact versions in use,
   and queries the crates.io API to check for newer releases.
*  ``GoModScanner``: Scans Go module dependencies declared in ``go.mod``
   files. It queries the local ``go`` toolchain (using ``go list``) to resolve
   dependencies, their timestamps, and available newer versions.

Extending the Scanner
=====================
The framework is designed to be easily extendable to support new package
managers or dependency definition formats. To add support for a new package
type:

1. **Create a Scanner Class**: Inherit from ``PackageScanner`` and implement
   the abstract methods:

   * ``pattern()``: Returns a glob-like pattern matching the dependency
     files this scanner handles.
   * ``scan(pathname)``: Parses the file at the given relative path,
     queries upstream versions, and yields ``FreshnessScanResult``
     objects.

2. **Register the Scanner**: In ``freshness_scan.py``, import and register
   your new scanner class with the registry in the ``main()`` function:

   .. code-block:: py

      from pw_fortifier.my_new_scanner import MyNewScanner
      # ...
      registry.register(MyNewScanner())

Command-line usage
==================
The tool can be run as a standalone script. By default, it scans the current
working directory.

.. code-block:: console

   $ python3 pw_fortifier/py/pw_fortifier/freshness_scan.py \
       -r /path/to/project

Arguments:

*  ``-r``, ``--root``: Root directory of the project to scan (defaults to the
   current working directory).
*  ``-o``, ``--output``: Output CSV file (defaults to stdout).
