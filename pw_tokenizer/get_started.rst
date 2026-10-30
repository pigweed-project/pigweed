.. _module-pw_tokenizer-get-started:

=============================
Get started with pw_tokenizer
=============================
.. pigweed-module-subpage::
   :name: pw_tokenizer

.. _module-pw_tokenizer-get-started-overview:

--------
Overview
--------
There are two sides to ``pw_tokenizer``, which we call tokenization and
detokenization.

* **Tokenization** converts string literals in the source code to binary tokens
  at compile time. If the string has printf-style arguments, these are encoded
  to compact binary form at runtime.
* **Detokenization** converts tokenized strings back to the original
  human-readable strings.

Here's an overview of what happens when ``pw_tokenizer`` is used:

1. During compilation, the ``pw_tokenizer`` module hashes string literals to
   generate stable 32-bit tokens.
2. The tokenization macro removes these strings by declaring them in an ELF
   section that is excluded from the final binary.
3. After compilation, strings are extracted from the ELF to build a database of
   tokenized strings for use by the detokenizer. The ELF file may also be used
   directly.
4. During operation, the device encodes the string token and its arguments, if
   any.
5. The encoded tokenized strings are sent off-device or stored.
6. Off-device, the detokenizer tools use the token database to decode the
   strings to human-readable form.

.. _module-pw_tokenizer-get-started-integration:

Integrating with Bazel / GN / CMake projects
============================================
Integrating ``pw_tokenizer`` requires a few steps beyond building the code. This
section describes one way ``pw_tokenizer`` might be integrated with a project.
These steps can be adapted as needed.

#. Add ``pw_tokenizer`` to your build. Build files for GN, CMake, and Bazel are
   provided. For Make or other build systems, add the files specified in the
   BUILD.gn's ``pw_tokenizer`` target to the build.
#. Use the tokenization macros in your code. See
   :ref:`module-pw_tokenizer-tokenization`.
#. Ensure the ``.pw_tokenizer.*`` sections are included in your output ELF file:

   * In GN and CMake, this is done automatically.
   * In Bazel, include ``"@pigweed//pw_tokenizer:linker_script"`` in the
     ``deps`` of your main binary rule. This automatically selects
     ``"@pigweed//pw_tokenizer:add_tokenizer_sections_to_default_script"`` on
     Linux (to augment the default host linker script) and
     ``"@pigweed//pw_tokenizer:tokenizer_linker_sections"`` on other platforms
     (assuming you're already overriding the default linker script). You can
     also depend on either of those targets directly.
   * In other build systems, if your binary does not use a custom linker script,
     you can pass ``pw_add_tokenizer_sections_to_default_script.ld`` to the
     linker which will augment the default linker script (rather than override
     it).
   * Alternatively, include ``pw_tokenizer/pw_tokenizer_linker_sections.ld``
     from your project's linker script by depending on
     ``"@pigweed//pw_tokenizer:pw_tokenizer_linker_sections_ld"``.

#. Compile your code to produce an ELF file.
#. Run ``database.py create`` on the ELF file to generate a CSV token
   database. See :ref:`module-pw_tokenizer-managing-token-databases`.
#. Commit the token database to your repository. See notes in
   :ref:`module-pw_tokenizer-database-management`.
#. Integrate a ``database.py add`` command to your build to automatically update
   the committed token database. In GN, use the ``pw_tokenizer_database``
   template to do this. See :ref:`module-pw_tokenizer-update-token-database`.
#. Integrate ``detokenize.py`` or the C++ detokenization library with your tools
   to decode tokenized logs. See :ref:`module-pw_tokenizer-detokenization`.

Using with Zephyr
=================
When building ``pw_tokenizer`` with Zephyr, 3 Kconfigs can be used currently:

* ``CONFIG_PIGWEED_TOKENIZER`` will automatically link ``pw_tokenizer`` as well
  as any dependencies.
* ``CONFIG_PIGWEED_TOKENIZER_BASE64`` will automatically link
  ``pw_tokenizer.base64`` as well as any dependencies.
* ``CONFIG_PIGWEED_DETOKENIZER`` will automatically link
  ``pw_tokenizer.decoder`` as well as any dependencies.

Once enabled, the tokenizer headers can be included like any Zephyr headers:

.. code-block:: cpp

   #include <pw_tokenizer/tokenize.h>

.. note::
  Zephyr handles the additional linker sections via
  ``pw_tokenizer_zephyr.ld`` which is added to the end of the linker file
  via a call to ``zephyr_linker_sources(SECTIONS ...)``.
