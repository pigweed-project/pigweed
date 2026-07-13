:tocdepth: 3

.. _module-pw_tokenizer-detokenization:

==============
Detokenization
==============
.. pigweed-module-subpage::
   :name: pw_tokenizer

Detokenization is the process of expanding a token to the string it represents
and decoding its arguments. ``pw_tokenizer`` provides Python, C++ and
TypeScript detokenization libraries.

--------------------------------
Example: decoding tokenized logs
--------------------------------
A project might tokenize its log messages with the
:ref:`module-pw_tokenizer-base64-format`. Consider the following log file, which
has four tokenized logs and one plain text log:

.. code-block:: text

   20200229 14:38:58 INF $HL2VHA==
   20200229 14:39:00 DBG $5IhTKg==
   20200229 14:39:20 DBG Crunching numbers to calculate probability of success
   20200229 14:39:21 INF $EgFj8lVVAUI=
   20200229 14:39:23 ERR $DFRDNwlOT1RfUkVBRFk=

The project's log strings are stored in a database like the following:

.. code-block::

   1c95bd1c,          ,"Initiating retrieval process for recovery object"
   2a5388e4,          ,"Determining optimal approach and coordinating vectors"
   3743540c,          ,"Recovery object retrieval failed with status %s"
   f2630112,          ,"Calculated acceptable probability of success (%.2f%%)"

Using the detokenizing tools with the database, the logs can be decoded:

.. code-block:: text

   20200229 14:38:58 INF Initiating retrieval process for recovery object
   20200229 14:39:00 DBG Determining optimal algorithm and coordinating approach vectors
   20200229 14:39:20 DBG Crunching numbers to calculate probability of success
   20200229 14:39:21 INF Calculated acceptable probability of success (32.33%)
   20200229 14:39:23 ERR Recovery object retrieval failed with status NOT_READY

.. note::

   This example uses the :ref:`module-pw_tokenizer-base64-format`, which
   occupies about 4/3 (133%) as much space as the default binary format when
   encoded. For projects that wish to interleave tokenized with plain text,
   using Base64 is a worthwhile tradeoff.

.. _module-pw_tokenizer-base64-decoding:

---------------------
Nested detokenization
---------------------
The Python, C++, TypeScript, and Java detokenization tools support detokenizing
:ref:`nested messages <module-pw_tokenizer-nested-arguments>`.

.. tip::
   The detokenization tools support recursive detokenization for Base64, base
   10, or base 16 messages. Tokenized strings found in detokenized text are
   detokenized recursively, so nested messages can be passed as arguments.

   For example, the tokenized string for "Wow!" is ``$RhYjmQ==``. This could be
   passed as an argument to the printf-style string ``Nested message: %s``,
   which encodes to ``$pEVTYQkkUmhZam1RPT0=``. The detokenizer would decode the
   message as follows:

   ::

     "$pEVTYQkkUmhZam1RPT0=" → "Nested message: $RhYjmQ==" → "Nested message: Wow!"


--------------------
Collision resolution
--------------------
Since ``pw_tokenizer`` uses 32-bit token hashes, hash collisions can occur. To
handle collisions, detokenizers rank candidates using a scoring algorithm.

When a message is detokenized, candidates are sorted according to the following
scoring rules, from highest priority to lowest:

1. **Decoding success**: Candidates that decoded all arguments successfully are
   preferred over those that failed to decode.
2. **All bytes consumed**: Candidates that consumed all provided argument bytes
   are preferred over those that left remaining bytes.
3. **Fewest decoding errors**: Candidates with fewer argument decoding errors
   are preferred.
4. **Most decoded arguments**: Candidates that successfully decoded more of
   their arguments are preferred.
5. **Newest database entry**: Candidates with a more recent removal date (or
   those never removed from the database) are preferred.

The best candidate is used if the detokenizer can determine an unambiguous
winner.

Collision rules for nested tokens
=================================
When detokenizing nested tokens in text logs:

* **Base64 nested tokens**: A Base64 nested token is decoded whenever the
  scoring rules yield a single best result. If there is a tie, the token remains
  undecoded in the text.

  To handle cases when argument data is intentionally dropped, messages that
  fail to decode because they have no argument data are decoded if there is only
  one matching token.

* **Plain nested tokens (base 10/16)**: Plain 32-bit tokens are detokenized if
  there is exactly one match in the database, or if there are multiple matches
  but only one active (never-removed) match.

------------------------
Detokenization in Python
------------------------
To detokenize in Python, import ``Detokenizer`` from the ``pw_tokenizer``
package, and instantiate it with paths to token databases or ELF files.

.. code-block:: python

   import pw_tokenizer

   detokenizer = pw_tokenizer.Detokenizer('path/to/database.csv', 'other/path.elf')

   def process_log_message(log_message):
       result = detokenizer.detokenize(log_message.payload)
       self._log(str(result))

The ``pw_tokenizer`` package also provides the ``AutoUpdatingDetokenizer``
class, which can be used in place of the standard ``Detokenizer``. This class
monitors database files for changes and automatically reloads them when they
change. This is helpful for long-running tools that use detokenization. The
class also supports filtering token domains for the given database files in the
``<path>#<domain>`` format.

For messages that are optionally tokenized and may be encoded as binary,
Base64, or plaintext UTF-8, use
:func:`pw_tokenizer.proto.decode_optionally_tokenized`. This will attempt to
determine the correct method to detokenize and always provide a printable
string.

Investigating undecoded Base64 messages
=======================================
Tokenized messages cannot be decoded if the token is not recognized. The Python
package includes the ``parse_message`` tool, which parses tokenized Base64
messages without looking up the token in a database. This tool attempts to guess
the types of the arguments and displays potential ways to decode them.

This tool can be used to extract argument information from an otherwise unusable
message. It could help identify which statement in the code produced the
message. This tool is not particularly helpful for tokenized messages without
arguments, since all it can do is show the value of the unknown token.

The tool is executed by passing Base64 tokenized messages, with or without the
``$`` prefix, to ``pw_tokenizer.parse_message``. Pass ``-h`` or ``--help`` to
see full usage information.

Example
-------
.. code-block::

   $ python -m pw_tokenizer.parse_message '$329JMwA=' koSl524TRkFJTEVEX1BSRUNPTkRJVElPTgJPSw== --specs %s %d

   INF Decoding arguments for '$329JMwA='
   INF Binary: b'\xdfoI3\x00' [df 6f 49 33 00] (5 bytes)
   INF Token:  0x33496fdf
   INF Args:   b'\x00' [00] (1 bytes)
   INF Decoding with up to 8 %s or %d arguments
   INF   Attempt 1: [%s]
   INF   Attempt 2: [%d] 0

   INF Decoding arguments for '$koSl524TRkFJTEVEX1BSRUNPTkRJVElPTgJPSw=='
   INF Binary: b'\x92\x84\xa5\xe7n\x13FAILED_PRECONDITION\x02OK' [92 84 a5 e7 6e 13 46 41 49 4c 45 44 5f 50 52 45 43 4f 4e 44 49 54 49 4f 4e 02 4f 4b] (28 bytes)
   INF Token:  0xe7a58492
   INF Args:   b'n\x13FAILED_PRECONDITION\x02OK' [6e 13 46 41 49 4c 45 44 5f 50 52 45 43 4f 4e 44 49 54 49 4f 4e 02 4f 4b] (24 bytes)
   INF Decoding with up to 8 %s or %d arguments
   INF   Attempt 1: [%d %s %d %d %d] 55 FAILED_PRECONDITION 1 -40 -38
   INF   Attempt 2: [%d %s %s] 55 FAILED_PRECONDITION OK


.. _module-pw_tokenizer-protobuf-tokenization-python:

Detokenizing protobufs
======================
The :py:mod:`pw_tokenizer.proto` Python module defines functions that may be
used to detokenize protobuf objects in Python. The function
:py:func:`pw_tokenizer.proto.detokenize_fields` detokenizes all fields
annotated as tokenized, replacing them with their detokenized version. For
example:

.. code-block:: python

   my_detokenizer = pw_tokenizer.Detokenizer(some_database)

   my_message = SomeMessage(tokenized_field=b'$YS1EMQ==')
   pw_tokenizer.proto.detokenize_fields(my_detokenizer, my_message)

   assert my_message.tokenized_field == b'The detokenized string! Cool!'

Decoding optionally tokenized strings
-------------------------------------
The encoding used for an optionally tokenized field is not recorded in the
protobuf. Despite this, the text can reliably be decoded. This is accomplished
by attempting to decode the field as binary or Base64 tokenized data before
treating it like plain text.

The following diagram describes the decoding process for optionally tokenized
fields in detail.

.. raw:: html

   <style>
     #decodeprocess > svg { height: 1000px; }
   </style>

.. mermaid::
   :name: decodeprocess

   flowchart TD
      start([Received bytes]) --> binary

      binary[Decode as<br>binary tokenized] --> binary_ok
      binary_ok{Detokenizes<br>successfully?} -->|no| utf8
      binary_ok -->|yes| done_binary([Display decoded binary])

      utf8[Decode as UTF-8] --> utf8_ok
      utf8_ok{Valid UTF-8?} -->|no| base64_encode
      utf8_ok -->|yes| base64

      base64_encode[Encode as<br>tokenized Base64] --> display
      display([Display encoded Base64])

      base64[Decode as<br>Base64 tokenized] --> base64_ok

      base64_ok{Fully<br>or partially<br>detokenized?} -->|no| is_plain_text
      base64_ok -->|yes| base64_results

      is_plain_text{Text is<br>printable?} -->|no| base64_encode
      is_plain_text-->|yes| plain_text

      base64_results([Display decoded Base64])
      plain_text([Display text])

Potential decoding problems
---------------------------
The decoding process for optionally tokenized fields will yield correct results
in almost every situation. In rare circumstances, it is possible for it to fail,
but these can be avoided with a low-overhead mitigation if desired.

There are two ways in which the decoding process may fail.

Accidentally interpreting plain text as tokenized binary
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
If a plain-text string happens to decode as a binary tokenized message, the
incorrect message could be displayed. This is very unlikely to occur. While many
tokens will incidentally end up being valid UTF-8 strings, it is highly unlikely
that a device will happen to log one of these strings as plain text. The
overwhelming majority of these strings will be nonsense.

If an implementation wishes to guard against this extremely improbable
situation, it is possible to prevent it. This situation is prevented by
appending 0xFF (or another byte never valid in UTF-8) to binary tokenized data
that happens to be valid UTF-8 (or all binary tokenized messages, if desired).
When decoding, if there is an extra 0xFF byte, it is discarded.

Displaying undecoded binary as plain text instead of Base64
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
If a message fails to decode as binary tokenized and it is not valid UTF-8, it
is displayed as tokenized Base64. This makes it easily recognizable as a
tokenized message and makes it simple to decode later from the text output (for
example, with an updated token database).

A binary message for which the token is not known may coincidentally be valid
UTF-8 or ASCII. 6.25% of 4-byte sequences are composed only of ASCII characters
When decoding with an out-of-date token database, it is possible that some
binary tokenized messages will be displayed as plain text rather than tokenized
Base64.

This situation is likely to occur, but should be infrequent. Even if it does
happen, it is not a serious issue. A very small number of strings will be
displayed incorrectly, but these strings cannot be decoded anyway. One nonsense
string (e.g. ``a-D1``) would be displayed instead of another (``$YS1EMQ==``).
Updating the token database would resolve the issue, though the non-Base64 logs
would be difficult decode later from a log file.

This situation can be avoided with the same approach described in
`Accidentally interpreting plain text as tokenized binary`_. Appending
an invalid UTF-8 character prevents the undecoded binary message from being
interpreted as plain text.

---------------------
Detokenization in C++
---------------------
The C++ detokenization libraries can be used in C++ or any language that can
call into C++ with a C-linkage wrapper, such as Java or Rust. A reference Java
Native Interface (JNI) implementation is provided.

The C++ detokenization library uses a CSV, binary-format (created with
``database.py create --type binary``), or ELF section format token database.

Binary database
===============
Read the database from a file or include it in the source code. Pass the
database array to ``TokenDatabase::Create``, and construct a detokenizer.

.. code-block:: cpp

   Detokenizer detokenizer(TokenDatabase::Create(token_database_array));

   std::string ProcessLog(span<uint8_t> log_data) {
     return detokenizer.Detokenize(log_data).BestString();
   }

The ``TokenDatabase`` class verifies that its data is valid before using it. If
it is invalid, the ``TokenDatabase::Create`` returns an empty database for which
``ok()`` returns false. If the token database is included in the source code,
this check can be done at compile time.

.. code-block:: cpp

   // This line fails to compile with a static_assert if the database is invalid.
   constexpr TokenDatabase kDefaultDatabase = TokenDatabase::Create<kData>();

   Detokenizer OpenDatabase(std::string_view path) {
     std::vector<uint8_t> data = ReadWholeFile(path);

     TokenDatabase database = TokenDatabase::Create(data);

     // This checks if the file contained a valid database. It is safe to use a
     // TokenDatabase that failed to load (it will be empty), but it may be
     // desirable to provide a default database or otherwise handle the error.
     if (database.ok()) {
       return Detokenizer(database);
     }
     return Detokenizer(kDefaultDatabase);
   }

Detokenization from CSV
=======================
Create a detokenizer from CSV token database text using
:cc:`pw::tokenizer::Detokenizer::FromCsv`.

Detokenization from an ELF file
===============================
Utilize an ELF file (with the appropriate ``.pw_tokenizer.entries`` section
included) as a token database using
:cc:`pw::tokenizer::Detokenizer::FromElfFile`.

Detokenization from this program
================================
An ELF executable program can use itself as a token database, for detokenizing
its own tokens. This is primarily intended for testing.

* Link with ``add_detokenize_from_this_program_sections.ld``.

  * For Bazel builds this is handled automatically.
  * For GN builds use the
    ``"$dir_pw_tokenizer:detokenize_from_this_program_linker_script"`` config.

* Use :cc:`pw::tokenizer::GetDetokenizerFromThisProgram`.

----------------------------
Detokenization in TypeScript
----------------------------
To detokenize in TypeScript, import ``Detokenizer`` from the ``pigweedjs``
package, and instantiate it with a CSV token database.

.. code-block:: typescript

   import { pw_tokenizer, pw_hdlc } from 'pigweedjs';
   const { Detokenizer } = pw_tokenizer;
   const { Frame } = pw_hdlc;

   const detokenizer = new Detokenizer(String(tokenCsv));

   function processLog(frame: Frame){
     const result = detokenizer.detokenize(frame);
     console.log(result);
   }

For messages that are encoded in Base64, use ``Detokenizer::detokenizeBase64``.
`detokenizeBase64` will also attempt to detokenize nested Base64 tokens. There
is also `detokenizeUint8Array` that works just like `detokenize` but expects
`Uint8Array` instead of a `Frame` argument.

.. _module-pw_tokenizer-cli-detokenizing:

---------------------
Detokenizing CLI tool
---------------------
``pw_tokenizer`` provides two standalone command line utilities for detokenizing
Base64-encoded tokenized strings.

* ``detokenize.py`` -- Detokenizes Base64-encoded strings in files or from
  stdin.
* ``serial_detokenizer.py`` -- Detokenizes Base64-encoded strings from a
  connected serial device.

If the ``pw_tokenizer`` Python package is installed, these tools may be executed
as runnable modules. For example:

.. code-block::

   # Detokenize Base64-encoded strings in a file
   python -m pw_tokenizer.detokenize -i input_file.txt

   # Detokenize Base64-encoded strings in output from a serial device
   python -m pw_tokenizer.serial_detokenizer --device /dev/ttyACM0

See the ``--help`` options for these tools for full usage information.

--------
Appendix
--------

.. _module-pw_tokenizer-python-detokenization-c99-printf-notes:

Python detokenization: C99 ``printf`` compatibility notes
=========================================================
This implementation is designed to align with the
`C99 specification, section 7.19.6
<https://www.dii.uchile.cl/~daespino/files/Iso_C_1999_definition.pdf>`_.
Notably, this specification is slightly different than what is implemented
in most compilers due to each compiler choosing to interpret undefined
behavior in slightly different ways. Treat the following description as the
source of truth.

This implementation supports:

- Overall Format: ``%[flags][width][.precision][length][specifier]``
- Flags (Zero or More)
   - ``-``: Left-justify within the given field width; Right justification is
     the default (see Width modifier).
   - ``+``: Forces to preceed the result with a plus or minus sign (``+`` or
     ``-``) even for positive numbers. By default, only negative numbers are
     preceded with a ``-`` sign.
   - (space): If no sign is going to be written, a blank space is inserted
     before the value.
   - ``#``: Specifies an alternative print syntax should be used.
      - Used with ``o``, ``x`` or ``X`` specifiers the value is preceeded with
        ``0``, ``0x`` or ``0X``, respectively, for values different than zero.
      - Used with ``a``, ``A``, ``e``, ``E``, ``f``, ``F``, ``g``, or ``G`` it
        forces the written output to contain a decimal point even if no more
        digits follow. By default, if no digits follow, no decimal point is
        written.
   - ``0``: Left-pads the number with zeroes (``0``) instead of spaces when
     padding is specified (see width sub-specifier).
- Width (Optional)
   - ``(number)``: Minimum number of characters to be printed. If the value to
     be printed is shorter than this number, the result is padded with blank
     spaces or ``0`` if the ``0`` flag is present. The value is not truncated
     even if the result is larger. If the value is negative and the ``0`` flag
     is present, the ``0``\s are padded after the ``-`` symbol.
   - ``*``: The width is not specified in the format string, but as an
     additional integer value argument preceding the argument that has to be
     formatted.
- Precision (Optional)
   - ``.(number)``
      - For ``d``, ``i``, ``o``, ``u``, ``x``, ``X``, specifies the minimum
        number of digits to be written. If the value to be written is shorter
        than this number, the result is padded with leading zeros. The value is
        not truncated even if the result is longer.

        - A precision of ``0`` means that no character is written for the value
          ``0``.

      - For ``a``, ``A``, ``e``, ``E``, ``f``, and ``F``, specifies the number
        of digits to be printed after the decimal point. By default, this is
        ``6``.

      - For ``g`` and ``G``, specifies the maximum number of significant digits
        to be printed.

      - For ``s``, specifies the maximum number of characters to be printed. By
        default all characters are printed until the ending null character is
        encountered.

      - If the period is specified without an explicit value for precision,
        ``0`` is assumed.
   - ``.*``: The precision is not specified in the format string, but as an
     additional integer value argument preceding the argument that has to be
     formatted.
- Length (Optional)
   - ``hh``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``signed char`` or ``unsigned char``.
     However, this is largely ignored in the implementation due to it not being
     necessary for Python or argument decoding (since the argument is always
     encoded at least as a 32-bit integer).
   - ``h``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``signed short int`` or
     ``unsigned short int``. However, this is largely ignored in the
     implementation due to it not being necessary for Python or argument
     decoding (since the argument is always encoded at least as a 32-bit
     integer).
   - ``l``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``signed long int`` or
     ``unsigned long int``. Also is usable with ``c`` and ``s`` to specify that
     the arguments will be encoded with ``wchar_t`` values (which isn't
     different from normal ``char`` values). However, this is largely ignored in
     the implementation due to it not being necessary for Python or argument
     decoding (since the argument is always encoded at least as a 32-bit
     integer).
   - ``ll``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``signed long long int`` or
     ``unsigned long long int``. This is required to properly decode the
     argument as a 64-bit integer.
   - ``L``: Usable with ``a``, ``A``, ``e``, ``E``, ``f``, ``F``, ``g``, or
     ``G`` conversion specifiers applies to a long double argument. However,
     this is ignored in the implementation due to floating point value encoded
     that is unaffected by bit width.
   - ``j``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``intmax_t`` or ``uintmax_t``.
   - ``z``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``size_t``. This will force the argument
     to be decoded as an unsigned integer.
   - ``t``: Usable with ``d``, ``i``, ``o``, ``u``, ``x``, or ``X`` specifiers
     to convey the argument will be a ``ptrdiff_t``.
   - If a length modifier is provided for an incorrect specifier, it is ignored.
- Specifier (Required)
   - ``d`` / ``i``: Used for signed decimal integers.

   - ``u``: Used for unsigned decimal integers.

   - ``o``: Used for unsigned decimal integers and specifies formatting should
     be as an octal number.

   - ``x``: Used for unsigned decimal integers and specifies formatting should
     be as a hexadecimal number using all lowercase letters.

   - ``X``: Used for unsigned decimal integers and specifies formatting should
     be as a hexadecimal number using all uppercase letters.

   - ``f``: Used for floating-point values and specifies to use lowercase,
     decimal floating point formatting.

     - Default precision is ``6`` decimal places unless explicitly specified.

   - ``F``: Used for floating-point values and specifies to use uppercase,
     decimal floating point formatting.

     - Default precision is ``6`` decimal places unless explicitly specified.

   - ``e``: Used for floating-point values and specifies to use lowercase,
     exponential (scientific) formatting.

     - Default precision is ``6`` decimal places unless explicitly specified.

   - ``E``: Used for floating-point values and specifies to use uppercase,
     exponential (scientific) formatting.

     - Default precision is ``6`` decimal places unless explicitly specified.

   - ``g``: Used for floating-point values and specified to use ``f`` or ``e``
     formatting depending on which would be the shortest representation.

     - Precision specifies the number of significant digits, not just digits
       after the decimal place.

     - If the precision is specified as ``0``, it is interpreted to mean ``1``.

     - ``e`` formatting is used if the exponent would be less than ``-4`` or is
       greater than or equal to the precision.

     - Trailing zeros are removed unless the ``#`` flag is set.

     - A decimal point only appears if it is followed by a digit.

     - ``NaN`` or infinities always follow ``f`` formatting.

   - ``G``: Used for floating-point values and specified to use ``f`` or ``e``
     formatting depending on which would be the shortest representation.

     - Precision specifies the number of significant digits, not just digits
       after the decimal place.

     - If the precision is specified as ``0``, it is interpreted to mean ``1``.

     - ``E`` formatting is used if the exponent would be less than ``-4`` or is
       greater than or equal to the precision.

     - Trailing zeros are removed unless the ``#`` flag is set.

     - A decimal point only appears if it is followed by a digit.

     - ``NaN`` or infinities always follow ``F`` formatting.

   - ``c``: Used for formatting a ``char`` value.

   - ``s``: Used for formatting a string of ``char`` values.

     - If width is specified, the null terminator character is included as a
       character for width count.

     - If precision is specified, no more ``char``\s than that value will be
       written from the string (padding is used to fill additional width).

   - ``p``: Used for formatting a pointer address.

   - ``%``: Prints a single ``%``. Only valid as ``%%`` (supports no flags,
     width, precision, or length modifiers).

Underspecified details:

- If both ``+`` and (space) flags appear, the (space) is ignored.
- The ``+`` and (space) flags will error if used with ``c`` or ``s``.
- The ``#`` flag will error if used with ``d``, ``i``, ``u``, ``c``, ``s``, or
  ``p``.
- The ``0`` flag will error if used with ``c``, ``s``, or ``p``.
- Both ``+`` and (space) can work with the unsigned integer specifiers ``u``,
  ``o``, ``x``, and ``X``.
- If a length modifier is provided for an incorrect specifier, it is ignored.
- The ``z`` length modifier will decode arugments as signed as long as ``d`` or
  ``i`` is used.
- ``p`` is implementation defined.

  - For this implementation, it will print with a ``0x`` prefix and then the
    pointer value was printed using ``%08X``.

  - ``p`` supports the ``+``, ``-``, and (space) flags, but not the ``#`` or
    ``0`` flags.

  - None of the length modifiers are usable with ``p``.

  - This implementation will try to adhere to user-specified width (assuming the
    width provided is larger than the guaranteed minimum of ``10``).

  - Specifying precision for ``p`` is considered an error.
- Only ``%%`` is allowed with no other modifiers. Things like ``%+%`` will fail
  to decode. Some C stdlib implementations support any modifiers being
  present between ``%``, but ignore any for the output.
- If a width is specified with the ``0`` flag for a negative value, the padded
  ``0``\s will appear after the ``-`` symbol.
- A precision of ``0`` for ``d``, ``i``, ``u``, ``o``, ``x``, or ``X`` means
  that no character is written for the value ``0``.
- Precision cannot be specified for ``c``.
- Using ``*`` or fixed precision with the ``s`` specifier still requires the
  string argument to be null-terminated. This is due to argument encoding
  happening on the C/C++-side while the precision value is not read or
  otherwise used until decoding happens in this Python code.

Non-conformant details:

- ``n`` specifier: We do not support the ``n`` specifier since it is impossible
  for us to retroactively tell the original program how many characters have
  been printed since this decoding happens a great deal of time after the
  device sent it, usually on a separate processing device entirely.
