.. _module-pw_build-bazel_rules:

===========
Bazel rules
===========
.. pigweed-module-subpage::
   :name: pw_build

.. _module-pw_build-bazel-wrapper-rules:

-------------
Wrapper rules
-------------
The built-in Bazel rules ``cc_binary``, ``cc_test``, ``py_binary``, and
``py_test`` are wrapped with :ref:`module-pw_build-bazel-pw_cc_binary`,
``pw_cc_test``, ``pw_py_binary``, and ``pw_py_test``, respectively.
To access a wrapper, load its individual ``.bzl`` file. For example, to
access ``pw_cc_binary``:

.. code-block:: python

   load("@pigweed//pw_build:pw_cc_binary.bzl", "pw_cc_binary")

These wrappers should be used across Pigweed in place of their original
counterparts. Projects using Pigweed may elect to use these wrappers, but
don't necessarily need to.

pw_cc_test
==========
``pw_cc_test`` is a non-trival wrapper that instantiates many different flavors
of test types using roughly the same interface as ``cc_test``.

Other wrappers
==============
Most of the other wrappers provide minor compatibility constraints or injected
dependencies. Please see the documentation for each wrapper for additional
information.

.. _module-pw_build-bazel-pw_linker_script:

----------------
pw_linker_script
----------------
In addition to wrapping the built-in rules, Pigweed also provides a custom
rule for handling linker scripts with Bazel. e.g.

.. code-block:: python

   pw_linker_script(
     name = "some_linker_script",
     linker_script = ":some_configurable_linker_script.ld",
     defines = [
         "PW_BOOT_FLASH_BEGIN=0x08000200",
         "PW_BOOT_FLASH_SIZE=1024K",
         "PW_BOOT_HEAP_SIZE=112K",
         "PW_BOOT_MIN_STACK_SIZE=1K",
         "PW_BOOT_RAM_BEGIN=0x20000000",
         "PW_BOOT_RAM_SIZE=192K",
         "PW_BOOT_VECTOR_TABLE_BEGIN=0x08000000",
         "PW_BOOT_VECTOR_TABLE_SIZE=512",
     ],
     deps = [":some_header_library"],
   )

   # You can include headers provided by targets specified in deps.
   cc_library(
     name = "some_header_library",
     hdrs = ["test_header.h"],
     includes = ["."],
   )

   # You can include the linker script in the deps.
   cc_binary(
     name = "some_binary",
     srcs = ["some_source.cc"],
     deps = [":some_linker_script"],
   )

   # Alternatively, you can use additional_linker_inputs and linkopts. This
   # allows you to explicitly specify the command line order of linker scripts,
   # and may be useful if your project defines more than one.
   cc_binary(
     name = "some_binary",
     srcs = ["some_source.cc"],
     additional_linker_inputs = [":some_linker_script"],
     linkopts = ["-T $(execpath :some_linker_script)"],
   )

.. _module-pw_build-bazel-pw_facade:

---------
pw_facade
---------
In Bazel, a :ref:`facade <docs-module-structure-facades>` module has a few
components:

#. The **facade target**, i.e. the interface to the module. This is what
   *backend implementations* depend on to know what interface they're supposed
   to implement.

#. The **library target**, i.e. both the facade (interface) and backend
   (implementation). This is what *users of the module* depend on. It's a
   regular ``cc_library`` that exposes the same headers as the facade, but
   has a dependency on the "backend label flag" (discussed next). It may also
   include some source files (if these are backend-independent).

   Both the facade and library targets are created using the
   ``pw_facade`` macro. For example, consider the following
   macro invocation:

   .. code-block:: python

      pw_facade(
          name = "binary_semaphore",
          # A backend-independent source file.
          srcs = [
              "binary_semaphore.cc",
          ],
          # The facade header.
          hdrs = [
              "public/pw_sync/binary_semaphore.h",
          ],
          # Dependencies of this header.
          deps = [
              "//pw_chrono:system_clock",
              "//pw_preprocessor",
          ],
          # The backend, hidden behind a label_flag; see below.
          backend = [
              ":binary_semaphore_backend",
          ],
      )

   This macro expands to both the library target, named ``binary_semaphore``,
   and the facade target, named ``binary_semaphore.facade``.

#. The **backend label flag**. This is a `label_flag
   <https://bazel.build/extending/config#label-typed-build-settings>`_: a
   dependency edge in the build graph that can be overridden by downstream
   projects.

#. The **backend target** implements a particular backend for a facade. It's
   just a plain ``cc_library``, with a dependency on the facade target. For
   example,

   .. code-block:: python

      cc_library(
          name = "binary_semaphore",
          srcs = [
              "binary_semaphore.cc",
          ],
          hdrs = [
              "public/pw_sync_stl/binary_semaphore_inline.h",
              "public/pw_sync_stl/binary_semaphore_native.h",
              "public_overrides/pw_sync_backend/binary_semaphore_inline.h",
              "public_overrides/pw_sync_backend/binary_semaphore_native.h",
          ],
          includes = [
              "public",
              "public_overrides",
          ],
          deps = [
              # Dependencies of the backend's headers and sources.
              "//pw_assert",
              "//pw_chrono:system_clock",
              # A dependency on the facade target, which defines the interface
              # this backend target implements.
              "//pw_sync:binary_semaphore.facade",
          ],
      )

The backend label flag should point at the backend target. Typically, the
backend you want to use depends on the platform you are building for. See the
:ref:`docs-build_system-bazel_configuration` for advice on how to set this up.

------------------
pw_cc_blob_library
------------------
The ``pw_cc_blob_library`` rule is useful for embedding binary data into a
program. The rule takes in a mapping of symbol names to file paths, and
generates a set of C++ source and header files that embed the contents of the
passed-in files as arrays of ``std::byte``.

The blob byte arrays are constant initialized and are safe to access at any
time, including before ``main()``.

``pw_cc_blob_library`` is also available in the :ref:`GN
<module-pw_build-cc_blob_library>` and CMake builds.

Arguments
=========
* ``blobs``: A list of ``pw_cc_blob_info`` targets, where each target
  corresponds to a binary blob to be transformed from file to byte array. This
  is a required field. ``pw_cc_blob_info`` attributes include:

  * ``symbol_name``: The C++ symbol for the byte array.
  * ``file_path``: The file path for the binary blob.
  * ``linker_section``: If present, places the byte array in the specified
    linker section.
  * ``alignas``: If present, uses the specified string verbatim in
    the ``alignas()`` specifier for the byte array.

* ``out_header``: The header file to generate. Users will include this file
  exactly as it is written here to reference the byte arrays.
* ``namespace``: C++ namespace to place the generated blobs within.
* ``alwayslink``: Whether this library should always be linked. Defaults to
  false.

Example
=======
**BUILD.bazel**

.. code-block:: python

   pw_cc_blob_info(
     name = "foo_blob",
     file_path = "foo.bin",
     symbol_name = "kFooBlob",
   )

   pw_cc_blob_info(
     name = "bar_blob",
     file_path = "bar.bin",
     symbol_name = "kBarBlob",
     linker_section = ".bar_section",
   )

   pw_cc_blob_library(
     name = "foo_bar_blobs",
     blobs = [
       ":foo_blob",
       ":bar_blob",
     ],
     out_header = "my/stuff/foo_bar_blobs.h",
     namespace = "my::stuff",
   )

.. note:: If the binary blobs are generated as part of the build, be sure to
          list them as deps to the pw_cc_blob_library target.

**Generated Header**

.. code-block::

   #pragma once

   #include <array>
   #include <cstddef>

   namespace my::stuff {

   extern const std::array<std::byte, 100> kFooBlob;

   extern const std::array<std::byte, 50> kBarBlob;

   }  // namespace my::stuff

**Generated Source**

.. code-block::

   #include "my/stuff/foo_bar_blobs.h"

   #include <array>
   #include <cstddef>

   #include "pw_preprocessor/compiler.h"

   namespace my::stuff {

   const std::array<std::byte, 100> kFooBlob = { ... };

   PW_PLACE_IN_SECTION(".bar_section")
   const std::array<std::byte, 50> kBarBlob = { ... };

   }  // namespace my::stuff

.. _module-pw_build-bazel-pw_cc_binary:

------------
pw_cc_binary
------------
.. _cc_binary: https://bazel.build/reference/be/c-cpp#cc_binary

``pw_cc_binary`` is a wrapper of `cc_binary`_. It's implemented at
:cs:`pw_build/pw_cc_binary.bzl`. Usage of this wrapper is optional;
downstream Pigweed projects can instead use ``cc_binary`` if preferred.

Basic usage:

.. code-block:: python

   load("@pigweed//pw_build:pw_cc_binary.bzl", "pw_cc_binary")

   pw_cc_binary(
       name = "…",
       srcs = ["…"],
       deps = [
           "…",
       ],
   )

Pros of using ``pw_cc_binary``:

* It simplifies :ref:`link-time dependency
  <docs-build_system-bazel_link-extra-lib>`. Projects using ``cc_binary``
  must set up (and document) link-time dependency themselves.

Cons of using ``pw_cc_binary``:

.. _magical: https://en.wikipedia.org/wiki/Magic_(programming)

* It makes the configuration of :ref:`module-pw_log` and
  :ref:`module-pw_assert` a bit more `magical`_.

.. _module-pw_build-bazel-pw_cc_binary_with_map:

---------------------
pw_cc_binary_with_map
---------------------
The ``pw_cc_binary_with_map`` rule can be used to build a binary like
``cc_binary`` does but also generate a .map file from the linking step.

.. code-block:: python

   pw_cc_binary_with_map(
     name = "test",
     srcs = ["empty_main.cc"],
   )

This should result in a ``test.map`` file generated next to the ``test`` binary.

Note that it's only partially compatible with the ``cc_binary`` interface and
certain things are not implemented like make variable substitution.

.. _module-pw_build-bazel-pw_elf_to_bin:

-------------
pw_elf_to_bin
-------------
The ``pw_elf_to_bin`` rule takes in a binary executable target and produces a
file using the ``-Obinary`` option to ``objcopy``. This is only suitable for use
with binaries where all the segments are non-overlapping. A common use case for
this type of file is booting directly on hardware with no bootloader.

.. code-block:: python

   load("@pigweed//pw_build:binary_tools.bzl", "pw_elf_to_bin")

   pw_elf_to_bin(
     name = "bin",
     elf_input = ":main",
     bin_out = "main.bin",
   )

.. _module-pw_build-bazel-pw_elf_to_dump:

--------------
pw_elf_to_dump
--------------
The ``pw_elf_to_dump`` rule takes in a binary executable target and produces a
text file containing the output of the toolchain's ``objdump -xd`` command. This
contains the full binary layout, symbol table and disassembly which is often
useful when debugging embedded firmware.

.. code-block:: python

   load("@pigweed//pw_build:binary_tools.bzl", "pw_elf_to_dump")

   pw_elf_to_dump(
     name = "dump",
     elf_input = ":main",
     dump_out = "main.dump",
   )

----------------------
pw_copy_and_patch_file
----------------------
Provides the ability to patch a file as part of the build.

The source file will not be patched in place, but instead copied before
patching. The output of this target will be the patched file.

Arguments
=========
- ``name``: The name of the target.
- ``source``: The source file to be patched.
- ``out``: The output file containing the patched contents.
- ``patch_file``: The patch file.

Example
=======
To apply the patch `changes.patch` to the file `data/file.txt` which is located
in the bazel dependency `@external-sdk//`.

.. code-block::

   pw_copy_and_patch_file(
       name = "apply_patch",
       src = "@external-sdk//data/file.txt",
       out = "data/patched_file.txt",
       patch_file = "changes.patch",
   )

------------------------
pw_py_importable_runfile
------------------------
An importable ``py_library`` that makes loading runfiles easier.

When using Bazel runfiles from Python,
`Rlocation() <https://rules-python.readthedocs.io/en/latest/api/py/runfiles/runfiles.runfiles.html#runfiles.runfiles.Runfiles.Rlocation>`__
takes two arguments:

1. The ``path`` of the runfiles. This is the apparent repo name joined with
   the path within that repo.
2. The ``source_repo`` to evaluate ``path`` from. This is related to how
   apparent repo names and canonical repo names are handled by Bazel.

Unfortunately, it's easy to get these arguments wrong.

This generated Python library short-circuits this problem by letting Bazel
generate the correct arguments to ``Rlocation()`` so users don't even have
to think about what to pass.

For example:

.. code-block:: python

   # In @bloaty//:BUILD.bazel, or wherever is convenient:
   pw_py_importable_runfile(
       name = "bloaty_runfiles",
       src = "//:bin/bloaty",
       executable = True,
       import_location = "bloaty.bloaty_binary",
       visibility = ["//visibility:public"],
   )

   # Using the pw_py_importable_runfile from a py_binary in a
   # BUILD.bazel file:
   py_binary(
       name = "my_binary",
       srcs = ["my_binary.py"],
       main = "my_binary.py",
       deps = ["@bloaty//:bloaty_runfiles"],
   )

   # In my_binary.py:
   import bloaty.bloaty_binary
   from python.runfiles import runfiles  # type: ignore

   r = runfiles.Create()
   bloaty_path = r.Rlocation(*bloaty.bloaty_binary.RLOCATION)

.. note::

   Because this exposes runfiles as importable Python modules,
   the import paths of the generated libraries may collide with existing
   Python libraries. When this occurs, you need to
   :ref:`docs-style-python-extend-generated-import-paths`.

Attrs
=====

.. list-table::
   :header-rows: 1

   * - Name
     - Description
   * - import_location
     - The final Python import path of the generated module. By default, this is
       ``path.to.package.label_name``.
   * - target
     - The file this library exposes as runfiles.
   * - executable
     - Whether or not the source file is executable.
   * - \*\*kwargs
     - Common attributes to forward both underlying targets.

.. _module-pw_build-bazel-boolean_constraint_value:

------------------------
boolean_constraint_value
------------------------
This macro is syntactic sugar for declaring a `constraint setting
<https://bazel.build/reference/be/platforms-and-toolchains#constraint_setting>`__
with just two possible `constraint values
<https://bazel.build/reference/be/platforms-and-toolchains#constraint_value>`__.
The only exposed target is the ``constraint_value`` corresponding to ``True``;
the default value of the setting is ``False``.

This macro is meant to simplify declaring
:ref:`docs-bazel-compatibility-module-specific`.

------------------
host_backend_alias
------------------
An alias that resolves to the backend for host platforms. This is useful when
declaring a facade that provides a default backend for host platform use.

.. _module-pw_build-bazel-pw_hermetic_cc_library:

----------------------
pw_hermetic_cc_library
----------------------
The ``pw_hermetic_cc_library`` rule produces an incrementally-linked hermetic C/C++ static
library. It compiles source files from ``srcs``, takes private dependencies from ``deps``,
performs a relocatable link (``ld -r``), and localizes all internal symbols using ``objcopy``.

Only linkage symbols explicitly listed in ``global_symbols`` or ``global_symbols_file`` remain
globally exported. Public dependencies and headers specified in ``public_deps`` and ``hdrs`` are
propagated to downstream consumers without localization.

.. code-block:: python

   load("@pigweed//pw_build:pw_hermetic_cc_library.bzl", "pw_hermetic_cc_library")

   pw_hermetic_cc_library(
       name = "my_hermetic_lib",
       srcs = ["lib.cc"],
       hdrs = ["public/my_lib/lib.h"],
       includes = ["public"],
       deps = [":private_dependency"],
       # Preserved global symbols reflecting the public linkage API of this library.
       # (Wildcards are not supported for global_symbols).
       global_symbols = [
           "my_lib_exported_foo",
           "my_lib_exported_bar",
       ],
       # External symbols allowed to be referenced by this library.
       # (Supports fnmatch wildcard patterns).
       undefined_symbols = [
           "malloc",
           "free",
           "memset",
           "memcpy",
           "*printf",
       ],
   )
