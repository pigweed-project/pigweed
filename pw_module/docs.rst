.. _module-pw_module:

=========
pw_module
=========
.. pigweed-module::
   :name: pw_module

The ``pw_module`` module contains tools for managing Pigweed modules.
For information on the structure of a Pigweed module, refer to
:ref:`docs-module-guides`.

--------
Commands
--------

.. _module-pw_module-module-check:

``pw module check``
===================
The ``pw module check`` command exists to ensure that your module conforms to
the Pigweed module norms.

For example, at time of writing ``pw module check pw_module`` is not passing
its own lint:

.. code-block:: none

   $ ./pw module check pw_module

    ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
     ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
     ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
     ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
     ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀

   20191205 17:05:19 INF Checking module: pw_module
   20191205 17:05:19 ERR PWCK004: Missing ReST documentation; need at least e.g. "docs.rst"
   20191205 17:05:19 ERR FAIL: Found errors when checking module pw_module


.. _module-pw_module-module-create:

``pw module create``
====================
The ``pw module create`` command generates all the required boilerplate for a
new Pigweed project module, including source files, tests, documentation, and
build files for GN, Bazel, and CMake.

Usage
-----
.. code-block:: none

   pw module create [OPTIONS] MODULE_NAME

**Options**

* ``--build-systems``: A comma-separated list of build systems the module
  should support (e.g., ``gn,bazel``). Options are ``gn``, ``bazel``, and
  ``cmake``. Defaults can be configured as described in
  :ref:`module-pw_module-configuration`.
* ``--languages``: A comma-separated list of languages the module will use.
  Currently only ``cc`` is supported.
* ``--owners``: (Upstream only) A comma-separated list of emails of the people
  who will own and maintain the new module. This list must contain at least two
  entries, and at least one user must be a top-level OWNER.

**Naming Conventions**

Module names must conform to a specific format:

* They must start with a prefix of at least two letters (e.g., ``pw``).
* The prefix must be followed by an underscore and the rest of the name.
* The rest of the name consists of groups of alphanumeric characters separated
  by single underscores.
* Upstream Pigweed modules must use the ``pw`` prefix.
* In C++ code, the prefix and the rest of the name define a nested namespace
  for the module. For example, ``pw_module_name`` results in the namespace
  ``pw::module_name``.

**Downstream Support**

When run in a downstream project, ``pw module create`` creates the module
directory and files but skips the integration steps that assume a Pigweed
upstream repository structure (such as updating ``PIGWEED_MODULES`` and the
Sphinx documentation index).

.. _module-pw_module-configuration:

-------------
Configuration
-------------
You can configure the default build systems and languages for ``pw module``
commands in your ``pigweed.json`` file:

If omitted, these default to all supported build systems and languages.

.. code-block:: json

   {
     "pw": {
       "pw_module": {
         "default_build_systems": ["gn", "bazel"],
         "default_languages": ["cc"]
       }
     }
   }
