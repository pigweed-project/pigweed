.. _module-pw_async2-codelab:

=======
Codelab
=======
.. pigweed-module-subpage::
   :name: pw_async2

Welcome to the :ref:`module-pw_async2` codelab! This codelab provides a hands-on
introduction to Pigweed's cooperative asynchronous framework.

-----------------
What you'll learn
-----------------
By the end of this codelab, you'll understand all of the core ``pw_async2``
primitives and will have experience implementing common ``pw_async2`` patterns,
such as:

* Implementing a ``Task`` as a state machine

* Calling async operations and managing state across suspension points

* Writing your own async operation that uses a ``Waker`` to handle external
  events

* Using ``Channel`` for inter-task communication

* Using ``TimeProvider`` and ``Select`` to implement timeouts

To get hands-on experience with these primitives and patterns, you'll be
building a simple, simulated `vending machine
<https://en.wikipedia.org/wiki/Vending_machine>`_ application. Your vending
machine will asynchronously wait for user input events like coin insertions and
keypad presses before dispensing items.

.. tip::

   We encourage you to figure out solutions on your own, but we'll provide lots
   of help along the way if anything is unclear:

   * Click the **Hint** dropdown at the end of an instruction in order to
     see a line-by-line breakdown of the code changes that you need to make
     at that point.

   * At the end of each step, the **Checkpoint** section provides a complete
     working example of the code at that point.

   Your code doesn't have to look exactly the same as the hints and checkpoints
   that we provide. They are only there to help you get back on track if you
   get lost.

.. _module-pw_async2-codelab-help:

------------
Getting help
------------
If you get stuck or have any questions about ``pw_async2``, you're welcome to
message us on our `Discord <https://discord.gg/M9NSeTA>`_ or `file an issue
<https://pwbug.dev>`_. Or, drop into our monthly community meeting, Pigweed
Live:

.. grid:: 1

   .. grid-item-card:: :octicon:`device-camera-video` Pigweed Live
      :link: https://groups.google.com/g/pigweed
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      .. pigweed-live::

-----
Setup
-----
The code for this codelab lives in the :ref:`docs-glossary-upstream`
repository. If you don't have the upstream Pigweed repo set up on your
development machine, do so now:

#. :ref:`docs-contributing-setup-devtools`. The important thing is to
   set up ``bazelisk``, which is the core tool for interacting with the repo's
   `Bazel <https://bazel.build>`_-based build system. You'll use ``bazelisk`` to
   build and run your vending machine app.

#. :ref:`docs-contributing-clone`.

#. ``cd`` into the root of the Pigweed repo:

   .. code-block:: console

      cd pigweed

   .. note::

      For the remainder of the codelab, we'll assume that your current working
      directory is the root of the upstream Pigweed repo.

#. We've already set up the vending machine app boilerplate for you. Run the
   starting code now to verify that your setup is working correctly:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

   You should see some logs from Bazel about the build followed by these
   informational logs from the app:

   .. code-block:: none

      INF  ==========================================
      INF  Command line HW simulation notes:
      INF    Type 'q' (then enter) to quit.
      INF    Type 'd' to show the dispatcher state.
      INF    Type 'c' to insert a coin.
      INF    Type 'i' to signal an item dropped.
      INF    Type '1'..'4' to press a keypad key.
      INF  ==========================================

#. Inspect the codelab directory:

   .. code-block:: console

      ls pw_async2/codelab

   There are a lot of files and directories here, but throughout the codelab,
   you only need to edit these files:

   * :cs:`pw_async2/codelab/main.cc` (``main.cc``)
   * :cs:`pw_async2/codelab/vending_machine.h` (``vending_machine.h``)
   * :cs:`pw_async2/codelab/vending_machine.cc` (``vending_machine.cc``)

----------
Next steps
----------
Proceed to :ref:`module-pw_async2-codelab-step1` to start learning
``pw_async2`` basics.

.. toctree::
   :hidden:

   step1/docs
   step2/docs
   step3/docs
   step4/docs
   step5/docs
