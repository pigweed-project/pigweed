.. _docs-style-python:

==================
Python style guide
==================
.. _Python: https://www.python.org/

Pigweed uses the standard `Python`_ style: `PEP8
<https://www.python.org/dev/peps/pep-0008/>`__. All Pigweed Python code should
pass ``pw format``, which invokes ``black`` with a couple options.

---------------
Python versions
---------------
Pigweed officially supports :ref:`a few Python versions
<docs-concepts-python-version>`. Upstream Pigweed code must support those Python
versions. The only exception is :ref:`module-pw_env_setup`, which must also
support Python 2 and 3.6.

.. _docs-style-python-extend-generated-import-paths:

-------------------------------------------------------
Extend the import path of packages with generated files
-------------------------------------------------------
Python packages that include generated files should extend their import path
using the following snippet at the beginning of their ``__init__.py``:

.. code-block:: python

   # This Python package contains generated Python modules that overlap with
   # this `__init__.py` file's import namespace, so this package's import path
   # must be extended for the generated modules to be discoverable.
   #
   # Note: This needs to be done in every nested `__init__.py` that will contain
   # overlapping generated files.
   __path__ = __import__('pkgutil').extend_path(__path__, __name__)

.. _docs-style-python-error-handling:

--------------
Error handling
--------------
* Keep ``try`` blocks narrow. Only the statements that can raise go inside;
  data preparation and logging go before or after the block.
* Raise on a violated precondition, with ``ValueError``, ``TypeError``, or an
  assertion as appropriate. Do not return ``None`` or another sentinel in its
  place.
* Pass ``check=True`` to ``subprocess.run``, or handle ``returncode``
  explicitly.

.. _docs-style-python-types:

-----------------------------
Type annotations and defaults
-----------------------------
* Annotate the parameters and return type of every public function and method.
* Never use a mutable default argument. Default to ``None`` and create the
  object in the function body.
* Use ``Path`` rather than ``str`` for file system paths, imported with
  ``from pathlib import Path``.

.. _docs-style-python-tests:

-----
Tests
-----
* Prefer real implementations over mocks: use temporary directories and real
  child processes instead of patching ``os``, ``pathlib``, or
  ``builtins.open``.
* Parse output files and assert on their structure, not on exact strings.

.. _docs-style-python-build:

-----------------
Build integration
-----------------
List every new module in the package's ``BUILD.bazel`` and ``BUILD.gn`` so
that presubmit builds, lints, type-checks, and tests it.
