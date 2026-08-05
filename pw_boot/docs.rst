.. _module-pw_boot:

=======
pw_boot
=======
.. pigweed-module::
   :name: pw_boot

``pw_boot`` provides a linker script and some early initialization of static
memory regions and C++ constructors. This is enough to get many targets booted
and ready to run C++ code.

This module is split into two components:

1. This module, which provides the :ref:`facade <docs-facades>`.
2. A backend module such as :ref:`module-pw_boot_cortex_m` that implements the
   facade.

.. _module-pw_boot-status:

---------------------
Status of this module
---------------------
``pw_boot`` can be used in production if it meets your needs. In practice
most production projects probably won't use it because they need their own
custom linker scripts. The ``pw_boot`` source code can still be a useful
example of how to set up a boot sequence.

.. _module-pw_boot-sequence:

--------
Sequence
--------
The high-level ``pw_boot`` sequence looks like the following pseudo-code
invocation of the user-implemented functions:

.. code-block:: cpp

   void pw_boot_Entry() {            // Boot entry point provided by backend.
     pw_boot_PreStaticMemoryInit();  // User-implemented function.
     // Static memory initialization.
     pw_boot_PreStaticConstructorInit();  // User-implemented function.
     // C++ static constructors are invoked.
     pw_boot_PreMainInit();  // User-implemented function.
     main();                 // User-implemented function.
     pw_boot_PostMain();     // User-implemented function.
     PW_UNREACHABLE;
   }

.. _module-pw_boot-user:

--------------------------
User-implemented functions
--------------------------
This module expects all of the following ``extern "C"`` functions to be defined
outside this module. If any of these functions are unimplemented, executables
will encounter a link error.

.. _module-pw_boot-prestaticmemoryinit:

``pw_boot_PreStaticMemoryInit()``
---------------------------------
Signature: ``void pw_boot_PreStaticMemoryInit()``

This function executes just before static memory has been zeroed and static
data is initialized. This function should set up any early initialization that
should be done before static memory is initialized, such as:

- Enabling the FPU or other coprocessors.
- Opting into extra restrictions such as disabling unaligned access to ensure
  the restrictions are active during static RAM initialization.
- Initial CPU clock, flash, and memory configurations including potentially
  enabling extra memory regions with ``.bss`` and ``.data`` sections, such as
  SDRAM or backup powered SRAM.
- Fault handler initialization if required before static memory
  initialization.

.. warning::

   Code running in this hook is violating the C spec as static values are not
   yet initialized, meaning they have not been loaded (``.data``) nor
   zero-initialized (``.bss``).

.. _module-pw_boot-prestaticconstructorinit:

``pw_boot_PreStaticConstructorInit()``
--------------------------------------
Signature: ``void pw_boot_PreStaticConstructorInit()``

This function executes just before C++ static constructors are called. At this
point, other static memory has been zero-initialized or data-initialized. This
function should set up any early initialization that should be done before C++
static constructors are run, such as:

- Run time dependencies such as ``malloc``, and ergo sometimes the RTOS.
- Persistent memory that survives warm reboots.
- Enabling the MPU to catch ``nullptr`` dereferences during construction.
- Main stack watermarking.
- Further fault handling configuration necessary for your platform which
  was not safe before ``pw_boot_PreStaticRamInit()``.
- Boot count and/or boot session UUID management.

.. _module-pw_boot-premaininit:

``pw_boot_PreMainInit()``
-------------------------
Signature: ``void pw_boot_PreMainInit()``

This function executes just before ``main()``, and can be used for any device
initialization that isn't application-specific. Depending on your platform,
this might be turning on a UART, setting up default clocks, etc.

.. _module-pw_boot-main:

``main()``
----------
Signature: ``int main()``

This is where applications reside.

.. _module-pw_boot-postmain:

``pw_boot_PostMain()``
----------------------
Signature: ``PW_NO_RETURN void pw_boot_PostMain()``

This function executes after ``main()`` has returned. This could be used for
device-specific teardown such as an infinite loop, soft reset, or QEMU
shutdown. In addition, if relevant for your application, this would be the
place to invoke the global static destructors. This function must not return!

.. _module-pw_boot-backend:

-----------------------------
Backend-implemented functions
-----------------------------
``pw_boot`` :ref:`backends <docs-facades-definition>` must implement the
following ``extern "C"`` functions.

.. _module-pw_boot-entry:

``pw_boot_Entry()``
-------------------
Signature: ``void pw_boot_Entry()``

This function executes as the entry point for the application, and must
call the :ref:`module-pw_boot-user` in the correct
:ref:`module-pw_boot-sequence`.

.. _module-pw_boot-dependencies:

------------
Dependencies
------------
- :bdg-ref-primary-line:`module-pw_preprocessor`

========
Security
========
See :ref:`module-pw_boot-security` for the module-specific threat model.

.. toctree::
   :hidden:
   :maxdepth: 1

   backends
   security
