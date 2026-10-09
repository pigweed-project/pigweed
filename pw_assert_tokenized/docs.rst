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

* **PW_ASSERT()**: The ``PW_ASSERT()`` macro will capture the file name and line
  number of the assert statement. By default, it is passed to the logging system
  to produce a string like this:

  .. code-block:: text

     PW_ASSERT() or PW_DASSERT() failure at
     pw_result/public/pw_result/result.h:63

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

  Note that the line number is passed to the tokenized logging system as
  metadata, but is not part of the tokenized string. This is to ensure the
  CHECK callsite maximizes efficiency by only passing two arguments to the
  handler.

In both cases, the assert handler is only called with two arguments: a 32-bit
token to represent a string, and the integer line number of the callsite.

-----
Setup
-----

#. Set ``pw_assert_BACKEND = "$dir_pw_assert_tokenized:check_backend"`` and
   ``pw_assert_LITE_BACKEND = "$dir_pw_assert_tokenized:assert_backend"`` in
   your target configuration.
#. Ensure your target provides
   ``pw_log_tokenized_HANDLER_BACKEND``. By default, pw_assert_tokenized will
   forward assert failures to the log system. The tokenizer handler should check
   for ``LOG_LEVEL_FATAL`` and properly divert to a crash handler.
#. When compiling with C or C++23, ``pw_assert_tokenized`` emits ``__FILE__``
   tokens directly into the ``.pw_tokenizer.entries`` ELF section, so no
   additional token database setup is needed (and offline file name generation
   should be skipped to avoid adding unused file names to the database).

   In C++17/C++20 (where ``static constexpr`` variables are not permitted inside
   ``constexpr`` functions) or in translation units that redefine ``static``
   (such as FreeRTOS ``tasks.c`` with ``PW_THIRD_PARTY_FREERTOS_NO_STATICS``),
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
