.. _module-pw_third_party_fuzztest:

========
FuzzTest
========
The ``$pw_external_fuzztest/`` module provides build files to allow
optionally including upstream FuzzTest.

.. _module-pw_third_party_fuzztest-using_upstream:

-----------------------
Using upstream FuzzTest
-----------------------
If you want to use FuzzTest, you must do the following:

Submodule
=========

.. tab-set::

   .. tab-item:: Bazel
      Set the following :ref:`label flags <docs-build_system-bazel_flags>`:

      * ``pw_fuzzer_fuzztest_backend`` to ``@com_google_fuzztest//fuzztest``.

      The easiest way to do this is to ensure your ``.bazelrc`` includes
      Pigweed's ``pw_fuzzer/fuzztest.bazelrc``, and then include the
      ``fuzztest`` config on the command line.

      For example:

      .. code-block:: console

         $ bazel test //... --config=fuzztest --config=asan

   .. tab-item:: CMake
      Add FuzzTest to your workspace with the following command.

      .. code-block:: console

         $ git submodule add https://github.com/google/fuzztest.git \
         > third_party/fuzztest

      Set the following CMake variables:

      * Set ``dir_pw_third_party_fuzztest`` to the location of the
        FuzzTest source.

      * Set ``dir_pw_third_party_googletest`` to the location of the
        :ref:`module-pw_third_party_googletest` source.

      * Set ``pw_unit_test_BACKEND`` to ``pw_third_party.fuzztest``.
