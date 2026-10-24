.. _docs-category-test:

=======
Testing
=======
.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------
Modules
-------
The following table lists all modules related to Testing.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_unit_test`", ":bdg-primary:`stable`", "GoogleTest for embedded", ""
   ":ref:`module-pw_compilation_testing`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_daq`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_fuzzer`", ":bdg-secondary:`unstable`", "Better C++ code through easier fuzzing", "C++"
   ":ref:`module-pw_hil`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_perf_test`", ":bdg-secondary:`unstable`", "Micro-benchmarks that are easy to write and run", "C++"
   ":ref:`module-pw_target_runner`", ":bdg-primary:`stable`", "gRPC server for running executables in parallel", "Go"

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Unit testing <../pw_unit_test/docs>
   Compilation testing <../pw_compilation_testing/docs>
   Data acquisition <../pw_daq/docs>
   Fuzzing <../pw_fuzzer/docs>
   Hardware-in-the-loop <../pw_hil/docs>
   Performance testing <../pw_perf_test/docs>
   Target runner <../pw_target_runner/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

.. toctree::
   :maxdepth: 1
   :hidden:

   ../targets/host/target_docs
   ../third_party/abseil-cpp/docs
   ../third_party/fuzztest/docs
   ../third_party/googletest/docs
