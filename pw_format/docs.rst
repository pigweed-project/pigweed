.. _module-pw_format:

=========
pw_format
=========
.. pigweed-module::
   :name: pw_format

``pw_format`` supports parsing ``printf`` and Rust ``core::fmt`` style format
strings and using them to format output across compile-time and runtime
contexts. This is especially useful for languages other than C/C++ (such as
Rust) that need to process, inspect, or detokenize format strings authored in
C/C++ or maintain cross-language formatting consistency.

Disambiguation: If you're looking for code formatting support, see
:ref:`pw_presubmit <module-pw_presubmit>`.

----
Rust
----
``pw_format`` provides:

* **Format string parsers**: Parses ``printf``-style (e.g., ``%d``, ``%s``)
  and ``core::fmt``-style (e.g., ``{}``, ``{:?}``) format strings into an AST.
* **Compile-time procedural macro utilities**: Facilitates type-checking and
  code generation for format string macros.
* **Runtime formatting**: Backing string formatting for embedded log
  detokenization (such as in :ref:`module-pw_tokenizer`).

``pw_format``'s Rust API is documented in the
`pw_format crate's docs </rustdoc/pw_format/>`_.

-------------------
Conformance Testing
-------------------
``pw_format`` includes a data-driven conformance test suite under
``pw_format/conformance_test/`` to verify consistent behavior across languages:

* **Test Case Definitions**: Format strings, arguments, and expected outputs
  are defined in ``printf_cases.yaml`` and rendered into per-case unit tests
  via ``generate_printf_cases.py`` and Jinja2 templates.
* **Standard Library Reference Cross-Check**: A C++ test
  (``snprintf_reference_cc_test``) executes generated typed ``std::snprintf``
  calls (excluding cases with a ``skip_snprintf`` reason string that test
  Pigweed-specific argument fallbacks or parser behavior) to ensure expected
  outputs conform to the C standard library.
* **Rust Formatting & Fallback Conformance**: A Rust test
  (``printf_conformance_rust_test``) evaluates each generated case using
  ``FormatString::parse_printf`` and ``FormatString::format``, validating
  standard formatting, boundary conditions, and unformatted specifier retention
  when arguments are missing or mismatched.
