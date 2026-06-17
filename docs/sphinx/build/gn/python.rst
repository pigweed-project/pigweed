.. _docs-python-build:

=========================
Pigweed's GN Python Build
=========================

.. seealso::
   - :bdg-ref-primary-line:`module-pw_build-python` for detailed template usage.
   - :bdg-ref-primary-line:`module-pw_build` for other GN templates available
     within Pigweed.
   - :bdg-ref-primary-line:`docs-build-systems-overview` for a high level guide and
     background information on Pigweed's build system as a whole.

Pigweed uses a custom GN-based build system to manage its Python code. The
Pigweed Python build supports packaging, installation and distribution of
interdependent local Python packages. It also provides for fast, incremental
static analysis and test running suitable for live use during development (e.g.
with :ref:`module-pw_watch`) or in continuous integration.

Pigweed's Python code is exclusively managed by GN, but the GN-based build may
be used alongside CMake, Bazel, or any other build system. Pigweed's environment
setup uses GN to set up the initial Python environment, regardless of the final
build system. As needed, non-GN projects can declare just their Python packages
in GN.

How it Works
============
In addition to compiler commands a Pigweed GN build will execute Python scripts
for various reasons including running tests, linting code, generating protos and
more. All these scripts are run as part of a
:ref:`module-pw_build-pw_python_action` GN template which will ultimately run
``python``. Running Python on its own by default will make any Python packages
installed on the users system available for importing. This can lead to flaky
builds when different packages are installed on each developer workstation. To
get around this the Python community uses `virtual environments
<https://docs.python.org/3/library/venv.html>`_ (venvs) that expose a specific
set of Python packages separate from the host system.

When a Pigweed GN build starts a single venv is created for use by all
:ref:`pw_python_actions <module-pw_build-pw_python_action>` throughout the build
graph. Once created, all required third-party Python packages needed for the
project are installed. At that point no further modifications are made to
the venv. Of course if a new third-party package dependency is added it will be
installed too. Beyond that all venvs remain static. More venvs can be created
with the :ref:`module-pw_build-pw_python_venv` template if desired, but only one
is used by default.

.. card::

   **Every pw_python_action is run inside a venv**
   ^^^
   .. mermaid::

      flowchart LR
        out[GN Build Dir<br/>fa:fa-folder out]

        out -->|ninja -C out| createvenvs

        createvenvs(Create venvs)
        createvenvs --> pyactions1
        createvenvs --> pyactions2

        subgraph pyactions1[Python venv 1]
          direction TB
          venv1(fa:fa-folder out/python-venv &nbsp)
          a1["pw_python_action('one')"]
          a2["pw_python_action('two')"]
          venv1 --> a1
          venv1 --> a2
        end

        subgraph pyactions2[Python venv 2]
          direction TB
          venv2(fa:fa-folder out/another-venv &nbsp)
          a3["pw_python_action('three')"]
          a4["pw_python_action('four')"]
          venv2 --> a3
          venv2 --> a4
        end

.. note::

   Pigweed uses the
   :cs:`pigweed_build_venv <main:pw_env_setup/BUILD.gn?q=pigweed_build_venv>`
   target if a project does not specify its own build venv. See
   :bdg-ref-primary-line:`docs-python-build-python-gn-venv` on how to define
   your own default venv.

Having a static venv containing only third-party dependencies makes it
possible for Python scripts to run. If the venv only contains third-party
dependencies you may be wondering how you can import your own in-tree Python
packages. Python code run in the build may still import any in-tree Python
packages created with :ref:`module-pw_build-pw_python_package`
templates. However this only works if a correct ``python_deps`` arg is
provided. Having that Python dependency defined in GN allows the
:ref:`module-pw_build-pw_python_action`
to set `PYTHONPATH
<https://docs.python.org/3/using/cmdline.html#envvar-PYTHONPATH>`_ so that given
package can be imported. This has the benefit of the build failing if a
dependency for any Python action or package is missing.

.. admonition:: Benefits of Python ``venvs`` in GN
   :class: important

   - Using venvs to execute Python in GN provides reproducible builds with fixed
     third-party dependencies.
   - Using ``PYTHONPATH`` coupled with ``python_deps`` to import in-tree Python
     packages enforces dependency correctness.


Managing Python Requirements
============================

.. _docs-python-build-python-gn-venv:

Build Time Python Virtualenv
----------------------------
Pigweed's GN Python build infrastructure relies on `Python virtual environments
<https://docs.python.org/3/library/venv.html>`_ for executing Python code. This
provides a controlled isolated environment with a defined set of third party
Python constraints where all Python tests, linting and
:ref:`module-pw_build-pw_python_action` targets are executed.

There must be at least one venv for Python defined in GN. There can be multiple
venvs but one must be the designated default.

The default build venv is specified via a GN arg and is best set in the root
``.gn`` or ``BUILD.gn`` file. For example:

.. code-block::

   pw_build_PYTHON_BUILD_VENV = "//:project_build_venv"

.. tip::
   Additional :ref:`module-pw_build-pw_python_venv` targets can be created as
   needed. The :ref:`module-pw_build-pw_python_action` template can take an
   optional ``venv`` argument to specify which Python venv it should run
   within. If not specified the target referred in the
   ``pw_build_PYTHON_BUILD_VENV`` is used.

.. _docs-python-build-python-gn-requirements-files:

Third-party Python Requirements and Constraints
-----------------------------------------------
Your project may have third party Python dependencies you wish to install into
the bootstrapped environment and in the GN build venv. There are two main ways
to add Python package dependencies:

**Adding Requirements Files**

1. Add a ``install_requires`` entry to a ``setup.cfg`` file defined in a
   :ref:`module-pw_build-pw_python_package` template. This is the best option
   if your in-tree Python package requires an external Python package.

2. Create a standard Python ``requirements.txt`` file in your project and add it
   to the ``pw_build_PIP_REQUIREMENTS`` GN arg list.

   Requirements files support a wide range of install locations including
   packages from pypi.org, the local file system and git repos. See `pip's
   Requirements File documentation
   <https://pip.pypa.io/en/stable/user_guide/#requirements-files>`_ for more
   info.

   The GN arg can be set in your project's root ``.gn`` or ``BUILD.gn`` file.

   .. code-block::

      pw_build_PIP_REQUIREMENTS = [
        # Project specific requirements
        "//tools/requirements.txt",
      ]

   See the :ref:`docs-python-build-python-gn-structure` section below for a full
   code listing.

**Adding Constraints Files**

Every project should ideally inherit Pigweed's third party Python package
version. This is accomplished via `Python constraints files
<https://pip.pypa.io/en/stable/user_guide/#constraints-files>`_. Constraints
control which versions of packages get installed by ``pip`` if that package is
installed. To inherit Pigweed's Python constraints include ``constraint.list``
from the ``pw_env_setup`` module from in your top level ``.gn`` file. Additonal
project specific constraints can be appended to this list.

.. code-block::

   pw_build_PIP_CONSTRAINTS = [
     "$dir_pw_env_setup/py/pw_env_setup/virtualenv_setup/constraint.list",
     "//tools/constraints.txt",
   ]

In-tree ``pw_python_package`` Requirements
------------------------------------------
A given venv inherits a project's requirements and constraint files by default
via the ``pw_build_PIP_CONSTRAINTS`` and ``pw_build_PIP_REQUIREMENTS`` GN args
as described above. This can be overridden if needed.

``generated_requirements.txt``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
To ensure the requirements of in-tree :ref:`module-pw_build-pw_python_package`
targets are installed :ref:`module-pw_build-pw_python_venv` introduces the
``source_packages`` argument. This is a list of in-tree ``pw_python_package``
GN targets expected to be used within the venv. When the venv is created each
``pw_python_package``'s ``setup.cfg`` file is read to pull the
``install_requires`` section for all third party dependencies. The full list of
all in-tree packages and any in-tree transitive dependencies is then written to
the out directory in a single ``generated_requirements.txt``.

Take the ``//pw_build/py/gn_tests:downstream_tools_build_venv`` example below,
its ``source package`` is a single ``pw_python_distribution`` package which
bundles the ``pw_env_setup`` and ``pw_console`` ``pw_python_package``s. Those
two packages each depend on a few other ``pw_python_package`` targets. The
output ``generated_requirements.txt`` below merges all these package deps and
adds ``-c`` lines for constraint files.

.. seealso::
   The pip documentation on the `Requirements File Format
   <https://pip.pypa.io/en/stable/reference/requirements-file-format/#requirements-file-format>`_

.. literalinclude:: ../../pw_build/py/gn_tests/BUILD.gn
   :start-after: [downstream-project-venv]
   :end-before: [downstream-project-venv]

.. code-block::
   :caption: :octicon:`file;1em` out/python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/generated_requirements.txt
   :name: generated_requirements

   # Auto-generated requirements.txt from the following packages:
   #
   # //pw_arduino_build/py:py
   # //pw_build/py/gn_tests:downstream_project_tools
   # //pw_build/py:py
   # //pw_cli/py:py
   # //pw_console/py:py
   # //pw_env_setup/py:py
   # //pw_log_tokenized/py:py
   # //pw_package/py:py
   # //pw_presubmit/py:py
   # //pw_stm32cube_build/py:py

   # Constraint files:
   -c ../../../../../../../pw_env_setup/py/pw_env_setup/virtualenv_setup/constraint.list

   black>=23.1.0
   build>=0.8.0
   coloredlogs
   coverage
   ipython
   jinja2
   mypy>=0.971
   parameterized
   pip-tools>=6.12.3
   prompt-toolkit>=3.0.26
   psutil
   ptpython>=3.0.20
   pygments
   pylint>=2.9.3
   pyperclip
   pyserial>=3.5,<4.0
   pyyaml
   setuptools
   six
   toml
   types-pygments
   types-pyserial>=3.5,<4.0
   types-pyyaml
   types-setuptools
   types-six
   websockets
   wheel
   yapf>=0.31.0

``compiled_requirements.txt``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
The above ``generated_requirements.txt`` file is then fed into the
``pip-compile`` command from `the pip-tools package
<https://pypi.org/project/pip-tools>`_ to fully expand and pin each package with
hashes. The resulting ``compiled_requirements.txt`` can then be used as the
single Python requirements file for replicating this ``pw_python_venv``
elsewhere. Each ``pw_python_venv`` will get this single file containing the
exact versions of each required Python package.

.. tip::
   The ``compiled_requirements.txt`` generated by a ``pw_python_venv`` is used
   by the :ref:`module-pw_build-pw_python_zip_with_setup` template when
   producing a self contained zip of in-tree and third party Python packages.

Below is a snippet of the ``compiled_requirements.txt`` for this
:ref:`module-pw_build-pw_python_venv` target:
``//pw_build/py/gn_tests:downstream_tools_build_venv``

.. code-block::
   :caption: :octicon:`file;1em` out/python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/compiled_requirements.txt
   :name: compiled_requirements

   #
   # This file is autogenerated by pip-compile with Python 3.11
   # by the following command:
   #
   #    pip-compile --allow-unsafe --generate-hashes
   #      --output-file=python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/compiled_requirements.txt
   #      --resolver=backtracking
   #      python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/generated_requirements.txt
   #
   appdirs==1.4.4 \
       --hash=sha256:7d5d0167b2b1ba821647616af46a749d1c653740dd0d2415100fe26e27afdf41 \
       --hash=sha256:a841dacd6b99318a741b166adb07e19ee71a274450e68237b4650ca1055ab128
       # via
       #   -c python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/../../../../../../../pw_env_setup/py/pw_env_setup/virtualenv_setup/constraint.list
       #   ptpython
   astroid==2.14.2 \
       --hash=sha256:0e0e3709d64fbffd3037e4ff403580550f14471fd3eaae9fa11cc9a5c7901153 \
       --hash=sha256:a3cf9f02c53dd259144a7e8f3ccd75d67c9a8c716ef183e0c1f291bc5d7bb3cf
       # via
       #   -c python/gen/pw_build/py/gn_tests/downstream_tools_build_venv/../../../../../../../pw_env_setup/py/pw_env_setup/virtualenv_setup/constraint.list
       #   pylint
   ...

The presence of hashes in the above example can be controlled via the
``pip_generate_hashes`` arg to the :ref:`module-pw_build-pw_python_venv`
template.

Caching Python Packages for Offline Installation
------------------------------------------------

.. _docs-python-build-downloading-packages:

Downloading Packages
^^^^^^^^^^^^^^^^^^^^
The :ref:`module-pw_build-pw_python_venv` target adds an optional sub target
that will download all Python packages from remote servers into a local
directory. The remote server is typically `pypi.org <https://pypi.org/>`_.

Taking the ``//pw_build/py/gn_tests:downstream_tools_build_venv`` target as an
example again let's build a local cache. To run the download target append
``.vendor_wheels`` to the end of the ``pw_python_venv`` target name. In this
example it would be
``//pw_build/py/gn_tests:downstream_tools_build_venv.vendor_wheels``

To build that one gn target with ninja, pass the output name from gn as a target
name for ninja:

.. code-block:: bash

   gn gen out
   ninja -C out \
     $(gn ls out --as=output \
       '//pw_build/py/gn_tests:downstream_tools_build_venv.vendor_wheels')

This creates a ``wheels`` folder with all downloaded packages and a
``pip_download_log.txt`` with verbose logs from running ``pip download``.

.. code-block::
   :caption: :octicon:`file-directory;1em` Vendor wheels output directory
   :name: vendor-wheel-output

   out/python/gen/pw_build/py/gn_tests/downstream_tools_build_venv.vendor_wheels/
   ├── pip_download_log.txt
   └── wheels
       ├── appdirs-1.4.4-py2.py3-none-any.whl
       ├── astroid-2.14.2-py3-none-any.whl
       ├── backcall-0.2.0-py2.py3-none-any.whl
       ├── black-23.1.0-cp311-cp311-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
       ├ ...
       ├── websockets-10.4-cp311-cp311-manylinux_2_5_x86_64.manylinux1_x86_64.manylinux_2_17_x86_64.manylinux2014_x86_64.whl
       ├── wheel-0.40.0-py3-none-any.whl
       ├── wrapt-1.14.1.tar.gz
       └── yapf-0.31.0-py2.py3-none-any.whl

Note the above output has both Python wheel ``.whl`` and source distribution
``.tar.gz`` files. The ``.whl`` may contain Python packages with precompiled C
extensions. This is denoted by this part of the filename:
``cp311-cp311-manylinux_2_17_x86_64.whl``. These binary packages are selected by
the ``pip download`` command based on the host machine python version, OS, and
CPU architecture.

.. warning::
   If you need to cache Python packages for multiple platforms the
   ``.vendor_wheels`` target will need to be run for each combination of Python
   version, host operating system and architecture. For example, look at `the
   files available for numpy <https://pypi.org/project/cffi/#files>`_. Some
   combinations are:

   - cp311, manylinux_2_17_x86_64
   - cp311, manylinux2014_x86_64
   - cp311, macosx_11_0_arm64
   - cp311, macosx_10_9_x86_64
   - cp311, win_amd64
   - cp311, win32

   Plus all of the above duplicated for Python 3.10 and 3.9 (``cp310`` and
   ``cp39``).

   The output of multiple ``.vendor_wheels`` runs on different host systems can
   all be merged into the same output directory.

``.vendor_wheels`` can attempt to download binary packages for multiple
platforms all at once by setting a GN arg:

.. code-block::

   pw_build_PYTHON_PIP_DOWNLOAD_ALL_PLATFORMS = true

This will invoke `pip download
<https://pip.pypa.io/en/stable/cli/pip_download/>`_ for each combination of
platform, architecture and Python version. This can take a significant amount of
time to complete. The current set of combinations is shown below:

.. literalinclude:: ../../pw_build/py/pw_build/generate_python_wheel_cache.py
   :start-after: [wheel-platform-args]
   :end-before: [wheel-platform-args]

.. warning::
   The set of Python packages that will be downloaded is determined by the
   ``compiled_requirements.txt`` file. This file can only be generated for the
   current host OS and Python version. `pip-tools
   <https://pypi.org/project/pip-tools>`_ does not expand requirements for
   `platform specific dependencies
   <https://setuptools.pypa.io/en/latest/userguide/dependency_management.html#platform-specific-dependencies>`_. For
   example ipython defines these two requirements:

   .. code-block::

      appnope; sys_platform == "darwin"
      colorama; sys_platform == "win32"

   If pip-tools is run on Linux then the above packages will not appear in
   ``compiled_requirements.txt`` and not downloaded by the ``.vendor_wheels``
   target.

.. _docs-python-build-installing-offline:

Installing Offline
^^^^^^^^^^^^^^^^^^
Once the vendor wheel output is saved to a directory in your project you can use
this as the default pip install location in two different ways.

GN Args
.......
Setting these args in the ``//.gn`` file will add the relevant pip command line
args to perform offline installations.

.. code-block::

   # Adds --no-index forcing pip to not reach out to the internet (pypi.org) to
   # download packages. Using this option requires setting
   # pw_build_PYTHON_PIP_INSTALL_FIND_LINKS as well.
   pw_build_PYTHON_PIP_INSTALL_OFFLINE = true

   # List of paths to folders containing Python wheels (*.whl) or source tar
   # files (*.tar.gz). Pip will check each of these directories when looking for
   # potential install candidates.
   pw_build_PYTHON_PIP_INSTALL_FIND_LINKS = [
     "//environment/cipd/packages/python_packages/universal",
     "//environment/cipd/packages/python_packages/linux/cp311",
   ]

   # Optional: Adds '--no-cache-dir' forcing pip to ignore any previously cached
   # Python packages. On most systems this is located in ~/.cache/pip/
   pw_build_PYTHON_PIP_INSTALL_DISABLE_CACHE = false

Using a ``.pip.conf`` File
..........................
1. Create a ``//pip.conf`` file containing:

   .. code-block::
      :caption: :octicon:`file;1em` //pip.conf
      :name: pip-conf-file

      [global]
      # Disable searching pypi.org for packages
      no-index = True
      # Find packages in these directories:
      find-links =
          file://third_party/python_packages/universal
          file://third_party/python_packages/linux/cp311

   This tells pip to not search pypi.org for packages and only look in
   ``third_party/python_packages/universal`` and
   ``third_party/python_packages/linux/cp311``. These paths can be absolute or
   are relative to the ``pip.conf`` file.

2. In the project ``bootstrap.sh`` set ``PIP_CONFIG_FILE`` to the location of
   this file.

   .. code-block:: bash

      export PIP_CONFIG_FILE="${PW_PROJECT_ROOT}/pip.conf"

   With that environment var set all invocations of pip will apply the config file
   settings above.

.. seealso::
   The ``pip`` `documentation on Configuration
   <https://pip.pypa.io/en/stable/topics/configuration/>`_.

.. _docs-python-build-python-gn-structure:

GN File Structure for Python Code
=================================
Here is a full example of what is required to build Python packages using
Pigweed's GN build system. A brief file hierarchy is shown here with file
content following. See also :ref:`docs-python-build-structure` below for details
on the structure of Python packages.

.. code-block::
   :caption: :octicon:`file-directory;1em` Top level GN file hierarchy
   :name: gn-python-file-tree

   project_root/
   ├── .gn
   ├── BUILDCONFIG.gn
   ├── build_overrides/
   │   └── pigweed.gni
   ├── BUILD.gn
   │
   ├── python_package1/
   │   ├── BUILD.gn
   │   ├── setup.cfg
   │   ├── pyproject.toml
   │   │
   │   ├── package_name/
   │   │   ├── module_a.py
   │   │   ├── module_b.py
   │   │   ├── py.typed
   │   │   └── nested_package/
   │   │       ├── py.typed
   │   │       └── module_c.py
   │   │
   │   ├── module_a_test.py
   │   └── module_c_test.py
   │
   ├── third_party/
   │   └── pigweed/
   │
   └── ...

- :octicon:`file-directory;1em` project_root/

  - :octicon:`file;1em` .gn

    .. code-block::

       buildconfig = "//BUILDCONFIG.gn"
       import("//build_overrides/pigweed.gni")

       default_args = {
         pw_build_PIP_CONSTRAINTS = [
           # Inherit Pigweed Python constraints
           "$dir_pw_env_setup/py/pw_env_setup/virtualenv_setup/constraint.list",

           # Project specific constraints file
           "//tools/constraint.txt",
         ]

         pw_build_PIP_REQUIREMENTS = [
           # Project specific requirements
           "//tools/requirements.txt",
         ]

         # Default gn build virtualenv target.
         pw_build_PYTHON_BUILD_VENV = "//:project_build_venv"
       }

    .. tip::

       There are some additional gn args to control how pip installations are
       performed during the build.

       .. literalinclude:: ../../pw_build/python_gn_args.gni
          :start-after: [default-pip-gn-args]
          :end-before: [default-pip-gn-args]

  - :octicon:`file;1em` BUILDCONFIG.gn

    .. code-block::

       _pigweed_directory = {
         import("//build_overrides/pigweed.gni")
       }

       set_default_toolchain("${_pigweed_directory.dir_pw_toolchain}/default")

  - :octicon:`file-directory;1em` build_overrides / :octicon:`file;1em` pigweed.gni

    .. code-block::

       declare_args() {
         # Location of the Pigweed repository.
         dir_pigweed = "//third_party/pigweed/"
       }

       # Upstream Pigweed modules.
       import("$dir_pigweed/modules.gni")

  - :octicon:`file;1em` BUILD.gn

    .. code-block::

       import("//build_overrides/pigweed.gni")

       import("$dir_pw_build/python.gni")
       import("$dir_pw_build/python_dist.gni")
       import("$dir_pw_build/python_venv.gni")
       import("$dir_pw_unit_test/test.gni")

       # Lists all the targets build by default with e.g. `ninja -C out`.
       group("default") {
         deps = [
           ":python.lint",
           ":python.tests",
         ]
       }

       # This group is built during bootstrap to setup the interactive Python
       # environment.
       pw_python_group("python") {
         python_deps = [
           # Generate and pip install _all_python_packages
           ":pip_install_project_tools",
         ]
       }

       # In-tree Python packages
       _project_python_packages = [
         "//python_package1",
       ]

       # Pigweed Python packages to include
       _pigweed_python_packages = [
         "$dir_pw_env_setup:core_pigweed_python_packages",
         "$dir_pigweed/targets/lm3s6965evb_qemu/py",
         "$dir_pigweed/targets/stm32f429i_disc1/py",
       ]

       _all_python_packages =
           _project_python_packages + _pigweed_python_packages

       # The default venv for Python actions in GN
       # Set this gn arg in a declare_args block in this file 'BUILD.gn' or in '.gn' to
       # use this venv.
       #
       #   pw_build_PYTHON_BUILD_VENV = "//:project_build_venv"
       #
       pw_python_venv("project_build_venv") {
         path = "$root_build_dir/python-venv"
         constraints = pw_build_PIP_CONSTRAINTS
         requirements = pw_build_PIP_REQUIREMENTS

         # Ensure all third party Python dependencies are installed into this venv.
         # This works by checking the setup.cfg files for all packages listed here and
         # installing the packages listed in the [options].install_requires field.
         source_packages = _all_python_packages
       }

       # This template collects all python packages and their dependencies into a
       # single super Python package for installation into the bootstrapped virtual
       # environment.
       pw_python_distribution("generate_project_python_distribution") {
         packages = _all_python_packages
         generate_setup_cfg = {
           name = "project-tools"
           version = "0.0.1"
           append_date_to_version = true
           include_default_pyproject_file = true
         }
       }

       # Install the project-tools super Python package into the bootstrapped
       # Python venv.
       pw_python_pip_install("pip_install_project_tools") {
         packages = [ ":generate_project_python_distribution" ]
       }

.. _docs-python-build-structure:

Pigweed Module Structure for Python Code
========================================
Pigweed Python code is structured into standard Python packages. This makes it
simple to package and distribute Pigweed Python packages with common Python
tools.

Like all Pigweed source code, Python packages are organized into Pigweed
modules. A module's Python package is nested under a ``py/`` directory (see
:ref:`Pigweed Module Stucture <docs-module-structure>`).

.. code-block::
   :caption: :octicon:`file-directory;1em` Example layout of a Pigweed Python package.
   :name: python-file-tree

   module_name/
   ├── py/
   │   ├── BUILD.gn
   │   ├── setup.cfg
   │   ├── pyproject.toml
   │   ├── package_name/
   │   │   ├── module_a.py
   │   │   ├── module_b.py
   │   │   ├── py.typed
   │   │   └── nested_package/
   │   │       ├── py.typed
   │   │       └── module_c.py
   │   ├── module_a_test.py
   │   └── module_c_test.py
   └── ...

The ``BUILD.gn`` declares this package in GN. For upstream Pigweed, a presubmit
check in ensures that all Python files are listed in a ``BUILD.gn``.

Pigweed prefers to define Python packages using ``setup.cfg`` files. In the
above file tree the ``pyproject.toml`` file is only a stub with the following
content:

.. code-block::
   :caption: :octicon:`file;1em` pyproject.toml
   :name: pyproject-toml-stub

   [build-system]
   requires = ['setuptools', 'wheel']
   build-backend = 'setuptools.build_meta'

Each ``pyproject.toml`` file is required to specify which build system should be
used for the given Python package. In Pigweed's case it always specifies using
setuptools.

.. seealso::

   - ``setup.cfg`` examples at `Configuring setup() using setup.cfg files`_
   - ``pyproject.toml`` background at `Build System Support - How to use it?`_

.. _module-pw_build-python-target:

pw_python_package targets
-------------------------
The key abstraction in the Python build is the ``pw_python_package``.
A ``pw_python_package`` represents a Python package as a GN target. It is
implemented with a GN template. The ``pw_python_package`` template is documented
in :ref:`module-pw_build-python`.

The key attributes of a ``pw_python_package`` are

- a ``setup.cfg`` and ``pyproject.toml`` file,
- source files,
- test files,
- dependencies on other ``pw_python_package`` targets.

A ``pw_python_package`` target is composed of several GN subtargets. Each
subtarget represents different functionality in the Python build.

- ``<name>`` - Represents the Python files in the build, but does not take any
  actions. All subtargets depend on this target.
- ``<name>.tests`` - Runs all tests for this package.

  - ``<name>.tests.<test_file>`` - Runs the specified test.

- ``<name>.lint`` - Runs static analysis tools on the Python code. This is a
  group of three subtargets:

  - ``<name>.lint.mypy`` - Runs Mypy on all Python files, if enabled.
  - ``<name>.lint.pylint`` - Runs Pylint on all Python files, if enabled.
  - ``<name>.lint.ruff`` - Runs ruff on all Python files, if enabled.

- ``<name>.install`` - Installs the package in a Python virtual environment.
- ``<name>.wheel`` - Builds a Python wheel for this package.

To avoid unnecessary duplication, all Python actions are executed in the default
toolchain, even if they are referred to from other toolchains.

Testing
^^^^^^^
Tests for a Python package are listed in its ``pw_python_package`` target.
Adding a new test is simple: write the test file and list it in its accompanying
Python package. The build will run it when the test, the package, or one of its
dependencies is updated.

Static analysis
^^^^^^^^^^^^^^^
``pw_python_package`` targets are preconfigured to run Pylint, Mypy and Ruff on
their source and test files. Users may specify which ``pylintrc``, ``mypy_ini``
and ``ruff_toml`` files to use on a per-package basis. The configuration files
may also be provided in the directory structure; the tools will locate them
using their standard means. Like tests, static analysis is only run when files
or their dependencies change.

Packages may opt out of static analysis as necessary by setting
``static_analysis`` on the ``pw_python_package`` target.

The default set of analysis tools to run can be set globally via a GN arg
``pw_build_PYTHON_STATIC_ANALYSIS_TOOLS``. By default this is set to include the
below tools:

.. literalinclude:: ../../pw_build/python.gni
   :start-after: [python-static-analysis-tools]
   :end-before: [python-static-analysis-tools]

In addition to user specified ``mypy_ini`` files some arguments are always
passed to ``mypy`` by default. They can be seen in this excerpt of
``//pw_build/python.gni`` below:

.. literalinclude:: ../../pw_build/python.gni
   :start-after: [default-mypy-args]
   :end-before: [default-mypy-args]

Building Python wheels
^^^^^^^^^^^^^^^^^^^^^^
`Wheels <https://wheel.readthedocs.io/en/stable/>`_ are the standard format for
distributing Python packages. The Pigweed Python build supports creating wheels
for individual packages and groups of packages. Building the ``.wheel``
subtarget creates a ``.whl`` file for the package using the PyPA's `build
<https://pypa-build.readthedocs.io/en/stable/>`_ tool.

The ``.wheel`` subtarget of any ``pw_python_package`` or
:ref:`module-pw_build-pw_python_distribution` records the location of the
generated wheel with `GN metadata
<https://gn.googlesource.com/gn/+/HEAD/docs/reference.md#var_metadata>`_.
Wheels for a Python package and its transitive dependencies can be collected
from the ``pw_python_package_wheels`` key. See
:ref:`module-pw_build-python-dist`.

Protocol buffers
^^^^^^^^^^^^^^^^
The Pigweed GN build supports protocol buffers with the ``pw_proto_library``
target (see :ref:`module-pw_protobuf_compiler`). Python protobuf modules are
generated as standalone Python packages by default. Protocol buffers may also be
nested within existing Python packages. In this case, the Python package in the
source tree is incomplete; the final Python package, including protobufs, is
generated in the output directory.

Generating setup.cfg
^^^^^^^^^^^^^^^^^^^^
The ``pw_python_package`` target in the ``BUILD.gn`` duplicates much of the
information in the ``setup.cfg`` file. In many cases, it would be possible to
generate a ``setup.cfg`` file rather than including it in the source
tree. However, removing the ``setup.cfg`` would preclude using a direct,
editable installation from the source tree.

Pigweed packages containing protobufs are generated in full or in part. These
packages may use generated setup files, since they are always packaged or
installed from the build output directory.


Rationale
=========

Background
----------
Developing software involves much more than writing source code. Software needs
to be compiled, executed, tested, analyzed, packaged, and deployed. As projects
grow beyond a few files, these tasks become impractical to manage manually.
Build systems automate these auxiliary tasks of software development, making it
possible to build larger, more complex systems quickly and robustly.

Python is an interpreted language, but it shares most build automation concerns
with other languages. Pigweed uses Python extensively and must address these
needs for itself and its users.

Existing solutions
------------------
The Python programming langauge does not have an official build automation
system. However, there are numerous Python-focused build automation tools with
varying degrees of adoption. See the `Python Wiki
<https://wiki.python.org/moin/ConfigurationAndBuildTools>`_ for examples.

A few Python tools have become defacto standards, including `setuptools
<https://pypi.org/project/setuptools/>`_, `wheel
<https://pypi.org/project/wheel/>`_, and `pip <https://pypi.org/project/pip/>`_.
These essential tools address key aspects of Python packaging and distribution,
but are not intended for general build automation. Tools like `PyBuilder
<https://pybuilder.io/>`_ and `tox <https://tox.readthedocs.io/en/latest/>`_
provide more general build automation for Python.

The `Bazel <http://bazel.build/>`_ build system has first class support for
Python and other languages used by Pigweed, including protocol buffers.

Challenges
----------
Pigweed's use of Python is different from many other projects. Pigweed is a
multi-language, modular project. It serves both as a library or middleware and
as a development environment.

This section describes Python build automation challenges encountered by
Pigweed.

Dependencies
^^^^^^^^^^^^
Pigweed is organized into distinct modules. In Python, each module is a separate
package, potentially with dependencies on other local or `PyPI
<https://pypi.org/>`_ packages.

The basic Python packaging tools lack dependency tracking for local packages.
For example, a package's ``setup.cfg`` lists all of its dependencies, but
``pip`` is not aware of local packages until they are installed. Packages must
be installed with their dependencies taken into account, in topological sorted
order.

To work around this, one could set up a private `PyPI server
<https://pypi.org/project/pypiserver/>`_ instance, but this is too cumbersome
for daily development and incompatible with editable package installation.

Testing
^^^^^^^
Tests are crucial to having a healthy, maintainable codebase. While they take
some initial work to write, the time investment pays for itself many times over
by contributing to the long-term resilience of a codebase. Despite their
benefit, developers don't always take the time to write tests. Any barriers to
writing and running tests result in fewer tests and consequently more fragile,
bug-prone codebases.

There are lots of great Python libraries for testing, such as
`unittest <https://docs.python.org/3/library/unittest.html>`_ and
`pytest <https://docs.pytest.org/en/stable/>`_. These tools make it easy to
write and execute individual Python tests, but are not well suited for managing
suites of interdependent tests in a large project. Writing a test with these
utilities does not automatically run them or keep running them as the codebase
changes.

Static analysis
^^^^^^^^^^^^^^^

.. seealso::

   :bdg-ref-primary-line:`docs-automated-analysis` for info on other static
   analysis tools used in Pigweed.

Various static analysis tools exist for Python. Two widely used, powerful tools
are `Pylint <https://www.pylint.org/>`_ and `Mypy <http://mypy-lang.org/>`_.
Using these tools improves code quality, as they catch bugs, encourage good
design practices, and enforce a consistent coding style. As with testing,
barriers to running static analysis tools cause many developers to skip them.
Some developers may not even be aware of these tools.

Deploying static analysis tools to a codebase like Pigweed has some challenges.
Mypy and Pylint are simple to run, but they are extremely slow. Ideally, these
tools would be run constantly during development, but only on files that change.
These tools do not have built-in support for incremental runs or dependency
tracking.

Another challenge is configuration. Mypy and Pylint support using configuration
files to select which checks to run and how to apply them. Both tools only
support using a single configuration file for an entire run, which poses a
challenge to modular middleware systems where different parts of a project may
require different configurations.

Protocol buffers
^^^^^^^^^^^^^^^^
`Protocol buffers <https://developers.google.com/protocol-buffers>`_ are an
efficient system for serializing structured data. They are widely used by Google
and other companies.

The protobuf compiler ``protoc`` generates Python modules from ``.proto`` files.
``protoc`` strictly generates protobuf modules according to their directory
structure. This works well in a monorepo, but poses a challenge to a middleware
system like Pigweed. Generating protobufs by path also makes integrating
protobufs with existing packages awkward.

Requirements
------------
Pigweed aims to provide high quality software components and a fast, effective,
flexible development experience for its customers. Pigweed's high-level goals
and the `challenges`_ described above inform these requirements for the Pigweed
Python build.

- Integrate seamlessly with the other Pigweed build tools.
- Easy to use independently, even if primarily using a different build system.
- Support standard packaging and distribution with setuptools, wheel, and pip.
- Correctly manage interdependent local Python packages.
- Out-of-the-box support for writing and running tests.
- Preconfigured, trivial-to-run static analysis integration for Pylint and Mypy.
- Fast, dependency-aware incremental rebuilds and test execution, suitable for
  use with :ref:`module-pw_watch`.
- Seamless protocol buffer support.

Design Decision
---------------
Existing Python tools may be effective for Python codebases, but their utility
is more limited in a multi-language project like Pigweed. The cost of bringing
up and maintaining an additional build automation system for a single language
is high.

Pigweed uses GN as its primary build system for all languages. While GN does not
natively support Python, adding support is straightforward with GN templates.

GN has strong multi-toolchain and multi-language capabilities. In GN, it is
straightforward to share targets and artifacts between different languages. For
example, C++, Go, and Python targets can depend on the same protobuf
declaration. When using GN for multiple languages, Ninja schedules build steps
for all languages together, resulting in faster total build times.

Not all Pigweed users build with GN. Of Pigweed's three supported build systems,
GN is the fastest, lightest weight, and easiest to run. It also has simple,
clean syntax. This makes it feasible to use GN only for Python while building
primarily with a different system.

Given these considerations, GN is an ideal choice for Pigweed's Python build.

.. _Configuring setup() using setup.cfg files: https://ipython.readthedocs.io/en/stable/interactive/reference.html#embedding
.. _Build System Support - How to use it?: https://setuptools.readthedocs.io/en/latest/build_meta.html?highlight=pyproject.toml#how-to-use-it
