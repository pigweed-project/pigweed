.. _module-pw_fuzzer-guides-using_fuzztest:

=============================
Adding Fuzzers Using FuzzTest
=============================
.. pigweed-module-subpage::
   :name: pw_fuzzer

.. note::

  `FuzzTest`_ is currently only supported on Linux and MacOS using Clang and
  either Bazel or CMake builds.

.. _module-pw_fuzzer-guides-using_fuzztest-toolchain:

----------------------------------------
Step 0: Set up FuzzTest for your project
----------------------------------------
.. note::

   This workflow only needs to be done once for a project.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Include FuzzTest in your ``MODULE.bazel`` file. For example:

      .. code-block::

         bazel_dep(name = "fuzztest", version = "20241028.0")

      Then, import the FuzzTest build configurations in your ``.bazelrc`` file
      by adding and adapting the following:

      .. code-block::

         import %workspace%/path/to/pigweed/pw_fuzzer/fuzztest.bazelrc

   .. tab-item:: CMake
      :sync: cmake

      FuzzTest and its dependencies are not included in Pigweed and need to be
      added.

      See the following:

      * :ref:`module-pw_third_party_abseil_cpp-using_upstream`
      * :ref:`module-pw_third_party_fuzztest-using_upstream`
      * :ref:`module-pw_third_party_googletest-using_upstream`

      FuzzTest is enabled by setting several CMake variables. The easiest way to
      set these is to extend your ``toolchain.cmake`` file. You need to specify
      where FuzzTest and its dependencies are, include FuzzTest's build flags,
      and set the unit test backend to use FuzzTest.

      For example:

      .. code-block::

         set(dir_pw_third_party_abseil_cpp "path/to/abseil" CACHE INTERNAL "" FORCE)
         set(dir_pw_third_party_googletest "path/to/googletest" CACHE INTERNAL "" FORCE)
         set(dir_pw_third_party_fuzztest "path/to/fuzztest" CACHE INTERNAL "" FORCE)

         pw_set_backend(pw_unit_test pw_unit_test.fuzztest)

         # This line should come after adding the Pigweed subdirectory, but
         # before any fuzz tests.
         fuzztest_setup_fuzzing_flags()

      You also must enable fuzzing when you build by passing the
      ``-DFUZZTEST_FUZZING_MODE`` flag to ``cmake`` when building.

----------------------------------------
Step 1: Write a unit test for the target
----------------------------------------

As noted previously, the very first step is to identify one or more target
behavior that would benefit from testing. See `FuzzTest Use Cases`_ for more
details on how to identify this code.

Once identified, it is useful to start from a unit test. You may already have a
unit test writtern, but if not it is likely still be helpful to write one first.
Many developers are more familiar with writing unit tests, and there are
detailed guides available. See for example the `GoogleTest documentation`_.

This guide will use code from ``//pw_fuzzer/examples/fuzztest/``. This code
includes the following object as an example of code that would benefit from
fuzzing for undefined behavior and from roundtrip fuzzing.

.. note::

   To keep the example simple, this code uses the standard library. As a result,
   this code may not work with certain devices.

.. literalinclude:: ../examples/fuzztest/metrics.h
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_h]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_h]

Unit tests for this class might attempt to deserialize previously serialized
objects and to deserialize invalid data:

.. literalinclude:: ../examples/fuzztest/metrics_unittest.cc
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_unittest]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_unittest]

--------------------------------------------
Step 2: Convert your unit test to a function
--------------------------------------------

Examine your unit tests and identify any places you have fixed values that could
vary. Turn your unit test into a function that takes those values as parameters.
Since fuzzing may not occur on all targets, you should preserve your unit test
by calling the new function with the previously fixed values.

.. literalinclude:: ../examples/fuzztest/metrics_fuzztest.cc
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_fuzztest1]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_fuzztest1]

.. literalinclude:: ../examples/fuzztest/metrics_fuzztest.cc
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_fuzztest3]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_fuzztest3]

Note that in ``ArbitrarySerializeAndDeserialize`` we no longer assume the
marshalling will always be successful, and exit early if it is not. You may need
to make similar modifications to your unit tests if constraints on parameters
are not expressed by `domains`__ as described below.

.. __: `FuzzTest Domain Reference`_

--------------------------------------------
Step 3: Add a FUZZ_TEST macro invocation
--------------------------------------------

Now, include ``"pw_fuzzer/fuzztest.h"`` and pass the test suite name and your
function name to the ``FUZZ_TEST`` macro. Call ``WithDomains`` on the returned
object to specify the input domain for each parameter of the function. For
example:

.. literalinclude:: ../examples/fuzztest/metrics_fuzztest.cc
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_fuzztest2]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_fuzztest2]

.. literalinclude:: ../examples/fuzztest/metrics_fuzztest.cc
   :language: cpp
   :linenos:
   :start-after: [pwfuzzer_examples_fuzztest-metrics_fuzztest4]
   :end-before: [pwfuzzer_examples_fuzztest-metrics_fuzztest4]

You may know of specific values that are "interesting", i.e. that represent
boundary conditions, involve, special handling, etc. To guide the fuzzer towards
these code paths, you can include them as `seeds`_. However, as noted in
the comments of the examples, it is recommended to include a unit test with the
original parameters to ensure the code is tested on target devices.

FuzzTest provides more detailed documentation on these topics. For example:

* Refer to `The FUZZ_TEST Macro`_ reference for more details on how to use this
  macro.

* Refer to the `FuzzTest Domain Reference`_ for details on all the different
  types of domains supported by FuzzTest and how they can be combined.

* Refer to the `Test Fixtures`_ reference for how to create fuzz tests from unit
  tests that use GoogleTest fixtures.

------------------------------------
Step 4: Add the fuzzer to your build
------------------------------------
Next, indicate that the unit test includes one or more fuzz tests.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Unit tests can support fuzz tests by simply adding a dependency on
      FuzzTest.

      For example, consider the following ``BUILD.bazel``:

      .. literalinclude:: ../examples/fuzztest/BUILD.bazel
         :linenos:
         :start-after: [pwfuzzer_examples_fuzztest-bazel]
         :end-before: [pwfuzzer_examples_fuzztest-bazel]

   .. tab-item:: CMake
      :sync: cmake

      Unit tests can support fuzz tests by simply adding a dependency on
      FuzzTest.

      For example, consider the following ``CMakeLists.txt``:

      .. literalinclude:: ../examples/fuzztest/CMakeLists.txt
         :linenos:
         :start-after: [pwfuzzer_examples_fuzztest-cmake]
         :end-before: [pwfuzzer_examples_fuzztest-cmake]

------------------------
Step 5: Build the fuzzer
------------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      By default, ``bazel`` will simply omit the fuzz tests and build unit
      tests. To build these tests as fuzz tests, specify the ``fuzztest``
      config. For example:

      .. code-block:: console

         $ bazel build //... --config=fuzztest

   .. tab-item:: CMake
      :sync: cmake

      Build using ``cmake`` with the FuzzTest and GoogleTest variables set. For
      example:

      .. code-block::

         cmake ... \
           -Ddir_pw_third_party_fuzztest=path/to/fuzztest \
           -Ddir_pw_third_party_googletest=path/to/googletest \
           -Dpw_unit_test_BACKEND=pw_third_party.fuzztest


----------------------------------
Step 6: Running the fuzzer locally
----------------------------------
.. TODO: b/281138993 - Add tooling to make it easier to find and run fuzzers.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      By default, ``bazel`` will simply omit the fuzz tests and build and run
      unit tests. To build these tests as fuzz tests, specify the "fuzztest"
      config. For example:

      .. code-block:: console

         $ bazel test //... --config=fuzztest

      This will build the tests as fuzz tests, but only run them for a limited
      time. This makes them suitable for automated testing as in CQ.

      To run a fuzz with different options, you can use ``run`` and pass
      additional flags to the fuzzer binary. For example:

      .. code-block:: console

         $ bazel run //my_module:metrics_test --config=fuzztest \
         > --fuzz=MetricsTest.Roundtrip

   .. tab-item:: CMake
      :sync: cmake

      When built with FuzzTest and GoogleTest, the fuzzer binaries can be run
      directly from the CMake build directory. By default, the fuzzers will only
      run for a limited time. This makes them suitable for automated testing as
      in CQ. To run a fuzz with different options, you can pass additional flags
      to the fuzzer binary.

      For example:

      .. code-block:: console

         $ build/my_module/metrics_test --fuzz=MetricsTest.Roundtrip

Running the fuzzer should produce output similar to the following:

.. code-block::

   [.] Sanitizer coverage enabled. Counter map size: 21290, Cmp map size: 262144
   Note: Google Test filter = MetricsTest.Roundtrip
   [==========] Running 1 test from 1 test suite.
   [----------] Global test environment set-up.
   [----------] 1 test from MetricsTest
   [ RUN      ] MetricsTest.Roundtrip
   [*] Corpus size:     1 | Edges covered:    131 | Fuzzing time:    504.798us | Total runs:  1.00e+00 | Runs/secs:     1
   [*] Corpus size:     2 | Edges covered:    133 | Fuzzing time:    934.176us | Total runs:  3.00e+00 | Runs/secs:     3
   [*] Corpus size:     3 | Edges covered:    134 | Fuzzing time:   2.384383ms | Total runs:  5.30e+01 | Runs/secs:    53
   [*] Corpus size:     4 | Edges covered:    137 | Fuzzing time:   2.732274ms | Total runs:  5.40e+01 | Runs/secs:    54
   [*] Corpus size:     5 | Edges covered:    137 | Fuzzing time:   7.275553ms | Total runs:  2.48e+02 | Runs/secs:   248

.. TODO: b/282560789 - Add guides/improve_fuzzers.rst
.. TODO: b/281139237 - Add guides/continuous_fuzzing.rst
.. ----------
.. Next steps
.. ----------
.. Once you have a working fuzzer, the next steps are to:

.. * `Run it continuously on a fuzzing infrastructure <continuous_fuzzing>`_.
.. * `Measure its code coverage and improve it <improve_fuzzers>`_.

.. _FuzzTest: https://github.com/google/fuzztest
.. _FuzzTest Domain Reference: https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
.. _FuzzTest Use Cases: https://github.com/google/fuzztest/blob/main/doc/use-cases.md
.. _GoogleTest documentation: https://google.github.io/googletest/
.. _Test Fixtures: https://github.com/google/fuzztest/blob/main/doc/fixtures.md
.. _The FUZZ_TEST Macro: https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md
.. _sanitizer flags: https://github.com/google/sanitizers/wiki/SanitizerCommonFlags
.. _seeds: https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md#initial-seeds
