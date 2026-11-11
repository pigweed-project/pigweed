.. _module-pw_assert_tokenized:

===================
pw_assert_tokenized
===================
.. pigweed-module::
   :name: pw_assert_tokenized

--------
Overview
--------
The ``pw_assert_tokenized`` module provides ``PW_ASSERT()`` and ``PW_CHECK_*()``
backends for the ``pw_assert`` module. These backends are much more space
efficient than using ``pw_assert_log`` with ``pw_log_tokenized`` The tradeoff,
however, is that ``PW_CHECK_*()`` macros are much more limited as all argument
values are discarded. This means only constant string information is captured in
the reported tokens.

* **PW_ASSERT() (assert_backend)**: The default ``assert_backend`` target
  captures the file name and line number of the assert statement and passes them
  to ``pw_assert_tokenized_HandleAssertFailure()``, producing a string like
  this:

  .. code-block:: text

     PW_ASSERT() or PW_DASSERT() failure at
     pw_result/public/pw_result/result.h:63

  In C and C++23 (where ``static`` variables are permitted inside unreached
  branches of ``constexpr`` functions per P2242R3), ``assert_backend`` emits
  tokenized ``__FILE__`` strings directly into the ``.pw_tokenizer.entries`` ELF
  section. In pre-C++23 C++ or in translation units that redefine ``static`` to
  empty (such as FreeRTOS ``tasks.c`` with
  ``PW_THIRD_PARTY_FREERTOS_NO_STATICS``), it falls back to
  ``PW_TOKENIZER_STRING_TOKEN(__FILE__)``, which requires offline file name
  token generation (see Setup below).

* **PW_ASSERT() (assert_with_condition_backend)**: In C and C++23, the opt-in
  ``assert_with_condition_backend`` target enables the ``PW_ASSERT()`` family of
  macros (including ``PW_DASSERT()``, ``PW_ASSERT_OK()``, and
  ``PW_ASSERT_NOTNULL()``) to capture the assert condition expression and file
  name directly in the ``.pw_tokenizer.entries`` ELF section using the
  ``pw_log_tokenized`` key/value format:

  .. code-block:: text

     "■msg♦Assert failure: x > 0■module♦PW_ASSERT■file♦pw_result/public/pw_result/result.h"

  .. note::
     Because the stringified assert condition is embedded directly into a
     ``pw_tokenizer`` format string with no format arguments, avoid ``%``
     characters followed by valid ``printf`` flags or conversion specifiers in
     ``PW_ASSERT()`` conditions (for example, ``PW_ASSERT(x % divisor == 0)``,
     where ``% d`` is parsed as a space flag plus ``%d`` with a missing
     argument at detokenization time).

  ``assert_with_condition_backend`` requires C or C++23 (where ``static``
  variables are permitted inside unreached branches of ``constexpr`` functions
  per P2242R3) and requires that ``static`` is not macro-defined to empty;
  attempting to use it in pre-C++23 C++ or when ``static`` is defined to nothing
  fails at compile time with a ``static_assert``.

* **PW_CHECK_\*()**: The ``PW_CHECK_*()`` macros work in contexts where
  tokenization is fully supported, so they are able to capture the CHECK
  statement expression and any provided string literal in addition to the file
  name in the pw_log_tokenized key/value format:

  .. code-block:: text

     "■msg♦Check failure: \*unoptimizable >= 0, Ensure this CHECK logic
     stays■module♦KVS■file♦pw_kvs/size_report/base.cc"

  Evaluated values of ``PW_CHECK_*()`` statements are not captured, and any
  string formatting arguments are also not captured. This minimizes call-site
  cost as only two arguments are ever passed to the handler (the calculated
  token, and the line number of the statement).

  Note that for both ``PW_CHECK_*()`` and ``assert_with_condition_backend``, the
  line number is passed to the tokenized logging system as metadata, but is not
  part of the tokenized string. This is to ensure the callsite maximizes
  efficiency by only passing two arguments to the handler.

In all cases, the assert handler is only called with two arguments: a 32-bit
token to represent a string, and the integer line number of the callsite.

-----
Setup
-----

#. Set ``pw_assert_BACKEND = "$dir_pw_assert_tokenized:check_backend"`` and
   ``pw_assert_LITE_BACKEND = "$dir_pw_assert_tokenized:assert_backend"`` (or
   ``"$dir_pw_assert_tokenized:assert_with_condition_backend"`` in C and C++23
   projects) in your target configuration.
#. Ensure your target provides
   ``pw_log_tokenized_HANDLER_BACKEND``. By default, pw_assert_tokenized will
   forward assert failures to the log system. The tokenizer handler should check
   for ``LOG_LEVEL_FATAL`` and properly divert to a crash handler.
#. When compiling with C or C++23 (using either ``assert_backend`` or
   ``assert_with_condition_backend``), ``pw_assert_tokenized`` emits tokens
   directly into the ``.pw_tokenizer.entries`` ELF section, so no additional
   token database setup is needed (unless a translation unit using
   ``assert_backend`` redefines ``static`` to empty).

   When using ``assert_backend`` in C++17/C++20 (where ``static`` variables are
   not permitted inside unreached branches of ``constexpr`` functions per
   P2242R3) or in translation units that redefine ``static`` (such as FreeRTOS
   ``tasks.c`` with ``PW_THIRD_PARTY_FREERTOS_NO_STATICS``),
   ``pw_assert_tokenized`` cannot emit file name tokens into the compiled
   binary. In those configurations, the ``pw_relative_source_file_names``
   :ref:`GN template<module-pw_build-relative-source-file-names>` can be used to
   collect the names of all source files used in your final executable into a
   JSON file, which can then be included in the creation of a tokenizer
   database.

Example file name token database setup (pre-C++23)
--------------------------------------------------

.. code-block::

   pw_executable("main") {
     deps = [
       # ...
     ]
     sources = [ "main.cc" ]
   }

   pw_tokenizer_database("log_tokens") {
     database = "tools/tokenized_logs.csv"
     deps = [
       ":source_file_names",
       ":main",
     ]
     optional_paths = [ "$root_build_dir/**/*.elf" ]
     input_databases = [ "$target_gen_dir/source_file_names.json" ]
   }

   # Extracts all source/header file names from "main" and its transitive
   # dependencies for tokenization.
   pw_relative_source_file_names("source_file_names") {
     deps = [ ":main" ]
     outputs = [ "$target_gen_dir/source_file_names.json" ]
   }


.. warning::
  This module is experimental and does not provide a stable API.
