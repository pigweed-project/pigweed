:tocdepth: 3

.. _module-pw_tokenizer-tokenization:

============
Tokenization
============
.. pigweed-module-subpage::
   :name: pw_tokenizer

Tokenization converts a string literal to a token. If it's a printf-style
string, its arguments are encoded along with it. The results of tokenization can
be sent off device or stored in place of a full string.

--------
Concepts
--------
See :ref:`module-pw_tokenizer-get-started-overview` for a high-level
explanation of how ``pw_tokenizer`` works.

Token generation: compile time hashing
======================================
String tokens are generated using a modified version of the x65599 hash used by
the SDBM project. All hashing is done at compile time.

In C code, strings are hashed with a preprocessor macro. For compatibility with
macros, the hash must be limited to a fixed maximum number of characters. This
value is set by ``PW_TOKENIZER_CFG_C_HASH_LENGTH``. Increasing
``PW_TOKENIZER_CFG_C_HASH_LENGTH`` increases the compilation time for C due to
the complexity of the hashing macros.

C++ macros use a constexpr function instead of a macro. This function works with
any length of string and has lower compilation time impact than the C macros.
For consistency, C++ tokenization uses the same hash algorithm, but the
calculated values will differ between C and C++ for strings longer than
``PW_TOKENIZER_CFG_C_HASH_LENGTH`` characters.

Token encoding
==============
The token is a 32-bit hash calculated during compilation. The string is encoded
little-endian with the token followed by arguments, if any. For example, the
31-byte string ``You can go about your business.`` hashes to 0xdac9a244.
This is encoded as 4 bytes: ``44 a2 c9 da``.

Arguments are encoded as follows:

* **Integers**  (1--10 bytes) --
  `ZagZag and varint encoded <https://developers.google.com/protocol-buffers/docs/encoding#signed-ints>`_,
  similarly to Protocol Buffers. Smaller values take fewer bytes.
* **Floating point numbers** (4 bytes) -- Single precision floating point.
* **Strings** (1--128 bytes) -- Length byte followed by the string contents.
  The top bit of the length whether the string was truncated or not. The
  remaining 7 bits encode the string length, with a maximum of 127 bytes.

.. TODO(hepler): insert diagram here!

.. tip::
   ``%s`` arguments can quickly fill a tokenization buffer. Keep ``%s``
   arguments short or avoid encoding them as strings (e.g. encode an enum as an
   integer instead of a string). See also
   :ref:`module-pw_tokenizer-nested-arguments`.

.. _module-pw_tokenizer-proto:

Tokenized fields in protocol buffers
====================================
Text may be represented in a few different ways:

- Plain ASCII or UTF-8 text (``This is plain text``)
- Base64-encoded tokenized message (``$ibafcA==``)
- Binary-encoded tokenized message (``89 b6 9f 70``)
- Little-endian 32-bit integer token (``0x709fb689``)

``pw_tokenizer`` provides the ``pw.tokenizer.format`` protobuf field option.
This option may be applied to a protobuf field to indicate that it may contain a
tokenized string. A string that is optionally tokenized is represented with a
single ``bytes`` field annotated with ``(pw.tokenizer.format) =
TOKENIZATION_OPTIONAL``.

For example, the following protobuf has one field that may contain a tokenized
string.

.. code-block:: protobuf

   import "pw_tokenizer_proto/options.proto";

   message MessageWithOptionallyTokenizedField {
     bytes just_bytes = 1;
     bytes maybe_tokenized = 2 [(pw.tokenizer.format) = TOKENIZATION_OPTIONAL];
     string just_text = 3;
   }

-----------------------
Tokenization in C++ / C
-----------------------
To tokenize a string, include ``pw_tokenizer/tokenize.h`` and invoke one of the
``PW_TOKENIZE_*`` macros.

Tokenize string literals outside of expressions
===============================================
``pw_tokenizer`` provides macros for tokenizing string literals with no
arguments:

* :cc:`PW_TOKENIZE_STRING`
* :cc:`PW_TOKENIZE_STRING_DOMAIN`
* :cc:`PW_TOKENIZE_STRING_MASK`

The tokenization macros above cannot be used inside other expressions.

.. admonition:: **Yes**: Assign :cc:`PW_TOKENIZE_STRING` to a ``constexpr`` variable.
  :class: checkmark

  .. code-block:: cpp

     constexpr uint32_t kGlobalToken = PW_TOKENIZE_STRING("Wowee Zowee!");

     void Function() {
       constexpr uint32_t local_token = PW_TOKENIZE_STRING("Wowee Zowee?");
     }

.. admonition:: **No**: Use :cc:`PW_TOKENIZE_STRING` in another expression.
  :class: error

  .. code-block:: cpp

     void BadExample() { ProcessToken(PW_TOKENIZE_STRING("This won't compile!")); }

  Use :cc:`PW_TOKENIZE_STRING_EXPR` instead.

Tokenize inside expressions
===========================
An alternate set of macros are provided for use inside expressions. These make
use of lambda functions, so while they can be used inside expressions, they
require C++ and cannot be assigned to constexpr variables or be used with
special function variables like ``__func__``.

* :cc:`PW_TOKENIZE_STRING_EXPR`
* :cc:`PW_TOKENIZE_STRING_DOMAIN_EXPR`
* :cc:`PW_TOKENIZE_STRING_MASK_EXPR`

.. admonition:: When to use these macros

  Use :cc:`PW_TOKENIZE_STRING` and related macros to tokenize string
  literals that do not need %-style arguments encoded.

.. admonition:: **Yes**: Use :cc:`PW_TOKENIZE_STRING_EXPR` within other expressions.
  :class: checkmark

  .. code-block:: cpp

     void GoodExample() {
       ProcessToken(PW_TOKENIZE_STRING_EXPR("This will compile!"));
     }

.. admonition:: **No**: Assign :cc:`PW_TOKENIZE_STRING_EXPR` to a ``constexpr`` variable.
  :class: error

  .. code-block:: cpp

     constexpr uint32_t wont_work = PW_TOKENIZE_STRING_EXPR("This won't compile!"));

  Instead, use :cc:`PW_TOKENIZE_STRING` to assign to a ``constexpr`` variable.

.. admonition:: **No**: Tokenize ``__func__`` in :cc:`PW_TOKENIZE_STRING_EXPR`.
  :class: error

  .. code-block:: cpp

     void BadExample() {
       // This compiles, but __func__ will not be the outer function's name, and
       // there may be compiler warnings.
       constexpr uint32_t wont_work = PW_TOKENIZE_STRING_EXPR(__func__);
     }

  Instead, use :cc:`PW_TOKENIZE_STRING` to tokenize ``__func__`` or similar macros.

Tokenize a message with arguments to a buffer
=============================================
* :cc:`PW_TOKENIZE_TO_BUFFER`
* :cc:`PW_TOKENIZE_TO_BUFFER_DOMAIN`
* :cc:`PW_TOKENIZE_TO_BUFFER_MASK`

.. admonition:: Why use this macro

   - Encode a tokenized message for consumption within a function.
   - Encode a tokenized message into an existing buffer.

   Avoid using ``PW_TOKENIZE_TO_BUFFER`` in widely expanded macros, such as a
   logging macro, because it will result in larger code size than passing the
   tokenized data to a function.

.. _module-pw_tokenizer-nested-arguments:

Tokenize nested arguments
=========================
Encoding ``%s`` string arguments is inefficient, since ``%s`` strings are
encoded 1:1, with no tokenization. Tokens can therefore be used to replace
string arguments to tokenized format strings.

* :cc:`PW_TOKEN_FMT`

.. admonition:: Logging nested tokens

  Users will typically interact with nested token arguments during logging.
  In this case there is a slightly different interface described by
  :ref:`module-pw_log-tokenized-args` that does not generally invoke
  :cc:`PW_TOKEN_FMT` directly.

The format specifier for a token is given by PRI-style macro ``PW_TOKEN_FMT()``,
which is concatenated to the rest of the format string by the C preprocessor.

.. code-block:: cpp

   PW_TOKENIZE_FORMAT_STRING("margarine_domain",
                             UINT32_MAX,
                             "I can't believe it's not " PW_TOKEN_FMT() "!",
                             PW_TOKENIZE_STRING_EXPR("butter"));

This feature is currently only supported by the Python detokenizer.

Nested token format
-------------------
Nested tokens have the following format within strings:

.. code-block::

   $[{DOMAIN}][BASE#]TOKEN

The ``$`` is a common prefix required for all nested tokens. It is possible to
configure a different common prefix if necessary, but using the default ``$``
character is strongly recommended.

The optional ``DOMAIN`` specifies the token domain. If this option is omitted,
the default (empty) domain is assumed.

The optional ``BASE`` defines the numeric base encoding of the token. Accepted
values are 10, 16, and 64. If the hash symbol ``#`` is used without specifying a
number, the base is assumed to be 16. If the base option is omitted entirely,
the base defaults to 64 for backward compatibility. All encodings except Base64
are not case sensitive. Other bases may be supported in the future.

Non-Base64 tokens are encoded strictly as 32-bit integers with padding.
Base64 data may additionally encode string arguments for the detokenized token,
and therefore does not have a maximum width.

The meaning of ``TOKEN`` depends on the current phase of transformation for the
current tokenized format string. Within the format string's entry in the token
database, when the actual value of the token argument is not known, ``TOKEN`` is
a printf argument specifier (e.g. ``%08x`` for a base-16 token with correct
padding). The actual tokens that will be used as arguments have separate
entries in the token database.

After the top-level format string has been detokenized and formatted, ``TOKEN``
should be the value of the token argument in the specified base, with any
necessary padding. This is the final format of a nested token if it cannot be
tokenized.

.. list-table:: Example tokens
   :widths: 10 25 25

   * - Base
     - | Token database
       | (within format string entry)
     - Partially detokenized
   * - 10
     - ``$10#%010d``
     - ``$10#0086025943``
   * - 16
     - ``$#%08x``
     - ``$#0000001A``
   * - 64
     - ``%s``
     - ``$QA19pfEQ``

.. _module-pw_tokenizer-custom-macro:

Tokenizing enums
================
Logging enums is one common special case where tokenization is particularly
appropriate: enum values are conceptually already tokens mapping to their
names, assuming no duplicate values.

:cc:`PW_TOKENIZE_ENUM` will take in a fully qualified enum name along with all
of the associated enum values. This macro will create database entries that
include the domain name (fully qualified enum name), enum value, and a tokenized
form of the enum value.

The macro also supports returing the string version of the enum value in the
case that there is a non-tokenizing backend, using
:cc:`EnumToString <pw::tokenizer::EnumToString>`.

All enum values in the enum declaration must be present in the macro, and the
macro must be in the same namespace as the enum to be able to use the
:cc:`EnumToString <pw::tokenizer::EnumToString>` function and avoid compiler errors.

.. literalinclude: enum_test.cc
   :language: cpp
   :start-after: [pw_tokenizer-examples-enum]
   :end-before: [pw_tokenizer-examples-enum]

:cc:`PW_TOKENIZE_ENUM_CUSTOM` is an alternative version of
:cc:`PW_TOKENIZE_ENUM` to tokenized a custom strings instead of a
stringified form of the enum value name. It will take in a fully qualified enum
name along with all the associated enum values and custom string for these
values. This macro will create database entries that include the domain name
(fully qualified enum name), enum value, and a tokenized form of the custom
string for the enum value.

.. literalinclude: enum_test.cc
   :language: cpp
   :start-after: [pw_tokenizer-examples-enum-custom]
   :end-before: [pw_tokenizer-examples-enum-custom]

Tokenize a message with arguments in a custom macro
===================================================
Projects can leverage the tokenization machinery in whichever way best suits
their needs. The most efficient way to use ``pw_tokenizer`` is to pass tokenized
data to a global handler function. A project's custom tokenization macro can
handle tokenized data in a function of their choosing. The function may accept
any arguments, but its final arguments must be:

* The 32-bit token (:cc:`pw_tokenizer_Token`)
* The argument types (``pw_tokenizer_ArgTypes``)
* Variadic arguments, if any

``pw_tokenizer`` provides two low-level macros to help projects create custom
tokenization macros:

* :cc:`PW_TOKENIZE_FORMAT_STRING`
* :cc:`PW_TOKENIZER_REPLACE_FORMAT_STRING`

.. caution::

   Note the spelling difference! The first macro begins with ``PW_TOKENIZE_``
   (no ``R``) whereas the second begins with ``PW_TOKENIZER_``.

Use these macros to invoke an encoding function with the token, argument types,
and variadic arguments. The function can then encode the tokenized message to a
buffer using helpers in ``pw_tokenizer/encode_args.h``:

* :cc:`EncodeArgs <pw::tokenizer::EncodeArgs>`
* :cc:`EncodedMessage <pw::tokenizer::EncodedMessage>`
* :cc:`pw_tokenizer_EncodeArgs`

Example
-------
The following example implements a custom tokenization macro similar to
:ref:`module-pw_log_tokenized`.

.. code-block:: cpp

   #include "pw_tokenizer/tokenize.h"

   #ifndef __cplusplus
   extern "C" {
   #endif

   void EncodeTokenizedMessage(uint32_t metadata,
                               pw_tokenizer_Token token,
                               pw_tokenizer_ArgTypes types,
                               ...);

   #ifndef __cplusplus
   }  // extern "C"
   #endif

   #define PW_LOG_TOKENIZED_ENCODE_MESSAGE(metadata, format, ...)               \
     do {                                                                       \
       PW_TOKENIZE_FORMAT_STRING("logs", UINT32_MAX, format, __VA_ARGS__);      \
       EncodeTokenizedMessage(metadata,                                         \
                              PW_TOKENIZER_REPLACE_FORMAT_STRING(__VA_ARGS__)); \
     } while (0)

In this example, the ``EncodeTokenizedMessage`` function would handle encoding
and processing the message. Encoding is done by the
:cc:`EncodedMessage <pw::tokenizer::EncodedMessage>` class or
:cc:`EncodeArgs <pw::tokenizer::EncodeArgs>` function from
``pw_tokenizer/encode_args.h``. The encoded message can then be transmitted or
stored as needed.

.. code-block:: cpp

   #include "pw_log_tokenized/log_tokenized.h"
   #include "pw_tokenizer/encode_args.h"

   void HandleTokenizedMessage(pw::log_tokenized::Metadata metadata,
                               pw::span<std::byte> message);

   extern "C" void EncodeTokenizedMessage(const uint32_t metadata,
                                          const pw_tokenizer_Token token,
                                          const pw_tokenizer_ArgTypes types,
                                          ...) {
     va_list args;
     va_start(args, types);
     pw::tokenizer::EncodedMessage<kLogBufferSize> encoded_message(
         token, types, args);
     va_end(args);

     HandleTokenizedMessage(metadata, encoded_message);
   }

.. admonition:: Why use a custom macro

   - Optimal code size. Invoking a free function with the tokenized data results
     in the smallest possible call site.
   - Pass additional arguments, such as metadata, with the tokenized message.
   - Integrate ``pw_tokenizer`` with other systems.

Length-delimited strings
========================
The ``%.*s`` ``printf`` format specifier is used for length-delimited strings.
The string is passed as two arguments: an ``int`` length followed by a ``const
char*`` string. Unlike ``%s``, the string does not have to be null terminated.

``pw_tokenizer`` does not yet support the ``%.*s`` specifier (see `b/408040194
<http://pwbug.dev/408040194>`_). To tokenize a length-delimited string, make a
local null-terminated copy first. This can be done with a temporary
:cc:`pw::InlineString` sized to fit the string.

.. literalinclude:: tokenize_test.cc
   :language: c++
   :start-after: pw_tokenizer-length-delimited
   :end-before: pw_tokenizer-length-delimited

Tokenizing function names
=========================
The string literal tokenization functions support tokenizing string literals or
constexpr character arrays (``constexpr const char[]``). In GCC and Clang, the
special ``__func__`` variable and ``__PRETTY_FUNCTION__`` extension are declared
as ``static constexpr char[]`` in C++ instead of the standard ``static const
char[]``. This means that ``__func__`` and ``__PRETTY_FUNCTION__`` can be
tokenized while compiling C++ with GCC or Clang.

.. code-block:: cpp

   // Tokenize the special function name variables.
   constexpr uint32_t function = PW_TOKENIZE_STRING(__func__);
   constexpr uint32_t pretty_function = PW_TOKENIZE_STRING(__PRETTY_FUNCTION__);

Note that ``__func__`` and ``__PRETTY_FUNCTION__`` are not string literals.
They are defined as static character arrays, so they cannot be implicitly
concatentated with string literals. For example, ``printf(__func__ ": %d",
123);`` will not compile.

Calculate minimum required buffer size
======================================
See :cc:`MinEncodingBufferSizeBytes <pw::tokenizer::MinEncodingBufferSizeBytes>`.

.. _module-pw_tokenizer-base64-format:

Encoding Base64
===============
The tokenizer encodes messages to a compact binary representation. Applications
may desire a textual representation of tokenized strings. This makes it easy to
use tokenized messages alongside plain text messages, but comes at a small
efficiency cost: encoded Base64 messages occupy about 4/3 (133%) as much memory
as binary messages.

The Base64 format is comprised of a ``$`` character followed by the
Base64-encoded contents of the tokenized message. For example, consider
tokenizing the string ``This is an example: %d!`` with the argument -1. The
string's token is 0x4b016e66.

.. code-block:: text

   Source code: PW_LOG("This is an example: %d!", -1);

    Plain text: This is an example: -1! [23 bytes]

        Binary: 66 6e 01 4b 01          [ 5 bytes]

        Base64: $Zm4BSwE=               [ 9 bytes]

To encode with the Base64 format, add a call to
``pw::tokenizer::PrefixedBase64Encode`` or ``pw_tokenizer_PrefixedBase64Encode``
in the tokenizer handler function. For example,

.. code-block:: cpp

   void TokenizedMessageHandler(const uint8_t encoded_message[],
                                size_t size_bytes) {
     pw::InlineBasicString base64 = pw::tokenizer::PrefixedBase64Encode(
         pw::span(encoded_message, size_bytes));

     TransmitLogMessage(base64.data(), base64.size());
   }

.. _module-pw_tokenizer-masks:

Reduce token size with masking
==============================
``pw_tokenizer`` uses 32-bit tokens. On 32-bit or 64-bit architectures, using
fewer than 32 bits does not improve runtime or code size efficiency. However,
when tokens are packed into data structures or stored in arrays, the size of the
token directly affects memory usage. In those cases, every bit counts, and it
may be desireable to use fewer bits for the token.

``pw_tokenizer`` allows users to provide a mask to apply to the token. This
masked token is used in both the token database and the code. The masked token
is not a masked version of the full 32-bit token, the masked token is the token.
This makes it trivial to decode tokens that use fewer than 32 bits.

Masking functionality is provided through the ``*_MASK`` versions of the macros:

* :cc:`PW_TOKENIZE_STRING_MASK`
* :cc:`PW_TOKENIZE_STRING_MASK_EXPR`
* :cc:`PW_TOKENIZE_TO_BUFFER_MASK`

For example, the following generates 16-bit tokens and packs them into an
existing value.

.. code-block:: cpp

   constexpr uint32_t token =
       PW_TOKENIZE_STRING_MASK("domain", 0xFFFF, "Pigweed!");
   uint32_t packed_word = (other_bits << 16) | token;

Tokens are hashes, so tokens of any size have a collision risk. The fewer bits
used for tokens, the more likely two strings are to hash to the same token. See
:ref:`module-pw_tokenizer-collisions`.

Masked tokens without arguments may be encoded in fewer bytes. For example, the
16-bit token ``0x1234`` may be encoded as two little-endian bytes (``34 12``)
rather than four (``34 12 00 00``). The detokenizer tools zero-pad data smaller
than four bytes. Tokens with arguments must always be encoded as four bytes.

.. _module-pw_tokenizer-domains:

Keep tokens from different sources separate with domains
========================================================
``pw_tokenizer`` supports having multiple tokenization domains. Domains are a
string label associated with each tokenized string. This allows projects to keep
tokens from different sources separate. Potential use cases include the
following:

* Keep large sets of tokenized strings separate to avoid collisions.
* Create a separate database for a small number of strings that use truncated
  tokens, for example only 10 or 16 bits instead of the full 32 bits.

Domain names must meet the following requirements:

- Domain names may only contain alphanumeric characters (``A–Z``, ``a–z``,
  ``0–9``), colon (``:``), or underscore (``_``).
- The first character must not be a digit (``0–9``).
- Whitespace is ignored. Source files may include whitespace in domain names,
  but it is removed from token databases. The following domains are equivalent:
  ``name``, ``n  a m  e``, and ``na\tme\n``.

If no domain is specified, the domain is empty (``""``).

.. code-block:: cpp

   // Tokenizes this string to the default ("") domain.
   PW_TOKENIZE_STRING("Hello, world!");

   // Tokenizes this string to the "my_custom_domain" domain.
   PW_TOKENIZE_STRING_DOMAIN("my_custom_domain", "Hello, world!");

The database and detokenization command line tools default to loading tokens
from all domains. The domain may be specified for ELF files by appending
``#DOMAIN_NAME_REGEX`` to the file path. Use ``#`` to only read from the default
domain. For example, the following reads strings in ``some_domain`` from
``my_image.elf``.

.. code-block:: console

   $ ./database.py create --database my_db.csv "path/to/my_image.elf#some_domain"

See :ref:`module-pw_tokenizer-managing-token-databases` for information about
the ``database.py`` command line tool.

Limitations, bugs, and future work
==================================

.. _module-pw_tokenizer-gcc-template-bug:

GCC bug: tokenization in template functions
-------------------------------------------
GCC releases prior to 14 incorrectly ignore the section attribute for template
`functions <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=70435>`_ and `variables
<https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88061>`_. The bug causes tokenized
strings in template functions to be emitted into ``.rodata`` instead of the
tokenized string section, so they cannot be extracted for detokenization.

Fortunately, this is simple to work around in the linker script.
``pw_tokenizer_linker_sections.ld`` includes a statement that pulls tokenized
string entries from ``.rodata`` into the tokenized string section. See
`b/321306079 <https://issues.pigweed.dev/issues/321306079>`_ for details.

If tokenization is working, but strings in templates are not appearing in token
databases, check the following:

- The full contents of the latest version of ``pw_tokenizer_linker_sections.ld``
  are included with the linker script. The linker script was updated in
  `pwrev.dev/188424 <http://pwrev.dev/188424>`_.
- The ``-fdata-sections`` compilation option is in use. This places each
  variable in its own section, which is necessary for pulling tokenized string
  entries from ``.rodata`` into the proper section.

64-bit tokenization
-------------------
The Python and C++ detokenizing libraries currently assume that strings were
tokenized on a system with 32-bit ``long``, ``size_t``, ``intptr_t``, and
``ptrdiff_t``. Decoding may not work correctly for these types if a 64-bit
device performed the tokenization.

Supporting detokenization of strings tokenized on 64-bit targets would be
simple. This could be done by adding an option to switch the 32-bit types to
64-bit. The tokenizer stores the sizes of these types in the
``.pw_tokenizer.info`` ELF section, so the sizes of these types can be verified
by checking the ELF file, if necessary.

Tokenization in headers
-----------------------
The ``pw_tokenizer`` macros may be used in either source or header files.
However, care must be used when tokenizing in header files (inline functions or
templates). The value of the tokenized string must not change across translation
units, otherwise the program violates the One Definition Rule. Compilers may
emit warnings such as ``-Wlto-type-mismatch``.

Tokenization requires declaring a character array for each tokenized string. If
a tokenized string includes macros that change value in different translation
units, the variable's value changes, which is an ODR violation. The tokenized
string is never read by the program, but this should still be avoided. Ensure
tokenized strings in headers do not change value, or move them to a source file
if they might.

For example, the following hypothetical log statement uses a macro whose value
is set differently per translation unit.

.. literalinclude:: tokenize_test.cc
   :language: c++
   :start-after: pw_tokenizer-header-example-1
   :end-before: pw_tokenizer-header-example-1

To avoid potential ODR violations, the log statemenent can be moved to a .cc
file.

.. literalinclude:: tokenize_test.cc
   :language: c++
   :start-after: pw_tokenizer-header-example-2
   :end-before: pw_tokenizer-header-example-2

.. literalinclude:: tokenize_test.cc
   :language: c++
   :start-after: pw_tokenizer-header-example-3
   :end-before: pw_tokenizer-header-example-3

.. warning::

   :ref:`module-pw_log_tokenized` includes the ``PW_LOG_MODULE_NAME`` in its
   tokenized strings, which may be different in each translation unit. To safely
   log in headers, the log module name must be set for the duration of the
   header. This can be done with `push/pop macro pragmas
   <https://gcc.gnu.org/onlinedocs/gcc/Push_002fPop-Macro-Pragmas.html>`_.

   .. code-block:: c++

      #pragma push_macro("PW_LOG_MODULE_NAME")
      #undef PW_LOG_MODULE_NAME
      #define PW_LOG_MODULE_NAME "MY_MODULE_NAME"

      // header contents

      #pragma pop_macro("PW_LOG_MODULE_NAME")

----------------------
Tokenization in Python
----------------------
The Python ``pw_tokenizer.encode`` module has limited support for encoding
tokenized messages with the :func:`pw_tokenizer.encode.encode_token_and_args`
function. This function requires a string's token is already calculated.
Typically these tokens are provided by a database, but they can be manually
created using the tokenizer hash.

:func:`pw_tokenizer.tokens.pw_tokenizer_65599_hash` is particularly useful
for offline token database generation in cases where tokenized strings in a
binary cannot be embedded as parsable pw_tokenizer entries.

.. note::
   In C, the hash length of a string has a fixed limit controlled by
   ``PW_TOKENIZER_CFG_C_HASH_LENGTH``. To match tokens produced by C (as opposed
   to C++) code, ``pw_tokenizer_65599_hash()`` should be called with a matching
   hash length limit. When creating an offline database, it's a good idea to
   generate tokens for both, and merge the databases.

.. _module-pw_tokenizer-cli-encoding:

-----------------
Encoding CLI tool
-----------------
The ``pw_tokenizer.encode`` command line tool can be used to encode
format strings and optional arguments.

.. code-block:: bash

   python -m pw_tokenizer.encode [-h] FORMAT_STRING [ARG ...]

Example:

.. code-block:: text

   $ python -m pw_tokenizer.encode "There's... %d many of %s!" 2 them
         Raw input: "There's... %d many of %s!" % (2, 'them')
   Formatted input: There's... 2 many of them!
             Token: 0xb6ef8b2d
           Encoded: b'-\x8b\xef\xb6\x04\x04them' (2d 8b ef b6 04 04 74 68 65 6d) [10 bytes]
   Prefixed Base64: $LYvvtgQEdGhlbQ==

See ``--help`` for full usage details.

--------
Appendix
--------

Case study
==========
.. note:: This section discusses the implementation, results, and lessons
   learned from a real-world deployment of ``pw_tokenizer``.

The tokenizer module was developed to bring tokenized logging to an
in-development product. The product already had an established text-based
logging system. Deploying tokenization was straightforward and had substantial
benefits.

Results
-------
* Log contents shrunk by over 50%, even with Base64 encoding.

  * Significant size savings for encoded logs, even using the less-efficient
    Base64 encoding required for compatibility with the existing log system.
  * Freed valuable communication bandwidth.
  * Allowed storing many more logs in crash dumps.

* Substantial flash savings.

  * Reduced the size firmware images by up to 18%.

* Simpler logging code.

  * Removed CPU-heavy ``snprintf`` calls.
  * Removed complex code for forwarding log arguments to a low-priority task.

This section describes the tokenizer deployment process and highlights key
insights.

Firmware deployment
-------------------
* In the project's logging macro, calls to the underlying logging function were
  replaced with a tokenized log macro invocation.
* The log level was passed as the payload argument to facilitate runtime log
  level control.
* For this project, it was necessary to encode the log messages as text. In
  the handler function the log messages were encoded in the $-prefixed
  :ref:`module-pw_tokenizer-base64-format`, then dispatched as normal log messages.
* Asserts were tokenized a callback-based API that has been removed (a
  :ref:`custom macro <module-pw_tokenizer-custom-macro>` is a better
  alternative).

.. attention::
  Do not encode line numbers in tokenized strings. This results in a huge
  number of lines being added to the database, since every time code moves,
  new strings are tokenized. If :ref:`module-pw_log_tokenized` is used, line
  numbers are encoded in the log metadata. Line numbers may also be included by
  by adding ``"%d"`` to the format string and passing ``__LINE__``.

.. _module-pw_tokenizer-database-management:

Database management
-------------------
* The token database was stored as a CSV file in the project's Git repo.
* The token database was automatically updated as part of the build, and
  developers were expected to check in the database changes alongside their code
  changes.
* A presubmit check verified that all strings added by a change were added to
  the token database.
* The token database included logs and asserts for all firmware images in the
  project.
* No strings were purged from the token database.

.. tip::
   Merge conflicts may be a frequent occurrence with an in-source CSV database.
   Use the :ref:`module-pw_tokenizer-directory-database-format` instead.

Decoding tooling deployment
---------------------------
* The Python detokenizer in ``pw_tokenizer`` was deployed to two places:

  * Product-specific Python command line tools, using
    ``pw_tokenizer.Detokenizer``.
  * Standalone script for decoding prefixed Base64 tokens in files or
    live output (e.g. from ``adb``), using ``detokenize.py``'s command line
    interface.

* The C++ detokenizer library was deployed to two Android apps with a Java
  Native Interface (JNI) layer.

  * The binary token database was included as a raw resource in the APK.
  * In one app, the built-in token database could be overridden by copying a
    file to the phone.

.. tip::
   Make the tokenized logging tools simple to use for your project.

   * Provide simple wrapper shell scripts that fill in arguments for the
     project. For example, point ``detokenize.py`` to the project's token
     databases.
   * Use ``pw_tokenizer.AutoUpdatingDetokenizer`` to decode in
     continuously-running tools, so that users don't have to restart the tool
     when the token database updates.
   * Integrate detokenization everywhere it is needed. Integrating the tools
     takes just a few lines of code, and token databases can be embedded in APKs
     or binaries.
