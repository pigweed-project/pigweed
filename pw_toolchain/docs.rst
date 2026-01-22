.. _module-pw_toolchain:

============
pw_toolchain
============
.. pigweed-module::
   :name: pw_toolchain

GN toolchains function both as a set of tools for compilation and as a workspace
for evaluating build files. The same compilations and actions can be executed by
different toolchains. Each toolchain maintains its own set of build args, and
build steps from all toolchains can be executed in parallel.

---------------------------------
C/C++ toolchain support libraries
---------------------------------
``pw_toolchain`` provides some toolchain-related C/C++ libraries.

``std:abort`` wrapper
=====================
The `std::abort <https://en.cppreference.com/w/cpp/utility/program/abort>`_
function is used to terminate a program abnormally. This function may be called
by standard library functions, so is often linked into binaries, even if users
never intentionally call it.

For embedded builds, the ``abort`` implementation likely does not work as
intended. For example, it may pull in undesired dependencies (e.g.
``std::raise``) and end in an infinite loop.

``pw_toolchain`` provides the ``pw_toolchain:wrap_abort`` library that replaces
``abort`` in builds where the default behavior is undesirable. It uses the
``-Wl,--wrap=abort`` linker option to redirect to ``abort`` calls to
``PW_CRASH`` instead.

arm-none-eabi-gcc support
=========================
Targets building with the GNU Arm Embedded Toolchain (``arm-none-eabi-gcc``)
should depend on the ``pw_toolchain/arm_gcc:arm_none_eabi_gcc_support``
library. In GN, that target should be included in ``pw_build_LINK_DEPS``. In
Bazel, it should be added to `link_extra_lib
<https://bazel.build/reference/be/c-cpp#cc_binary.link_extra_lib>`__ or
directly to the `deps` of any binary being build with that toolchain:

.. code-block:: python

   cc_binary(
      deps = [
        # Other deps, omitted
      ] + select({
        "@platforms//cpu:armv7e-m": [
          "@pigweed//pw_toolchain/arm_gcc:arm_none_eabi_gcc_support",
        ],
        "//conditions:default": [],
      }),
   )

Newlib OS interface
-------------------
`Newlib <https://sourceware.org/newlib/>`_, the C Standard Library
implementation provided with ``arm-none-eabi-gcc``, defines a set of `OS
interface functions <https://sourceware.org/newlib/libc.html#Stubs>`_ that
should be implemented. Newlib provides default implementations, but using these
results in linker warnings like the following:

.. code-block:: none

   readr.c:(.text._read_r+0x10): warning: _read is not implemented and will always fail

Most of the OS interface functions should never be called in embedded builds.
The ``pw_toolchain/arg_gcc:newlib_os_interface_stubs`` library, which is
provided through ``pw_toolchain/arm_gcc:arm_none_eabi_gcc_support``, implements
these functions and forces a linker error if they are used. It also
automatically includes a wrapper for ``abort`` for use of ``stdout`` and
``stderr`` which abort if they are called.

If you need to use your own wrapper for ``abort``, include the library directly
using ``pw_toolchain/arm_gcc:newlib_os_interface_stubs``.

.. _module-pw_toolchain-cpp-globals:

Freestanding support
====================
While Pigweed largely works with ``-ffreestanding``, Pigweed has observed
issues where ``newlib-nano`` loses ``PRIx64`` and the other 64-bit
``PRI*`` macros due to ``gcc``'s ``stdint-gcc.h`` being pulled in rather than
newlib-nano's ``stdint.h`` (see https://pwbug.dev/382484307).

Additionally, ``-ffreestanding`` isn't often correctly supported well, and most
embedded toolchains and libc implementations have been designed around the
assumption that ``-ffreestanding`` is not set. This partial support can cause
confusing errors/behaviors that wouldn't be encountered under normal conditions.

For these reasons, Pigweed recommends most projects **do not** use
``-ffreestanding``.

API reference
=============
Moved: :cc:`pw_toolchain`

builtins
========
builtins are LLVM's equivalent of libgcc, the compiler will insert calls to
these routines. Setting the ``dir_pw_third_party_builtins`` gn var to your
compiler-rt/builtins checkout will enable building builtins from source instead
of relying on the shipped libgcc.

.. toctree::
   :hidden:
   :maxdepth: 1

   bazel
   gn
