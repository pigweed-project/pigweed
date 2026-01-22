.. _module-pw_memory:

=========
pw_memory
=========
.. pigweed-module::
   :name: pw_memory

``pw_memory`` is a collection of low-level utilities for managing memory and
object lifetime utilities. It is similar to C++'s ``<memory>`` header, but
memory allocation and smart pointers are provided by :ref:`module-pw_allocator`
instead.

---------------------------------------------------------
Global variables: constant initialization and binary size
---------------------------------------------------------
-lobal variables---variables with static storage duration---are initialized
either during compilation (constant initialization) or at runtime.
Runtime-initialized globals are initialized before ``main``; function ``static``
variables are initialized when the function is called the first time.

Constant initialization is guaranteed for ``constinit`` or ``constexpr``
variables. However, the compiler may constant initialize any variable, even if
it is not ``constinit`` or ``constexpr`` constructible.

Constant initialization is usually better than runtime initialization. Constant
initialization:

- Reduces binary size. The binary stores initialized variable in the binary
  (e.g. in ``.data`` or ``.rodata``), instead of the code needed to produce that
  data, which is typically larger.
- Saves CPU cycles. Initialization is a simple ``memcpy``.
- Avoids the `static initialization order fiasco
  <https://en.cppreference.com/w/cpp/language/siof>`_. Constant initialization
  is order-independent and occurs before static initialization.

Constant initialization may be undesirable if initialized data is larger than
the code that produces it. Variables that are initialized to all 0s are
placed in a zero-initialized segment (e.g. ``.bss``) and never affect binary
size. Non-zero globals may increase binary size if they are constant
initialized, however.

Should I constant initialize?
-----------------------------
Globals should usually be constant initialized when possible. Consider the
following when deciding:

- If the global is zero-initialized, make it ``constinit`` or ``constexpr`` if
  possible. It will not increase binary size.
- If the global is initialized to anything other than 0 or ``nullptr``,
  it will occupy space in the binary.

  - If the variable is small (e.g. a few words), make it ``constinit`` or
    ``constexpr``. The initialized variable takes space in the binary, but it
    probably takes less space than the code to initialize it would.
  - If the variable is large, weigh its size against the size and runtime
    cost of its initialization code.

There is no hard-and-fast rule for when to constant initialize or not. The
decision must be considered in light of each project's memory layout and
capabilities. Experimentation may be necessary.

**Example**

.. literalinclude:: globals_test.cc
   :start-after: [pw_memory-globals-init]
   :end-before: [pw_memory-globals-init]
   :language: cpp

.. note::

   Objects cannot be split between ``.data`` and ``.bss``. If an object contains
   a single ``bool`` initialized to ``true`` followed by a 4KB array of zeroes,
   it will be placed in ``.data``, and all 4096 zeroes will be present in the
   binary.

   A global ``pw::Vector`` works like this. A default-initialized
   ``pw::Vector<char, 4096>`` includes one non-zero ``uint16_t``. If constant
   initialized, the entire ``pw::Vector`` is stored in the binary, even though
   it is mostly zeroes.

Controlling constant initialization of globals
----------------------------------------------
``pw_memory`` offers two utilities for declaring global variables:

- :cc:`pw::NoDestructor` -- Removes the destructor, which is not
  necessary for globals. Constant initialization is supported, but not required.
- :cc:`pw::RuntimeInitGlobal` -- Removes the destructor. Prevents
  constant initialization.

It is recommended to specify constant or runtime initialization for all global
variables.

.. list-table:: **Declaring globals**
   :header-rows: 1

   * - Initialization
     - Mutability
     - Declaration
   * - constant
     - mutable
     - | ``constinit T``
       | ``constinit pw::NoDestructor<T>``
   * - constant
     - constant
     - ``constexpr T``
   * - runtime
     - mutable
     - ``pw::RuntimeInitGlobal<T>``
   * - runtime
     - constant
     - ``const pw::RuntimeInitGlobal<T>``
   * - unspecified
     - constant
     - | ``const T``
       | ``const pw::NoDestructor<T>``
   * - unspecified
     - mutable
     - | ``T``
       | ``pw::NoDestructor<T>``
