.. _module-pw_fuzzer-guides-reproducing_oss_fuzz_bugs:

==============
Using OSS-Fuzz
==============
.. pigweed-module-subpage::
   :name: pw_fuzzer

.. TODO: b/281139237 - Update with better instructions for downstream projects.

Core Pigweed is integrated with `OSS-Fuzz`_, a continuous fuzzing infrastructure
for open source software. Fuzzers listed in in ``pw_test_groups`` will
automatically start being run within a day or so of appearing in the git
repository.

-------------------------
Reproducing Bugs Directly
-------------------------

Bugs produced by OSS-Fuzz can be found in its `Monorail instance`_. These bugs
include:

* A detailed report, including a symbolized backtrace.
* A revision range indicating when the bug has been detected.
* A minimized testcase, which is a fuzzer input that can be used to reproduce
  the bug.

To reproduce a bug:

#. Build the fuzzers in a local source checkout using one of the
   :ref:`module-pw_fuzzer-guides`.
#. Download the minimized testcase from the OSS-Fuzz bug.
#. Run the fuzzer with the testcase as an argument.

For example, if the testcase is saved as ``~/Downloads/testcase``
and the fuzzer is the same as in the examples above, you could run:

.. code-block:: console

   $ bazelisk run //pw_fuzzer/toy_fuzzer_run --config=asan --config=libfuzzer\
   > -- ~/Downloads/testcase

As noted in OSS-Fuzz's documentation on `timeouts and OOMs`_, you may want to
add a `-timeout=25` or `-rss_limit_mb=2560` argument to reproduce timeouts or
OOMs, respectively.

--------------------------------
Using a OSS-Fuzz Docker Instance
--------------------------------

If Pigweed fails to build for OSS-Fuzz, or if a fuzzer only triggers a bug in
OSS-Fuzz and not when run directly, you may want to recreate the OSS-Fuzz
environment locally using Docker. You can do so using OSS-Fuzz's documentation
on `reproducing`_ issues.

In particular, you can recreate the OSS-Fuzz environment using:

.. code-block::

   $ python infra/helper.py pull_images
   $ python infra/helper.py build_image pigweed
   $ python infra/helper.py build_fuzzers --sanitizer <address/undefined> pigweed

Using a Local Source Checkout
=============================

When addressing build failures or issues related to specific fuzzers, it is
very useful to have an OSS-Fuzz instance use a local source checkout with edits
rather than pull from a public repo. Unfortunately, the normal workflow for
`using a local source checkout`_ **does not work** for Pigweed. Pigweed provides
an embedded development environment along with source code for individual
modules, and this environment includes checks that conflict with the way
OSS-Fuzz tries to remap and change ownership of the source code.

To work around this, a helper script is provided as part of the ``pigweed``
project on OSS-Fuzz that wraps the usual ``infra/helper.py``. For commands that
take a local source path, the wrapper instead provides a ``--local`` flag. This
flag will use the ``PW_ROOT`` environment variable to find the source checkout,
and attempt to mount it in the correct location and set specific environment
variables in order to present a working development environment to the OSS-Fuzz
instance. Also, the ``pigweed`` project is implied:

.. code-block::

   $ python project/pigweed/helper.py build_fuzzers --sanitizer <sanitizer> --local

The ``sanitizer`` value is one of the usual values based to Clang via
``-fsanitize=...``, e.g. "address" or "undefined".

After building with a local source checkout, you can verify an issue previously
found by a fuzzer is fixed:

.. code-block::

   $ python project/pigweed/helper.py reproduce <fuzzer> ~/Downloads/testcase

For libFuzzer-based fuzzers, ``fuzzer`` will be of the form
``{module_name}_{fuzzer_name}``, e.g. ``pw_protobuf_encoder_fuzzer``.

For FuzzTest-based fuzzers, ``fuzzer`` will additionally include the test case
and be of the form ``{module_name}_{fuzzer_name}@{test_case}``, e.g.
``pw_hdlc_decoder_test@Decoder.ProcessNeverCrashes``.

The helper script attempts to restore proper ownership of the source checkout to
the current user on completion. This can also be triggered manually using:

.. code-block::

   $ python project/pigweed/helper.py reset_local


.. _Monorail instance: https://bugs.chromium.org/p/oss-fuzz/issues/list?q=pigweed
.. _OSS-Fuzz: https://github.com/google/oss-fuzz
.. _reproducing: https://google.github.io/oss-fuzz/advanced-topics/reproducing/
.. _timeouts and OOMs: https://google.github.io/oss-fuzz/faq/#how-do-you-handle-timeouts-and-ooms
.. _using a local source checkout: https://google.github.io/oss-fuzz/advanced-topics/reproducing/#reproduce-using-local-source-checkout
