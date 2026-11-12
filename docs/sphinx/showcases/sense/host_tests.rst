.. _showcase-sense-tutorial-hosttests:

=================
5. Run host tests
=================
.. _GoogleTest: https://google.github.io/googletest/

:ref:`module-pw_unit_test` provides an extensive `GoogleTest`_-compatible
unit testing framework. Before building and running the app, let's first
verify that the app's logic is correct by exercising the app's unit
tests:

#. Open ``//modules/blinky/blinky_test.cc``.

   Remember that ``//`` represents the root directory of your Sense repository.
   E.g. if your Sense repo was located at ``/home/example/sense/`` then
   ``//modules/blinky/blinky_test.cc`` would be located at
   ``/home/examples/sense/modules/blinky/blinky_test.cc``.

#. Make the ``Toggle`` test fail by changing one of the expected values.
   Example:

   .. code-block:: c++

      TEST_F(BlinkyTest, Toggle) {
        // ...
        auto event = FirstActive();
        ASSERT_NE(event, monochrome_led_.events().end());
        EXPECT_EQ(event->state, State::kInactive);  // add this line
        // EXPECT_EQ(event->state, State::kActive);  // comment out this line
        EXPECT_GE(ToMs(event->timestamp - start), kIntervalMs * 0);
        start = event->timestamp;
        // ...
      }

   .. caution:: Remember to save your changes!

#. Run the tests:

   .. tab-set::

      .. tab-item:: VS Code
         :sync: vsc

         Make sure that your platform is set to **Host**, as demonstrated
         in :ref:`showcase-sense-tutorial-intel-nav`. If VS Code was displaying
         red squiggly line warnings in ``blinky_test.cc``, those should go away.

         In **Bazel Targets** expand **//modules/blinky**, then right-click
         **:blinky_test (cc_test)**, then select **Test target**.

         .. figure:: https://www.gstatic.com/pigweed/sense/20240802/test_target_v2.png
            :alt: Selecting Test target

            Starting ``blinky_test``

         A task launches a terminal. You should see ``blinky_test`` fail:

         .. code-block:: console
            :emphasize-lines: 16

            INF  LED blinking: ON
            INF  [*]
            INF  [ ]
            INF  Stopped blinking
            [       OK ] BlinkyTest.BlinkSlow
            [==========] Done running all tests.
            [  PASSED  ] 3 test(s).
            [  FAILED  ] 1 test(s).
            ================================================================================
            INFO: Found 1 test target...
            Target //modules/blinky:blinky_test up-to-date:
              bazel-bin/modules/blinky/blinky_test
            INFO: Elapsed time: 2.060s, Critical Path: 1.75s
            INFO: 27 processes: 15 internal, 12 linux-sandbox.
            INFO: Build completed, 1 test FAILED, 27 total actions
            //modules/blinky:blinky_test                   FAILED in 0.0s
              /home/kayce/.cache/bazel/_bazel_kayce/e6adb4cdc44e1f72d34a105431e60eae/execroot/_main/bazel-out/k8-fastbuild/testlogs/modules/blinky/blinky_test/test.log

            Executed 1 out of 1 test: 1 fails locally.

         Press any key to close the terminal that was launched.

      .. tab-item:: CLI
         :sync: cli

         #. Run the tests with the following command:

         Run the following command. You should see output similar to what's
         shown after the command. The key line is
         ``Executed 1 out of 1 test: 1 fails locally.``

         .. code-block:: console

            bazelisk test //modules/blinky:blinky_test

         You should see output similar to this:

         .. code-block:: text

            INF  LED blinking: ON
            INF  [*]
            INF  LED blinking: OFF
            INF  [ ]
            INF  LED blinking: ON
            INF  [*]
            INF  [ ]
            INF  Stopped blinking
            [       OK ] BlinkyTest.BlinkMany
            [ RUN      ] BlinkyTest.BlinkSlow
            INF  [ ]
            INF  PWM: -
            INF  [ ]
            INF  [ ]
            INF  PWM: +
            INF  PWM: +
            INF  PWM: +
            INF  Blinking 1 times at a 320ms interval
            INF  LED blinking: OFF
            INF  [ ]
            INF  LED blinking: ON
            INF  [*]
            INF  [ ]
            INF  Stopped blinking
            [       OK ] BlinkyTest.BlinkSlow
            [==========] Done running all tests.
            [  PASSED  ] 3 test(s).
            [  FAILED  ] 1 test(s).
            ================================================================================
            INFO: Found 1 test target...
            Target //modules/blinky:blinky_test up-to-date:
              bazel-bin/modules/blinky/blinky_test
            INFO: Elapsed time: 2.032s, Critical Path: 1.69s
            INFO: 9 processes: 1 internal, 8 linux-sandbox.
            INFO: Build completed, 1 test FAILED, 9 total actions
            //modules/blinky:blinky_test                   FAILED in 0.0s
              /home/kayce/.cache/bazel/_bazel_kayce/e6adb4cdc44e1f72d34a105431e60eae/execroot/_main/bazel-out/k8-fastbuild/testlogs/modules/blinky/blinky_test/test.log

            Executed 1 out of 1 test: 1 fails locally.

         .. tip::

            To run all host tests, run this command:

            .. code-block:: console

               bazelisk test //...

#. Revert the test to its original state. Remember to save your change.

#. Run the tests again and make sure they pass this time.

   You should see ``blinky_test`` pass this second time:

   .. code-block:: console
      :emphasize-lines: 8

      INFO: Analyzed target //modules/blinky:blinky_test (0 packages loaded, 0 targets configured).
      INFO: Found 1 test target...
      Target //modules/blinky:blinky_test up-to-date:
        bazel-bin/modules/blinky/blinky_test
      INFO: Elapsed time: 1.861s, Critical Path: 1.65s
      INFO: 4 processes: 4 linux-sandbox.
      INFO: Build completed successfully, 4 total actions
      //modules/blinky:blinky_test                   PASSED in 0.0s

      Executed 1 out of 1 test: 1 test passes.

.. note::

   If you see warnings that begin with ``There were tests whose
   specified size is too big``, you can ignore them. If you encounter this warning
   in your own project, it means you need to adjust the timeout of the tests.

.. _showcase-sense-tutorial-hosttests-summary:

-------
Summary
-------
We know that unit tests are a little boring, but they're an
important part of Pigweed's :ref:`mission <docs-mission>`.
When you're on a large embedded development team creating a new product, it's
much easier to iterate quickly when you have confidence that your code
changes do not introduce bugs in other parts of the codebase. The best way
to build that confidence is to rigorously test every part of your codebase
and to make sure all these tests pass before allowing any new code to merge.

Pigweed spends a lot of time making it easier for teams to test their
codebases, such as making it easy to run unit tests on your development
host rather than on physical hardware. This is especially useful when
your physical hardware doesn't exist yet because your hardware team
hasn't finished designing it!

.. _ASAN: https://clang.llvm.org/docs/AddressSanitizer.html
.. _TSAN: https://clang.llvm.org/docs/ThreadSanitizer.html
.. _MSAN: https://clang.llvm.org/docs/MemorySanitizer.html

Another reason why it's important to make code that can be tested on your host:
security and robustness. This enables you to run modern code analysis
tooling like `ASAN`_, `TSAN`_, `MSAN`_, :ref:`fuzzers <module-pw_fuzzer>`, and
more. These tools are unlikely to run correctly in on-device embedded contexts.
Fun fact: We caught real bugs in Sense with this tooling during development!
Head over to :ref:`showcase-sense-tutorial-analysis` next to learn more about
this.
