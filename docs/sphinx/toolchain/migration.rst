.. _toolchain-migration:

============================
Migrate from a GNU toolchain
============================
When migrating an existing project from a GNU toolchain
(GCC, Binutils, newlib, libstdc++, libgcc) to LLVM (Clang, LLD, LLVM libc,
libc++, compiler-rt), adopt an incremental, three-stage approach and keep the
project building and testable at every stage:

#. **Make it compile.** Switch the compiler invocation to Clang while
   retaining the existing linker and runtime libraries. Resolve diagnostics
   and non-portable GNU extensions.
#. **Make it fit.** Switch the link step to LLD, then swap the runtime
   libraries. Audit linker scripts and get back under your size budget.
#. **Make it run.** Validate on hardware. ABI and alignment problems surface
   here, not at build time.

The rest of this guide covers the problems that projects hit most often at
each stage.

.. _toolchain-migration-rollout:

----------------
Plan the rollout
----------------
* **Migrate incrementally.** Don't convert the whole codebase at once. Add
  parallel build targets (for example ``target_clang`` alongside
  ``target_gcc``) so you can work through compilation errors without breaking
  the production build.
* **Split the flags per image.** For projects that build several images, such
  as a bootloader and an application, use separate options for each (for
  example ``build_bootloader_with_clang`` and ``build_prod_app_with_clang``).
  This limits the blast radius and makes runtime failures such as a bricked
  device much easier to isolate.
* **Keep generated build files toolchain-specific.** If your build generates
  files into the source tree, such as extracting compiler flags from CMake
  into GN, give each toolchain its own output directory. Otherwise the GCC and
  Clang builds overwrite each other's flags and fail in confusing ways.
* **Update host-side tooling too.** Size reports, coredump parsers, and
  flashing scripts must move to the LLVM binary utilities (``llvm-size``,
  ``llvm-nm``, ``llvm-objcopy``). ``llvm-objcopy`` is stricter than GNU
  ``objcopy``: removing a section that's still referenced by another section's
  ``sh_link``, such as ``rom_start`` referenced by ``.ARM.exidx``, fails
  unless you pass ``--allow-broken-links``.

.. _toolchain-migration-diagnostics:

-------------------------------------------
Fix diagnostics and non-portable extensions
-------------------------------------------
Clang conforms more closely to the C and C++ standards and warns about more
constructs than GCC, so expect a batch of ``-Werror`` failures first:

* **Unused code and shadowing**: Clang flags unused variables, unused private
  members, unused functions, and variable shadowing. Delete the dead code or
  mark deliberate cases with ``[[maybe_unused]]``.
* **Invalid** ``constexpr``: Clang rejects standard violations that GCC
  sometimes accepts, such as ``reinterpret_cast`` in a ``constexpr``
  declaration. Change these to ``const`` or ``inline``.
* **Naked functions**: Clang rejects ``__attribute__((naked))`` functions that
  contain anything other than assembly, including compiler-generated prologue
  and epilogue code. Write them as pure inline assembly, or use a normal
  function with an ``asm`` block.
* ``printf`` **format specifiers**: GCC and Clang may use different underlying
  types for ``uint32_t`` and friends (``unsigned int`` versus ``unsigned
  long`` on 32-bit targets), which produces format warnings. Cast explicitly
  at the call site to match the specifier.
* **Function-level optimization attributes**: ``__attribute__((optimize(...)))``
  isn't supported. Use per-file or per-target compiler options instead.

.. _toolchain-migration-abi:

-------------
Match the ABI
-------------
ABI mismatches don't fail the link. They corrupt data at runtime, so check
them deliberately:

* **Short enums**: GCC defaults to ``-fshort-enums`` on Arm targets while
  Clang uses 32-bit enums. Mixing the two changes struct layout and produces
  failures such as corrupt OTA updates. Pass ``-fshort-enums`` to Clang
  explicitly to stay compatible with GCC-built code.
* **Alignment**: Clang may emit instructions that require strict alignment,
  such as ``LDRD`` and ``STRD`` on Arm, where GCC emitted alignment-safe
  sequences. Unsafe pointer casts that worked under GCC can hard fault. Audit
  the casts and give the affected structs and buffers explicit alignment with
  ``alignas``.
* **Atomics**: Atomic operations on under-aligned types lower to compiler-rt
  library calls instead of native instructions. Align atomic variables
  naturally; :ref:`module-pw_alignment` provides ``pw::AlignedAtomic`` for
  this.
* **Prebuilt vendor libraries**: Linking Clang-built code against archives
  built by ``arm-none-eabi-gcc`` generally works, but only if the
  ABI-affecting options match. ``-fshort-enums``, the floating-point ABI, and
  packing attributes are the usual culprits.

UBSan's minimal embedded runtime is an effective way to catch the misaligned
accesses and undefined behavior that these mismatches cause.

.. _toolchain-migration-linker:

-----------------------------
Update linker scripts for LLD
-----------------------------
LLD is stricter than GNU ``ld`` about both syntax and memory layout:

* **Unsupported syntax**: Some GNU-specific constructs aren't accepted, such
  as ``KEEP(*libgcc.a:save-restore.o)`` or the ``i`` attribute in a memory
  region definition. Replace them with standard patterns or remove them.
* **Section names with spaces**: LLD may quote them in the output ELF, which
  breaks downstream post-processing tools. Rename the sections.
* **Overlapping load addresses**: LLD validates LMAs strictly and fails the
  link if two sections overlap in physical memory, for example a
  ``.sdk_version`` section overlapping ``.bss``. Sequence the LMAs explicitly
  so each section starts after the previous one ends.
* **Global constructor symbols**: Clang and GCC emit slightly different
  constructor symbol names. Match both with a ``_GLOBAL__sub_I_*`` pattern (or
  ``.text._GLOBAL__sub_I_*`` if ``-ffunction-sections`` is used), or
  constructors can land in RAM instead of flash.

.. _toolchain-migration-runtimes:

--------------------------
Swap the runtime libraries
--------------------------
Moving from libstdc++ and newlib to libc++, LLVM libc, and compiler-rt is
usually the longest stage:

* **Iterators aren't pointers**: Under libc++, ``std::begin()`` and
  ``std::end()`` return iterator objects that may not be raw pointers. Where a
  pointer is required, such as a ``pw::span`` constructor, use ``.data()``.
* **Fewer transitive includes**: LLVM's headers are more modular, so code that
  compiled under GCC by accident now needs its own includes, for example
  ``<cmath>`` for ``std::abs`` and ``std::round``.
* **No POSIX headers**: LLVM libc's embedded profiles intentionally omit
  ``<unistd.h>``, ``<fcntl.h>``, ``<sys/*.h>``, and POSIX types. Replace
  them with standard types, for example ``ssize_t`` with ``ptrdiff_t`` and
  ``uint`` with ``unsigned``.
* **Third-party libraries assume a hosted environment**: Libraries such as
  mbedTLS and Nanopb reference ``malloc``, or ``printf``. Use their
  configuration macros to disable or redirect dynamic memory and I/O, and add
  stubs to :ref:`module-pw_libc` only when there's no alternative.
* **libc++ needs a few baremetal stubs**: libc++ references symbols such as
  ``_LIBCPP_VERBOSE_ABORT`` and ``operator delete`` for global destructors.
  Provide overrides that map them to a trap or an assert.
* **compiler-rt builtins**: Make sure the build compiles the builtins for your
  architecture, such as the Armv6-M and Armv7-M sources in
  :cs:`third_party/llvm_builtins`, or operations like 64-bit division won't
  link.
* **Duplicate symbols from vendor blobs**: Precompiled vendor libraries often
  bundle their own runtime helpers, which collide with compiler-rt. Exclude
  the conflicting objects from the compiler-rt build.

.. _toolchain-migration-size:

------------------------------
Get back under the size budget
------------------------------
Clang's optimization decisions differ from GCC's, and the first Clang build is
often larger:

* **Inlining**: Even at ``-Oz``, Clang may inline large assembly-heavy
  functions, for example in crypto libraries such as micro-ecc. Mark the
  offenders ``__attribute__((noinline))``.
* **LTO**: Archives built with GCC's LTO can't be linked by LLD, and without
  LTO unused code may not be stripped. Rebuild third-party libraries with
  Clang and enable FatLTO so dead-code elimination works across the whole
  program.
* ``KEEP`` **directives**: Overuse of ``KEEP``, for example on ``.ram_code``
  or vendor sections, prevents the linker from discarding unused code. Remove
  it from everything that isn't strictly required to boot.
* **Garbage collection and folding**: Enable ``--gc-sections`` and
  ``--icf=all``.
* **Very small images**: In a severely constrained partition, such as a 40 KB
  bootloader, you may need to disable subsystems that fit under GCC. Turning
  off logging and console I/O (for example ``CONFIG_LOG``, ``CONFIG_PRINTK``,
  ``CONFIG_CONSOLE``, ``CONFIG_SERIAL`` in Zephyr) removes formatting code such
  as ``vfprintf`` and typically saves 5–10 KB of flash.

Use :ref:`module-pw_bloat` to track size across each of these changes. For
areas of active toolchain and upstream LLVM development, see
:ref:`toolchain-roadmap`.
