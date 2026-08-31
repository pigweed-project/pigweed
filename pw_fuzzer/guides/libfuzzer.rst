.. _module-pw_fuzzer-guides-using_libfuzzer:

==============================
Adding Fuzzers Using LibFuzzer
==============================
.. pigweed-module-subpage::
   :name: pw_fuzzer

.. note::

  `libFuzzer`_ is currently only supported on Linux and MacOS using Clang and
  either Bazel or GN builds.

.. _module-pw_fuzzer-guides-using_libfuzzer-toolchain:

-----------------------------------------
Step 0: Set up libFuzzer for your project
-----------------------------------------
.. note::

   This workflow only needs to be done once for a project.

`libFuzzer`_ is a LLVM compiler runtime and should included with your ``clang``
installation. In order to use it, you only need to define a suitable toolchain.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Include ``rules_fuzzing`` in your ``MODULE.bazel`` file. For example:

      .. code-block::

         bazel_dep(name = "rules_fuzzing", version = "0.5.2")

      Then, import the libFuzzer build configurations in your ``.bazelrc`` file
      by adding and adapting the following:

      .. code-block::

         import %workspace%/path/to/pigweed/pw_fuzzer/libfuzzer.bazelrc

   .. tab-item:: GN
      :sync: gn

      Use ``pw_toolchain_host_clang``, or derive a new toolchain from it.
      For example:

      .. code-block::

         import("$dir_pw_toolchain/host/target_toolchains.gni")

         my_toolchains = {
           ...
           clang_fuzz = {
             name = "my_clang_fuzz"
             forward_variables_from(pw_toolchain_host.clang_fuzz, "*", ["name"])
           }
           ...
         }

------------------------------------
Step 1: Write a fuzz target function
------------------------------------
To write a fuzzer, a developer needs to write a `fuzz target function`_
following the guidelines given by libFuzzer:

.. code-block:: cpp

   extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
     DoSomethingInterestingWithMyAPI(data, size);
     return 0;  // Non-zero return values are reserved for future use.
   }

When writing your fuzz target function, you may want to consider:

- It is acceptable to return early if the input doesn't meet some constraints,
  e.g. it is too short.
- If your fuzzer accepts data with a well-defined format, you can bootstrap
  coverage by crafting examples and adding them to a `corpus`_.
- There are tools to `split a fuzzing input`_ into multiple fields if needed;
  the `FuzzedDataProvider`_ is particularly easy to use.
- If your code acts on "transformed" inputs, such as encoded or compressed
  inputs, you may want to try `structure aware fuzzing`.
- You can do `startup initialization`_ if you need to.
- If your code is non-deterministic or uses checksums, you may want to disable
  those **only** when fuzzing by using LLVM's
  `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`_

------------------------------------
Step 2: Add the fuzzer to your build
------------------------------------
To build a fuzzer, do the following:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Add a Bazel target to the module using the ``pw_cc_fuzz_test`` rule. For
      example:

      .. code-block::

         # In $dir_my_module/BUILD.bazel
         pw_cc_fuzz_test(
             name = "my_fuzzer",
             srcs = ["my_fuzzer.cc"],
             deps = [":my_lib"]
         )

   .. tab-item:: GN
      :sync: gn

      Add the GN target to the module using ``pw_fuzzer`` GN template. If you
      wish to limit when the generated unit test is run, you can set
      ``enable_test_if`` in the same manner as ``enable_if`` for `pw_test`:

      .. code-block::

         # In $dir_my_module/BUILD.gn
         import("$dir_pw_fuzzer/fuzzer.gni")

         pw_fuzzer("my_fuzzer") {
           sources = [ "my_fuzzer.cc" ]
           deps = [ ":my_lib" ]
           enable_test_if = device_has_1m_flash
         }

      Add the fuzzer GN target to the module's group of fuzzers. Create this
      group if it does not exist.

      .. code-block::

         # In $dir_my_module/BUILD.gn
         group("fuzzers") {
           deps = [
             ...
             ":my_fuzzer",
           ]
         }

      Make sure this group is referenced from a top-level ``fuzzers`` target in
      your project, with the appropriate
      :ref:`fuzzing toolchain<module-pw_fuzzer-guides-using_libfuzzer-toolchain>`.
      For example:

      .. code-block::

         # In //BUILD.gn
         group("fuzzers") {
           deps = [
             ...
             "$dir_my_module:fuzzers(//my_toolchains:host_clang_fuzz)",
           ]
         }

----------------------------------------------
Step 3: Add the fuzzer unit test to your build
----------------------------------------------
Pigweed automatically generates unit tests for libFuzzer-based fuzzers in some
build systems.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Fuzzer unit tests are included automatically in Pigweed's Bazel build.

   .. tab-item:: GN
      :sync: gn

      The generated unit test will be suffixed by ``_test`` and needs to be
      added to the module's test group. This test verifies the fuzzer can build
      and run, even when not being built in a
      :ref:`fuzzing toolchain<module-pw_fuzzer-guides-using_libfuzzer-toolchain>`.
      For example, for a fuzzer called ``my_fuzzer``, add the following:

      .. code-block::

         # In $dir_my_module/BUILD.gn
         pw_test_group("tests") {
           tests = [
             ...
             ":my_fuzzer_test",
           ]
         }

------------------------
Step 4: Build the fuzzer
------------------------
LibFuzzer-style fuzzers require the compiler to add instrumentation and
runtimes when building.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Specify the libFuzzer config and a sanitizer config when building fuzzers.

      .. code-block:: console

         $ bazel build //my_module:my_fuzzer --config=asan --config=libfuzzer

   .. tab-item:: GN
      :sync: gn

      Select a sanitizer runtime. See LLVM for `valid options`_.

      .. code-block:: console

         $ gn gen out --args='pw_toolchain_SANITIZERS=["address"]'

      Some toolchains may set a default for fuzzers if none is specified. For
      example, `//targets/host:host_clang_fuzz` defaults to "address".

      Build the fuzzers using ``ninja`` directly.

      .. code-block:: console

         $ ninja -C out fuzzers

----------------------------------
Step 5: Running the fuzzer locally
----------------------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Specify the libFuzzer config and a sanitizer config when building and
      running fuzzers. For each fuzzer build rule with a given name,
      `rules_fuzzing`_ produces a ``<name>_run`` target. For example:

      .. code-block:: console

         $ bazel run //my_module:my_fuzzer_run --config=asan --config=libfuzzer\
         > -- --timeout_secs=60

   .. tab-item:: GN
      :sync: gn

      The fuzzer binary will be in a subdirectory related to the toolchain.
      Additional `libFuzzer options`_ and `corpus`_ arguments can be passed on
      the command line. For example:

      .. code-block:: console

         $ out/host_clang_fuzz/obj/my_module/bin/my_fuzzer -seed=1 path/to/corpus

      Additional `sanitizer flags`_ may be passed uisng environment variables.

Running the fuzzer should produce output similar to the following:

.. code-block::

   INFO: Seed: 305325345
   INFO: Loaded 1 modules   (46 inline 8-bit counters): 46 [0x38dfc0, 0x38dfee),
   INFO: Loaded 1 PC tables (46 PCs): 46 [0x23aaf0,0x23add0),
   INFO:        0 files found in corpus
   INFO: -max_len is not provided; libFuzzer will not generate inputs larger than 4096 bytes
   INFO: A corpus is not provided, starting from an empty corpus
   #2      INITED cov: 2 ft: 3 corp: 1/1b exec/s: 0 rss: 27Mb
   #4      NEW    cov: 3 ft: 4 corp: 2/3b lim: 4 exec/s: 0 rss: 27Mb L: 2/2 MS: 2 ShuffleBytes-InsertByte-
   #11     NEW    cov: 7 ft: 8 corp: 3/7b lim: 4 exec/s: 0 rss: 27Mb L: 4/4 MS: 2 EraseBytes-CrossOver-
   #27     REDUCE cov: 7 ft: 8 corp: 3/6b lim: 4 exec/s: 0 rss: 27Mb L: 3/3 MS: 1 EraseBytes-
   #29     REDUCE cov: 7 ft: 8 corp: 3/5b lim: 4 exec/s: 0 rss: 27Mb L: 2/2 MS: 2 ChangeBit-EraseBytes-
   #445    REDUCE cov: 9 ft: 10 corp: 4/13b lim: 8 exec/s: 0 rss: 27Mb L: 8/8 MS: 1 InsertRepeatedBytes-
   ...

.. TODO: b/282560789 - Add guides/improve_fuzzers.rst
.. TODO: b/281139237 - Add guides/continuous_fuzzing.rst
.. ----------
.. Next steps
.. ----------
.. Once you have created a fuzzer, you may want to:

.. * `Run it continuously on a fuzzing infrastructure <continuous_fuzzing>`_.
.. * `Measure its code coverage and improve it <improve_a_fuzzer>`_.

.. inclusive-language: disable

.. _AddressSanitizer: https://github.com/google/sanitizers/wiki/AddressSanitizer
.. _continuous_fuzzing: :ref:`module-pw_fuzzer-guides-continuous_fuzzing`
.. _corpus: https://llvm.org/docs/LibFuzzer.html#corpus
.. _fuzz target function: https://llvm.org/docs/LibFuzzer.html#fuzz-target
.. _FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION: https://llvm.org/docs/LibFuzzer.html#fuzzer-friendly-build-mode
.. _FuzzedDataProvider: https://github.com/llvm/llvm-project/blob/HEAD/compiler-rt/include/fuzzer/FuzzedDataProvider.h
.. _improve_fuzzers: :ref:`module-pw_fuzzer-guides-improve_fuzzers
.. _libFuzzer: https://llvm.org/docs/LibFuzzer.html
.. _libFuzzer options: https://llvm.org/docs/LibFuzzer.html#options
.. _rules_fuzzing: https://github.com/bazel-contrib/rules_fuzzing/blob/master/docs/guide.md#building-and-running
.. _sanitizer flags: https://github.com/google/sanitizers/wiki/SanitizerCommonFlags
.. _split a fuzzing input: https://github.com/google/fuzzing/blob/HEAD/docs/split-inputs.md
.. _startup initialization: https://llvm.org/docs/LibFuzzer.html#startup-initialization
.. _structure aware fuzzing: https://github.com/google/fuzzing/blob/HEAD/docs/structure-aware-fuzzing.md
.. _valid options: https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html

.. inclusive-language: enable
