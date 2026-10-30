.. _module-pw_codegen:

==========
pw_codegen
==========
.. pigweed-module::
   :name: pw_codegen

.. pw_codegen-nav-start

.. grid:: 1

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: module-pw_codegen-api
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Reference details about the ``pw_codegen`` Python API

.. pw_codegen-nav-end

``pw_codegen`` provides low-level Python utilities for generating source code in
C++, Python, Java, Rust, and TypeScript.

--------
Overview
--------
Code generators across Pigweed (including :ref:`module-pw_protobuf`,
:ref:`module-pw_rpc`, :ref:`module-pw_rpc2`, :ref:`module-pw_enum`,
:ref:`module-pw_tokenizer`, :ref:`module-pw_module`, and
:ref:`module-pw_build`) share common primitives in ``pw_codegen``:

- :py:mod:`pw_codegen` / :py:mod:`pw_codegen.output_file`: Indentation-aware
  source file buffering (:py:class:`~pw_codegen.OutputFile`) and
  language-independent naming helpers
  (:py:func:`~pw_codegen.camel_to_snake`,
  :py:func:`~pw_codegen.camel_to_upper_snake`,
  :py:func:`~pw_codegen.snake_to_pascal`).
- :py:mod:`pw_codegen.cpp`: C++ keywords and standard-library macro sets,
  identifier validation and escaping, Google constant naming
  (:py:func:`~pw_codegen.cpp.constant_style`), integer, ``std::byte`` array,
  and UTF-8/byte string literals, ``#include`` sorting, ``namespace`` blocks,
  and Doxygen ``///`` doc comments.
- :py:mod:`pw_codegen.python`: Python identifier validation, double-quote
  preferred string literals, and safe triple-quoted docstring formatting.
- :py:mod:`pw_codegen.java`: Java string literals and ``byte[]`` array literals.
- :py:mod:`pw_codegen.rust`: Rust string literals and ``&[u8]`` byte slice
  literals.
- :py:mod:`pw_codegen.typescript`: TypeScript ``Uint8Array`` literals.

.. toctree::
   :hidden:
   :maxdepth: 1

   api
