.. _module-pw_enum:

=======
pw_enum
=======
.. pigweed-module-subpage::
   :name: pw_enum

``pw_enum`` supports automatic stringifying and tokenizing of C++ enums. It
works by parsing C++ standard header files and generating versions of those
headers with minimal additions needed to support these features. Enums can also
be defined in language-neutral TOML or JSON files, from which ``pw_enum``
generates equivalent C++ and Python code.

Why use ``pw_enum``?

* **Efficient string or tokenized logging**: Stringifies or :ref:`tokenizes
  <module-pw_tokenizer>` logs automatically for seamless logging.
* **Automatic content-based versioning**: Generates version hashes to prevent
  collisions as values change.
* **Cross-language consistency**: Generates matching C++ and Python enums from
  a single TOML or JSON definition.

-----------------------------------------
Automatic tokenized and stringified enums
-----------------------------------------
``pw_enum`` works on enums declared in standard C++ header files. To use
``pw_enum``:

1. Declare one or more enums in a header file.
2. Include the header file in a versioned enum target (e.g. ``pw_cc_enum_source_set``
   for GN, ``pw_cc_enum_library`` for Bazel, ``pw_add_enum_library`` for CMake).
3. Include :cs:`pw_enum/generate.h` in the header file.
4. Register the enum using the :cc:`PW_ENUM(MyEnum, ...) <PW_ENUM>` macro at
   global scope. List the fully qualified enum name, followed by all of its
   enumerators. If an enumerator has multiple aliases, only include one of them.

.. important::

   The ``PW_ENUM`` macro **must be called at global scope** (outside of any
   namespace blocks, class definitions, or functions).

   If ``PW_ENUM`` is called inside a namespace block, class, or function, the
   C++ compiler will reject it with a compilation error indicating that the
   template specialization of
   ``_PW_ENUM_cannot_be_used_within_namespaces`` must occur at global scope.

``pw_enum`` headers are parsed during the build to support versioned
tokenization and stringification with :cc:`pw::EnumToString`.

Example
=======
Declare enums in a standard C++ header and call :cc:`PW_ENUM(MyEnum, ...)
<PW_ENUM>` at the bottom of the file, outside of any namespace blocks (in the
global namespace).

.. literalinclude:: examples/private/enum_example/basic_enum.h
   :language: cpp
   :start-after: // the License.

Use the enum normally. It is tokenized with :cc:`PW_TOKENIZE_ENUM` and works
with tokenized logs and :cc:`pw::EnumToString`.

.. literalinclude:: examples/basic_enum.cc
   :language: cpp
   :start-after: // the License.

Enums can reference values from other enums, even if they reside in different
files and namespaces.

.. literalinclude:: examples/private/enum_example/other_enum.h
   :language: cpp
   :start-after: // the License.

.. literalinclude:: examples/enum_example/references_other_enum.h
   :language: cpp
   :start-after: // the License.

.. literalinclude:: examples/BUILD.bazel
   :language: python
   :start-after: [pw_enum-examples-advanced-bazel]
   :end-before: [pw_enum-examples-advanced-bazel]

Enumerator names
================
By default, enumerator names that follow Google's ``kEnumName`` style are
converted to upper snake case, without the ``k`` prefix (``ENUM_NAME``). Names
that do not follow Google style are used directly.

To override the default enumerator name, specify it in the :cc:`PW_ENUM(name,
...) <PW_ENUM>` macro with a string literal after ``=``. For example:

.. code-block:: cpp

   PW_ENUM(my::Enum,                 // String name:
           kStandardStyle,           // "STANDARD_STYLE"
           kCustom = "custom_name",  // "custom_name"
           nonStandard,              // "nonStandard"
   );

Enumerator aliases
==================
If multiple enumerator names share the same value (aliases), they can be
registered together in the ``PW_ENUM`` macro. The generator groups registered
aliases, sorting their display names alphabetically and joining them with ``|``
(e.g. ``"ALPHA|ALIAS_ALPHA"``). To omit aliases, simply leave them out of
``PW_ENUM``.

Status enums
============
``pw_enum`` integrates seamlessly with :ref:`module-pw_status`'s
:cc:`pw::StatusBase` to declare status-like classes wrapping custom enums. Use
:cc:`PW_ENUM_STATUS` in place of :cc:`PW_ENUM`. The first argument is the
fully qualified name of the status class to generate. The second argument is the
enumerator that represents success, including its enum type (e.g.
``I2cStatusCode::kOk``); its namespace is inferred from the status class if
omitted. Enums in other namespaces or nested in classes must be fully qualified.
The remaining arguments are the enumerators to register, as with ``PW_ENUM``.

Place ``PW_ENUM_STATUS`` at the bottom of the header, after any declarations
that reference the generated status class; otherwise, those declarations
require a forward declaration of the status class.

.. literalinclude:: examples/private/enum_example/i2c_status.h
   :language: cpp
   :start-after: [pw_enum-examples-status-enum]
   :end-before: [pw_enum-examples-status-enum]

.. literalinclude:: examples/BUILD.bazel
   :language: python
   :start-after: [pw_enum-examples-status-enum-bazel]
   :end-before: [pw_enum-examples-status-enum-bazel]

``pw_enum`` generates a ``final``, ``[[nodiscard]]`` class named ``I2cStatus``
derived from :cc:`pw::StatusBase`, with:

* ``I2cStatus::Code`` aliased to the underlying enum (e.g.,
  ``I2cStatus::Code::kNack``).
* Static factory functions (e.g., ``I2cStatus::Nack()``,
  ``I2cStatus::Timeout()``) generated for non-OK ``kEnumName``-style
  enumerators. No factory is generated for the OK code; default construct
  (``I2cStatus()``) for OK.
* ``Is*()`` query functions (e.g., ``status.IsNack()``, ``status.IsTimeout()``)
  generated for ``kEnumName``-style enumerators, including OK codes such as
  ``kReady``. ``IsOk()`` is not generated for codes named OK, since it would
  duplicate ``ok()``.
* An ``explicit`` constructor from ``I2cStatus::Code`` (e.g.,
  ``I2cStatus(I2cStatus::Code::kNack)``).
* Full integration with :cc:`PW_TRY`, :cc:`ok() <pw::StatusBase::ok>`,
  :cc:`code() <pw::StatusBase::code>`, :cc:`str() <pw::StatusBase::str>`, and
  tokenized logging.

Enumerators that do not get a factory or ``Is*()`` function are noted with a
``// PW_ENUM_STATUS: No ...`` comment in the generated header. A code named OK
(e.g. ``kOk``) that is not the OK code is an error, since it would conflict
with ``ok()``.

.. literalinclude:: examples/status_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-status-enum-usage]
   :end-before: [pw_enum-examples-status-enum-usage]

Logging enums
=============
Enums generated by ``pw_enum`` natively support Pigweed's tokenized logging
infrastructure.

* **Versioned format macro**: ``pw_enum`` generates a macro to use in the format
  string for the enum. The macro is named for the namespace and enum name
  (e.g. ``MY_NESTED_PKG_MY_ENUM_FMT``). The macro evaluates to a string literal
  that can be concatenated into a format string.

  The macro is versioned based on the enum's contents. The version changes
  automatically when the enum changes, so tokenized logs of enums never have
  collisions.

  A ``*_DOMAIN`` macro (e.g. ``MY_NESTED_PKG_MY_ENUM_DOMAIN``) is also generated
  with the enum's tokenization domain, for use with :ref:`nested tokenization
  <module-pw_tokenizer-nested-arguments>`.

* **Argument macro**: Include :cs:`pw_log/tokenized_args.h` and use
  :cc:`PW_LOG_ENUM(value) <PW_LOG_ENUM>` as the argument to the log
  statement.

When using a tokenizing logging backend, the generated format macro evaluates to
``PW_TOKEN_FMT(::namespace::Enum)``, and :cc:`PW_LOG_ENUM` resolves to
:cc:`pw::tokenizer::EnumToToken`, logging the 32-bit token. When using a
standard string-based logging backend, the format macro yields the string format
specifier ``%s``, and :cc:`PW_LOG_ENUM` resolves to :cc:`pw::EnumToString`,
which yields the string representation.

Example
-------
.. literalinclude:: examples/basic_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-basic-cc-log]
   :end-before: [pw_enum-examples-basic-cc-log]

Logging versioned enums
=======================
If you need to log versioned enums using nested tokenization (so that the enum's
unique domain and its value are logged together to prevent collisions and support
reliable detokenization), you can use the versioned logging macro:

* **Versioned argument macro**: Include :cs:`pw_log/tokenized_args.h` and use
  :cc:`PW_LOG_ENUM_VERSIONED` as the argument to the log statement,
  paired with the format specifier :cc:`PW_LOG_ENUM_VERSIONED_FMT`.

When using a tokenizing logging backend, the format macro evaluates to a nested
token format specifier (e.g. ``"${$#%08" PRIx32 "}#%08" PRIx32``), and
:cc:`PW_LOG_ENUM_VERSIONED` expands to two arguments: the 32-bit versioned
domain token and the 32-bit enum value token. On non-tokenizing backends, it
yields the string format specifier ``%s::%s`` and resolves to the enum's domain name
string followed by its stringified value.

Example
-------
.. literalinclude:: examples/basic_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-versioned-cc-log]
   :end-before: [pw_enum-examples-versioned-cc-log]

Build integration
=================
``pw_enum`` provides build integration for Bazel, GN, and CMake.

.. tab-set::

   .. tab-item:: Bazel

      Use the ``pw_cc_enum_library`` rule from :cs:`//pw_enum:pw_cc_enum_library.bzl`.

      .. literalinclude:: BUILD.bazel
         :language: python
         :start-after: [pw_enum-basic-bazel]
         :end-before: [pw_enum-basic-bazel]

   .. tab-item:: GN

      Use the ``pw_cc_enum_source_set`` template from :cs:`"$dir_pw_enum/pw_cc_enum_source_set.gni"
      <//pw_enum:pw_cc_enum_source_set.gni>`.

      .. literalinclude:: BUILD.gn
         :start-after: [pw_enum-basic-gn]
         :end-before: [pw_enum-basic-gn]

   .. tab-item:: CMake

      Use the ``pw_add_enum_library`` function from :cs:`pw_enum/pw_cc_enum_library.cmake
      <//pw_enum:pw_cc_enum_library.cmake>`.

      .. literalinclude:: CMakeLists.txt
         :language: cmake
         :start-after: [pw_enum-basic-cmake]
         :end-before: [pw_enum-basic-cmake]

.. _module-pw_enum-toml:
.. _module-pw_enum-json:

----------------------
Language-neutral enums
----------------------
Enums can be defined in language-neutral TOML (``.toml``) or JSON (``.json``)
files, from which ``pw_enum`` generates C++ and Python code. Use TOML or JSON
when an enum's single source of truth must be shared across languages, such as
an enum used by firmware and host tools. TOML is preferred for human-authored
files because it supports comments, hex and binary integer literals, and a more
concise syntax; JSON is also supported using the same structure. For enums that
are only used in C++, prefer standard C++ headers.

TOML- and JSON-defined enums get the same C++ features as header-defined enums:
:cc:`pw::EnumToString`, :cc:`pw::EnumTraits`, versioned tokenization, ``*_FMT``
and ``*_DOMAIN`` macros, and optional status classes.

Example
=======
Define enums in a TOML file:

.. literalinclude:: examples/enum_example/sensor.toml
   :language: toml
   :start-after: # the License.

Declare a ``pw_enum`` target for the file, and generate code for it with
``pw_cc_enum_library`` and ``pw_py_enum_library``:

.. literalinclude:: examples/BUILD.bazel
   :language: bazel
   :start-after: [pw_enum-examples-toml-bazel]
   :end-before: [pw_enum-examples-toml-bazel]

The generated C++ header is included at the source file's path, with a ``.h``
extension:

.. literalinclude:: examples/toml_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-toml-cc]
   :end-before: [pw_enum-examples-toml-cc]

.. literalinclude:: examples/toml_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-toml-cc-usage]
   :end-before: [pw_enum-examples-toml-cc-usage]

The generated Python module is imported at the source file's path. Each enum is
a standard ``enum.IntEnum`` that only depends on the Python standard library and
is fully understood by type checkers.

.. literalinclude:: examples/toml_enum_test.py
   :language: py
   :start-after: [pw_enum-examples-toml-py]
   :end-before: [pw_enum-examples-toml-py]

.. literalinclude:: examples/toml_enum_test.py
   :language: py
   :dedent: 8
   :start-after: [pw_enum-examples-toml-py-usage]
   :end-before: [pw_enum-examples-toml-py-usage]

File format
===========
Each TOML or JSON file defines one or more enums in a namespace. Both formats
use the same schema. Unknown fields are errors.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Field
     - Description
   * - ``namespace``
     - Required. Dot-separated namespace for the file's enums (e.g.
       ``my.pkg``). Sets the C++ namespace (``my::pkg``) and qualifies
       references from other files.
   * - ``cc.namespace``
     - Optional. Overrides the C++ namespace (e.g. ``my::other_pkg``).
   * - ``enums.<Name>``
     - Required. Table/object mapping each ``PascalCase`` enum name to its
       definition. The enum name is used in every language.
   * - ``enums.<Name>.type``
     - Optional. Underlying type: ``int8``, ``int16``, ``int32``, ``int64``,
       ``uint8``, ``uint16``, ``uint32``, or ``uint64``. Defaults to
       ``int32``. Values must fit in the type.
   * - ``enums.<Name>.doc``
     - Optional. Documentation, emitted as a C++ comment and Python docstring.
       Lines cannot end with a backslash, which would continue the C++ comment
       onto the next line.
   * - ``enums.<Name>.status``
     - Optional. Makes the enum a status. ``name`` is the ``PascalCase``
       status name, which must differ from the enum name, and ``ok`` names the
       OK enumerator. See `Statuses`_.
   * - ``enums.<Name>.allow_alias``
     - Optional. Allows enumerators to share values without explicitly
       aliasing each other. Defaults to ``false``. See `Aliases`_.
   * - ``enums.<Name>.values``
     - Required. List of enumerators (in TOML, ``values = [...]`` or
       ``[[enums.<Name>.values]]``).
   * - ``values[].name``
     - Required. ``UPPER_SNAKE_CASE`` enumerator name. Used as the Python name
       and the display name, and converted to ``kPascalCase`` for C++.
   * - ``values[].value``
     - Optional. An integer or an expression string. Defaults to the previous
       enumerator's value plus one, or ``0`` for the first enumerator.
   * - ``values[].display_name``
     - Optional. Overrides the stringified and tokenized name. Display names
       must be printable, cannot contain ``|``, and must be unique among
       enumerators with different values.
   * - ``values[].doc``
     - Optional. Documentation for the enumerator. Lines cannot end with a
       backslash.
   * - ``values[].cc.name``
     - Optional. Overrides the C++ enumerator name.

Value expressions support integer literals (``10``, ``0x1F``, ``0b101``),
parentheses, unary ``- + ~``, and binary ``+ - * << >> | & ^``. Expressions may
reference enumerators by name:

* ``ALPHA``: an enumerator in the same enum.
* ``Other.ALPHA``: an enumerator in an enum in the same namespace.
* ``my.pkg.Other.ALPHA``: a fully qualified enumerator.

Referenced enums must be defined in the same ``pw_enum`` target or in one of its
``deps``. Generated code always uses the resolved integer values, so every
language sees identical values. TOML and JSON enums cannot reference C++
constants; use header-defined enums for that. Header-defined enums can
reference TOML- or JSON-defined enums by including their generated headers.

Aliases
=======
Like Protocol Buffers, ``pw_enum`` rejects enumerators that share a value,
since duplicate values are easy to create by accident (e.g. with implicit
values). Aliases share a token, and their display names are joined with ``|``
(e.g. ``"ALIAS_SECOND|SECOND"``).

To define an alias, set its ``value`` to the name of another enumerator in the
same enum:

.. code-block:: toml

   [enums.MyEnum]
   values = [
     { name = "SECOND", value = 2 },
     { name = "ALIAS_SECOND", value = "SECOND" },
   ]

To allow enumerators to share values in other ways, such as through
expressions or implicit values, set ``allow_alias = true`` on the enum.

Statuses
========
An enum with a ``status`` describes the outcome of an operation, with one
enumerator for success:

.. code-block:: toml

   [enums.SensorErrorCode]
   status = { name = "SensorStatus", ok = "OK" }
   values = [
     { name = "OK" },
     { name = "NOT_READY" },
   ]

Each language decides how to represent the status and its codes:

* C++ generates the ``enum class SensorErrorCode`` and a ``SensorStatus``
  class that wraps it, exactly like ``PW_ENUM_STATUS`` (see `Status enums`_).
* Python does not distinguish between a status and its code, so it only
  generates ``class SensorStatus(enum.IntEnum)``. There is no
  ``SensorErrorCode`` class. ``SensorStatus`` has an ``ok()`` method, which
  returns whether it is the OK enumerator.

An enumerator named ``OK`` must be the OK enumerator or an alias of it, since it
would otherwise conflict with ``ok()``.

Names and paths
===============
A source file's path determines where its generated code lives in every
language. Its ``namespace`` only affects languages with namespaces inside
files, such as C++.

.. list-table::
   :header-rows: 1

   * - Concept
     - TOML / JSON
     - C++
     - Python
   * - File
     - ``bar/baz/foo.toml`` (or ``.json``)
     - ``#include "bar/baz/foo.h"``
     - ``import bar.baz.foo``
   * - Namespace
     - ``"my.pkg"``
     - ``namespace my::pkg``
     - (none; the module path is the namespace)
   * - Enum
     - ``[enums.MyEnum]``
     - ``enum class MyEnum``
     - ``class MyEnum(enum.IntEnum)``
   * - Status
     - ``status = { name = "MyStatus", ... }``
     - ``class MyStatus`` wrapping ``enum class MyEnum``
     - ``class MyStatus(enum.IntEnum)`` (no ``MyEnum``)
   * - Enumerator
     - ``IO_ERROR``
     - ``kIoError``
     - ``IO_ERROR``
   * - Display name
     - ``display_name`` or ``name``
     - :cc:`pw::EnumToString`
     - ``str()``
   * - Token domain
     - (derived)
     - ``EnumTraits<MyEnum>::kTokenDomain``
     - ``MyEnum.__pw_enum_token_domain__``

By default, paths are relative to the repository root in Bazel, the GN root
(``//``) in GN, and ``CMAKE_SOURCE_DIR`` in CMake. To change the path of the
generated code, set the prefix options on the ``pw_enum`` target:

* Bazel: ``strip_import_prefix`` and ``import_prefix``, which work like the
  attributes of the same names on ``proto_library``.
* GN: ``strip_prefix`` and ``prefix``.
* CMake: ``STRIP_PREFIX`` and ``PREFIX``.

For example, stripping ``public`` makes ``public/bar/foo.toml`` generate
``#include "bar/foo.h"`` and ``import bar.foo``. In GN and CMake, the default
paths depend on where the project is checked out, so enums that are used across
projects or build systems should always strip a prefix.

If generated Python modules are part of a package that is also defined
elsewhere (e.g. in a ``py/`` directory), extend the package's path in its
``__init__.py``:

.. code-block:: py

   __path__ = __import__('pkgutil').extend_path(__path__, __name__)

A TOML- or JSON-defined enum has the same token domain as a header-defined enum
with the same C++ name, enumerator values, and display names. Enums can move
between TOML/JSON and C++ headers without changing their tokens.

Build integration
=================
.. tab-set::

   .. tab-item:: Bazel

      * ``pw_enum`` from :cs:`//pw_enum:pw_enum.bzl` declares TOML or JSON enum
        sources. ``deps`` lists other ``pw_enum`` targets that the sources
        reference.
      * ``pw_cc_enum_library`` from :cs:`//pw_enum:pw_cc_enum_library.bzl`
        generates C++ headers for the ``pw_enum`` targets in ``enums``.
        ``enums`` may be combined with ``enum_headers``, ``hdrs``, and
        ``srcs``, which may include the generated headers.
      * ``pw_py_enum_library`` from :cs:`//pw_enum:pw_py_enum_library.bzl`
        generates Python modules for the ``pw_enum`` targets in ``enums``.

      Code is generated once per ``pw_enum`` target, no matter how many
      libraries reference it.

   .. tab-item:: GN

      * ``pw_enum`` from :cs:`"$dir_pw_enum/pw_enum.gni" <//pw_enum:pw_enum.gni>`
        declares TOML or JSON enum sources. It generates C++ code in the
        ``<name>.cc`` subtarget and Python code in the ``<name>.python``
        subtarget, a ``pw_python_package``. Building ``<name>`` validates the
        sources.
      * ``pw_cc_enum_source_set`` accepts ``pw_enum`` targets in ``enums``,
        which may be combined with ``enum_headers``.

      .. literalinclude:: BUILD.gn
         :start-after: [pw_enum-json-gn]
         :end-before: [pw_enum-json-gn]

   .. tab-item:: CMake

      * ``pw_add_enum`` from :cs:`pw_enum/pw_enum.cmake <//pw_enum:pw_enum.cmake>`
        declares TOML or JSON enum sources and generates C++ code in the
        ``<name>.cc`` library.
      * ``pw_add_enum_library`` accepts ``pw_add_enum`` targets in ``ENUMS``,
        which may be combined with ``ENUM_HEADERS``.

      Python code is not generated in CMake.

      .. literalinclude:: CMakeLists.txt
         :language: cmake
         :start-after: [pw_enum-json-cmake]
         :end-before: [pw_enum-json-cmake]

------------------
Stringifying enums
------------------
:cc:`pw::EnumToString` returns a string version of an enum. It uses a FTADLE
extension point ``PwEnumToString(enum)``. FTADLE is a pattern that enables
customization by searching for a matching function via Argument-Dependent Lookup
(ADL). For more information, see `Designing Extension Points With FTADLE
<https://abseil.io/tips/218>`_.

If you don't use the build-time generation targets, you can manually use :cc:`PW_TOKENIZE_ENUM`
in :cs:`pw_tokenizer/enum.h <pw_tokenizer/public/pw_tokenizer/enum.h>` to
tokenize the enum and implement ``PwEnumToString``.

--------------------------
Enum traits and validation
--------------------------
Enums registered with ``PW_ENUM`` automatically specialize :cc:`pw::EnumTraits`
(defined in :cs:`pw_enum/traits.h <pw_enum/public/pw_enum/traits.h>`).
``pw::EnumTraits<EnumType>`` provides compile-time metadata about the enum,
including its names, values, and an ``IsValid`` function.  See
:cc:`pw::EnumTraits` for the full list of members.

.. literalinclude:: examples/traits.cc
   :language: cpp
   :start-after: [pw_enum-examples-traits]
   :end-before: [pw_enum-examples-traits]

Use :cc:`pw::has_enum_traits_v` to detect whether an enum has traits, and
:cc:`pw::IsValidEnum` to validate a value:

.. literalinclude:: examples/traits.cc
   :language: cpp
   :start-after: [pw_enum-examples-traits-validate]
   :end-before: [pw_enum-examples-traits-validate]

.. note::

   Do not specialize :cc:`pw::EnumTraits` by hand. Specializations are
   identified by an internal tag that only the code generator emits, so a
   hand-written one is not recognized by :cc:`pw::has_enum_traits_v` and is
   rejected by :cc:`pw::IsValidEnum`. Register the enum with ``PW_ENUM``
   instead.

``IsValid`` accepts an enumerator or an integer of any type, but deliberately
rejects ``bool``, since nearly everything converts implicitly to ``bool`` and
would silently validate as ``0`` or ``1``. This is unrelated to the enum's
underlying type: an ``enum class E : bool`` is supported like any other enum.

-----------------
Enum domain names
-----------------
:cc:`pw::PwEnumDomainName` returns a fallback string domain name of a given
enum type, used by non-tokenizing logging backends to prefix versioned enum
logs.

By default, this function returns ``"Enum"``. If an enum uses a custom
tokenization domain and you want non-tokenizing logging backends to prefix the
enum values with that custom domain name instead of ``"Enum"``, you can provide
a custom template specialization for the enum type:

.. code-block:: cpp

   #include "pw_enum/to_string.h"

   template <>
   constexpr const char* pw::PwEnumDomainName<my::CustomEnum>() {
     return "CustomDomain";
   }

------------------
Enum domain tokens
------------------
``pw::tokenizer::EnumDomainToken`` returns the 32-bit tokenization domain token of a
given enum type. It relies on the primary template ``pw::tokenizer::PwEnumDomainToken<T>()``.

To get the domain token for an enum:

.. code-block:: cpp

   #include "pw_tokenizer/enum.h"

   constexpr uint32_t domain = pw::tokenizer::EnumDomainToken<MyEnum>();

If the enum is versioned using ``pw_cc_enum_source_set``, this returns its unique versioned
domain token (hash). If the enum has not been tokenized or specialized, attempting to get
its domain token will result in a compile-time error.

----------------------
Cross language support
----------------------
Enums defined in TOML or JSON generate C++ and Python code (see
:ref:`module-pw_enum-toml`). Header-defined enums currently only generate C++.
The header parser extracts the full enum definition and resolves all enumerator
values, so it could also be used to generate code for other languages.

----------
Background
----------
:ref:`module-pw_tokenizer` is one of Pigweed's most widely adopted features. It
has supported nested tokenization---a tokenized message inside another tokenized
message---since the early days. Initially, only Base64-encoded messages were
supported, which is inefficient. Support for directly encoding nested messages
as 32-bit integers was added later (see :ref:`seed-0105`).

With support for encoding tokens as integers, supporting rich enums was a clear
next step. This culminated in the creation of :cs:`pw_tokenizer/enum.h
<pw_tokenizer/public/pw_tokenizer/enum.h>` and its supporting macros. This
approach uses the enum's integral value as a nested token, discriminated by its
namespace to avoid collisions between different enum types. The result is
highly efficient enum logs that are still readable and user-friendly.

The need for versioning
=======================
Real-world deployment of :cs:`pw_tokenizer/enum.h
<pw_tokenizer/public/pw_tokenizer/enum.h>` soon revealed a critical flaw. When
enum values were changed or reordered during development, the resulting tokens
changed. When merging token databases from different builds, this led to
collisions, where the same token mapped to different string representations. It
became clear that enum tokenization required versioning.

Several alternatives were explored for automatic enum versioning. A key
constraint was that enum values must be able to be set with expressions, which
may reference constants or other enum values. Approaches considered included:

* **Tokenize names instead of values**: Hash the enumerator names and generate
  a function with a switch statement to map values to tokens at runtime.
* **Version in the domain**: Incorporate a hash of the enum's contents (names
  and values) into the tokenization domain, requiring two arguments to log an
  enum (the version and the value).
* **Calculate tokens from a base**: This approach used a hash of the enum's
  contents as a base offset, adding the enum value to it at the call site.

Ultimately, these approaches were ruled out because they increased code size
relative to the existing implementation, primarily due to the additional code
required at the call site.

The code size penalty could be avoided if there were a ``constexpr`` way to
insert the enum's version into the log format string. Then, the existing token
logging macros could be used (``PW_LOG_FMT``). For example:

.. code-block:: cpp

   // If there were a way to define this macro during compilation, versioned
   // enums would have no code size cost relative to unversioned enums.
   #define MY_ENUM_FMT PW_LOG_FMT("::my::Enum::version_1234")

   PW_LOG_INFO("My enum: " MY_ENUM_FMT, PW_LOG_ENUM(my_enum))

Unfortunately, there is no way get the versioned enum domain into a
concatenatable string literal. This is required for compatibility with
``pw_log``'s C-style API. If ``pw_log`` offered a C++-only API, this would be
feasible, but adding such an API was out of scope.

Generating enums
================
Generating enums appeared to be the only way to get the enum's version into a
string literal at compile time. This led to the creation of ``pw_enum``.

JSON definition
---------------
The initial implementation of ``pw_enum`` generated C++ headers from JSON files.
While this worked well technically, it proved too difficult for projects to
adopt due to the friction of maintaining JSON definitions for standard C++
enums. Protocol Buffers were considered in place of JSON, but they are too
limited for this use case. Protobufs do not support setting enumerator values
based on other enums or external constants.

Language-neutral TOML and JSON definitions later returned as an opt-in
alternative to C++ headers, for enums that must be shared across languages (see
:ref:`module-pw_enum-toml`).

Parse C++ source
----------------
Finally, multiple approaches for parsing enum definitions out of C++ source code
were explored. These included:

- Use `libclang <https://clang.llvm.org/docs/LibClang.html>`_ from Python to
  parse header files. This would be robust and even perform ``constexpr``
  evaluation of enumerator values. Unfortunately, ``libclang`` is a large
  dependency, and is not readily available on all platforms.
- Parse ``clang``'s `-ast-dump <https://clang.llvm.org/docs/IntroductionToTheClangAST.html#examining-the-ast>`_
  output. This would be fairly robust, but would involve parsing moderately
  complex, non-standard text output intended for human consumption. It also
  requires ``clang``, which not all projects build with.
- Use a custom Python parser. This approach would be utterly impractical and
  brittle.

Ultimately, parsing C++ source directly proved infeasible. The final design
avoids parsing arbitrary C++ source with the :cc:`PW_ENUM(name, ...) <PW_ENUM>`
macro.

------
Design
------

The final design of ``pw_enum`` addresses the constraints identified during its
evolution by combining standard C++ header files with a specialized build-time
generator powered by compile-time template evaluation.

This architecture provides a seamless user experience with zero runtime overhead
and robust protection against database collisions, while maintaining the full
expressiveness of standard C++ enum definitions.

Source files
============
Users define enums in standard C++ header files. To opt-in to ``pw_enum``, the
header includes :cs:`pw_enum/generate.h` and registers each enum by calling the
:cc:`PW_ENUM(...) <PW_ENUM>` macro with the enum name and names of all of its
enumerators.

The macro's primary purpose is to capture enum metadata in an easily parsable
format. ``PW_ENUM()`` expands to the enum name and list of enumerators,
surrounded by unique markers. A :cs:`Python script
<pw_enum/py/pw_enum/parse.py>` searches a preprocessed source file for the
markers and extracts the enum metadata.

The macro also serves to require that users list the file in a versioned enum
target (see `Build system integration`_), which is necessary for it to be
processed. If the file is not processed by ``pw_enum`` machinery, the macro
expands to ``static_assert(false)``, causing the build to fail with an
informative error.

Enumerator evaluation
=====================
Enumerator values can be defined by arbitrary C++ expressions. The values may change,
even if the individual source file does not change.

The :cs:`parse.py <pw_enum/py/pw_enum/parse.py>` script evaluates enumerators
generating a source file that references them. The source file instantiates a
template with the enumerators as template arguments. Compilation fails, but
includes the enumerator values in an easily parsable form.

This solution is far from ideal, but has proven to be robust. It evaluates
enumerators with the same toolchain as the rest of the project. Printing
compile-time constants with failed template instantiations is a common
workaround to achieve compile-time "printf" functionality. The script searches
for a unique template name and doesn't depend on a particular compiler or
version.

Build-time generation
=====================
After parsing, the versioned enum target runs a Python script
(:cs:`pw_enum/py/pw_enum/generate.py`) that generates a header.

1. **Enum generation**: The script generates a "shadowed" version of the header
   in the build directory. This generated header contains the original content,
   plus a footer with tokenization metadata. It also replaces the
   ``PW_ENUM(...)`` calls with ``_PW_ENUM_GENERATED(...)``. ``PW_ENUM(...)``
   expands to ``static_assert(false)`` to require users to build headers with
   versioned enum targets.
2. **Versioning**: A unique version hash is calculated for each enum based on
   its fully qualified name and the names and values of all its enumerators.
   This hash is used to construct a unique tokenization domain (e.g.,
   ``::namespace::_pw_enum_HASH::EnumName``). This ensures that if the enum
   changes, the domain changes, preventing collisions in merged token databases.
3. **Tokenization**: The generated footer includes a call to
   :cc:`PW_TOKENIZE_ENUM_CUSTOM` from :ref:`module-pw_tokenizer`, which
   registers the enum values and their string representations in the database.
4. **Enum traits**: The generated footer defines a specialization of
   :cc:`pw::EnumTraits` providing compile-time metadata and validation for the
   enum. Enumerators that share a value (aliases) count as a single *distinct
   value*. See `Enum traits and validation`_.

Build system integration
========================
The versioned enum build rules automate the process of parsing C++ headers and
generating versioned enum metadata. It invokes the ``pw_enum`` generator with
the correct compilation flags and ensures the generated headers are prioritized
during compilation.

* **Bazel** (:cs:`pw_cc_enum_library.bzl <//pw_enum:pw_cc_enum_library.bzl>`) creates an
  internal library target to collect the compilation flags (includes, defines)
  required to parse the header correctly and passes them to the Python script.
* **CMake** (:cs:`pw_cc_enum_library.cmake <//pw_enum:pw_cc_enum_library.cmake>`) creates an
  internal interface library to collect includes and defines from dependencies,
  and uses ``file(GENERATE)`` to produce a flags file for the generator. It
  uses ``-iquote`` to ensure that the build system prioritizes the generated
  shadowed header over the original source header.
* **GN** (:cs:`pw_cc_enum_source_set.gni <//pw_enum:pw_cc_enum_source_set.gni>`) compiles a
  placeholder C++ file with the enum's dependencies to generate a target Ninja
  file. The generator script parses the target Ninja file and the toolchain's
  Ninja file to extract the compiler and its compilation flags (defines,
  includes, and flags) to run the evaluation step. This is similar to how
  :ref:`module-pw_compilation_testing` works. Like CMake, the GN build uses
  ``-iquote`` to ensure that the build system prioritizes the generated shadowed
  header over the original source header.

Language-neutral enums
======================
TOML- and JSON-defined enums follow the same architecture as Protocol Buffers: a
language-neutral source target, and language-specific rules that generate code
from it.

1. **Resolution**: :cs:`pw_enum/py/pw_enum/json_source.py` validates TOML and
   JSON sources and resolves every enumerator value, including references to
   other enums, into the same in-memory representation that the C++ header
   parser produces. Resolution is pure Python and does not require a compiler.
   Sources may only reference enums in their own target or its direct ``deps``.
2. **Generation**: Language-specific generators
   (:cs:`pw_enum/py/pw_enum/generate.py` and
   :cs:`pw_enum/py/pw_enum/generate_py.py`) generate code from the resolved
   enums. Every language uses the same resolution code, so values, display
   names, and versions match across languages. The C++ generator emits the enum
   definition followed by the same footer that header-defined enums get.
3. **Build integration**: In Bazel, ``pw_cc_enum_library`` and
   ``pw_py_enum_library`` attach aspects to their ``enums``, like
   ``cc_proto_library`` and ``py_proto_library``. Each aspect resolves the enum
   sources and generates code in a single action, once per ``pw_enum`` target,
   which avoids duplicate headers when several libraries reference the same
   enums. ``pw_enum`` targets also run a validation action, so building one
   reports errors without generating code. In GN and CMake, which lack
   aspects, the ``pw_enum`` template or function itself declares the code
   generation targets. GN collects the enum sources of dependencies with
   metadata, and CMake with generator expressions.

Python enums are generated as static source rather than created dynamically
from TOML or JSON at import time. Type checkers cannot see the members of
dynamically created enums. Protobuf's Python code faces the same problem and
requires separate ``.pyi`` stubs.
