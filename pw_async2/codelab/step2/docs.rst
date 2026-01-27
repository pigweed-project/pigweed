.. _module-pw_async2-codelab-step2:

=========================
2. Call an async function
=========================
.. pigweed-module-subpage::
   :name: pw_async2

The task that you created in the last step isn't actually asynchronous
yet. It runs from start to finish without stopping. Most real-world tasks
need to wait for one reason or another. For example, one task may need to wait
for a timer to expire. Another one might need to wait for a network packet to
arrive. In the case of the vending machine app, your task needs to wait for a
user to insert a coin.

In ``pw_async2``, operations that can wait represented by **futures**.

---------------
Wait for a coin
---------------
Your vending machine will use the ``CoinSlot`` class to read the number of coins
that a customer inserts. We've already implemented this class for you. Update
your ``VendingMachineTask`` to use ``CoinSlot`` now.

#. Study the ``GetCoins()`` declaration in :cs:`pw_async2/codelab/coin_slot.h`.

   .. literalinclude:: ../coin_slot.h
      :language: cpp
      :start-after: constexpr CoinSlot() : coins_deposited_(0) {}
      :end-before: // Report that a coin was received by the coin slot.
      :linenos:

   Asynchronous operations like ``GetCoins()`` return some type of ``Future``.
   A future is an object that represents the result of an asynchronous operation
   that may or may not be complete.

   Similar to a task's ``DoPend()``, futures have a ``Pend()`` method to
   progress them, which takes an async :cc:`Context <pw::async2::Context>` and
   returns a ``Poll`` of some value. When a task pends a future, it checks the
   return value to determine how to proceed:

   * If it's ``Ready(value)``, the operation is complete, and the task can
     continue with the ``value``.

   * If it's ``Pending()``, the operation is not yet complete. The task will
     generally stop and return ``Pending()`` itself, effectively "sleeping" until
     it is woken up.

   When a task is sleeping, it doesn't consume any CPU cycles. The
   ``Dispatcher`` won't poll it again until an external event wakes it
   up. This is the core of cooperative multitasking.

   .. note::

      If you peek into :cs:`pw_async2/codelab/coin_slot.cc` you'll see that it
      internally "wakes" its future when coins are received. You'll learn more
      about waking in the next step.

#. Update the ``VendingMachineTask`` declaration in ``vending_machine.h``
   to use ``CoinSlot``:

   * Include the ``coin_slot.h`` header

   * Update the ``VendingMachineTask`` constructor to accept a coin slot
     reference (``CoinSlot& coin_slot``)

   * Add a ``coin_slot_`` data member to ``VendingMachineTask``

   * Add a ``coin_future_`` member to store the future

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :start-after: #pragma once
         :linenos:
         :emphasize-lines: 1,11,13,20-21

#. Update ``main.cc``:

   * When creating your ``VendingMachineTask`` instance, pass the coin
     slot as an arg

   We've already provided a global ``CoinSlot`` instance (``coin_slot``) at
   the top of ``main.cc``. You don't need to create one.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/main.cc
         :language: cpp
         :start-at: int main() {
         :emphasize-lines: 5
         :linenos:

#. Update the ``DoPend()`` implementation in ``vending_machine.cc`` to interact
   with the coin slot:

   * Prompt the user to insert a coin:

     .. code-block:: cpp

        PW_LOG_INFO("Please insert a coin.");

   * Obtain a future from the ``CoinSlot`` into ``coin_future_``.

   * Use ``coin_future_.Pend(cx)`` to wait for coin insertion

   * Handle the pending case of ``coin_future_.Pend(cx)``

   * If ``coin_future_.Pend(cx)`` is ready, log the number of coins that
     were detected

   Recall that ``CoinFuture::Pend`` returns ``Poll<unsigned>`` indicating the
   status of the coin slot:

   * If ``Poll()`` returns ``Pending()``, it means that no coin has been
     inserted yet. Your task cannot proceed without payment, so it must signal
     this to the dispatcher by returning ``Pending()`` itself. Futures like
     ``CoinFuture`` which wait for data will automatically wake your waiting
     task once that data becomes available.

   * If the ``Poll`` is ``Ready()``, it means that coins have been inserted. The
     ``Poll`` object now contains the number of coins. Your task can get this
     value and proceed to the next step.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> VendingMachineTask::DoPend
         :emphasize-lines: 3-11
         :linenos:

--------------
Spot the issue
--------------
There's a problem with the current implementation…

#. Run your vending machine app again:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

   You should see the welcome message, followed by the prompt to insert coins.

   .. code-block::

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.

#. To simulate inserting a coin, type :kbd:`c` :kbd:`Enter` (type :kbd:`c`
   and then type :kbd:`Enter`):

   The hardware thread will call the coin slot's Interrupt Service Routine
   (ISR), which wakes up your task. The dispatcher will run the task again, but
   instead of seeing the success message, you'll see a duplicate welcome message
   followed by a crash!

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
         ▄████▄      ██▀███      ▄▄▄           ██████     ██░ ██
        ▒██▀ ▀█     ▓██ ▒ ██▒   ▒████▄       ▒██    ▒    ▓██░ ██▒
        ▒▓█ 💥 ▄    ▓██ ░▄█ ▒   ▒██  ▀█▄     ░ ▓██▄      ▒██▀▀██░
        ▒▓▓▄ ▄██▒   ▒██▀▀█▄     ░██▄▄▄▄██      ▒   ██▒   ░▓█ ░██
        ▒ ▓███▀ ░   ░██▓ ▒██▒    ▓█   ▓██▒   ▒██████▒▒   ░▓█▒░██▓
        ░ ░▒ ▒  ░   ░ ▒▓ ░▒▓░    ▒▒   ▓▒█░   ▒ ▒▓▒ ▒ ░    ▒ ░░▒░▒
          ░  ▒        ░▒ ░ ▒░     ▒   ▒▒ ░   ░ ░▒  ░ ░    ▒ ░▒░ ░
        ░             ░░   ░      ░   ▒      ░  ░  ░      ░  ░░ ░
        ░ ░            ░              ░  ░         ░      ░  ░  ░
        ░

      pw_async2/codelab/coin_slot.cc:27: PW_CHECK() or PW_DCHECK() FAILED!

        FAILED ASSERTION

          current_future_ == nullptr

        FILE & LINE

          pw_async2/codelab/coin_slot.cc:27

        FUNCTION

          CoinFuture codelab::CoinSlot::GetCoins()

        MESSAGE

          Called GetCoins() while a CoinFuture is already active

   What happened?

   .. dropdown:: Answer

      When a task is suspended and resumed, its ``DoPend()`` method is called
      again *from the beginning*. The first time ``DoPend()`` ran, it obtained a
      future from ``GetCoins()`` and then returned ``Pending()``.

      When the coin was inserted, the task was woken up and the dispatcher called
      ``DoPend()`` again from the top. It printed the welcome message again, then
      tried to call ``GetCoins()`` a second time. However, ``CoinSlot`` enforces
      that only one future can be active at a time, so it asserted and crashed.

      This demonstrates a critical concept of asynchronous programming: **tasks
      must manage their own state**.

-----------------------
Manage the future state
-----------------------
Because a task can be suspended and resumed at any ``Pending()`` return, you
need a way to remember where you left off. All futures have an
``is_pendable()`` method that can help us determining whether the future has
been initialized and can be polled.

#. Update ``vending_machine.cc``:

   * Gate the welcome message and ``GetCoins()`` call in ``DoPend()`` so they
     only run if ``!coin_future_.is_pendable()``.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint2/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> VendingMachineTask::DoPend
         :end-before: pw::async2::Poll<unsigned> poll_result
         :emphasize-lines: 2-6
         :linenos:

--------------
Verify the fix
--------------
With this guard in place, the vending machine task will only obtain a future
once and no longer crash.

#. Run the app again:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Simulate inserting a coin by pressing :kbd:`c` :kbd:`Enter`.

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Received 1 coin. Dispensing item.

------------------
Reduce boilerplate
------------------
The pattern of polling a future and returning ``Pending()`` if it's not ready is
common in ``pw_async2``. To reduce this boilerplate, ``pw_async2`` provides the
:cc:`PW_TRY_READY_ASSIGN` macro to simplify writing clean async code.

#. Refactor the ``DoPend()`` implementation in ``vending_machine.cc``:

   * Replace the code that you wrote in the last step with a
     :cc:`PW_TRY_READY_ASSIGN` implementation that handles both the ready and
     pending scenarios.

     1. If the future returns ``Pending()``, the macro immediately returns
     ``Pending()`` from the current function (your ``DoPend``). This propagates
     the "sleeping" state up to the dispatcher.

     2. If the future returns ``Ready(some_value)``, the macro unwraps the value
     and assigns it to a variable you specify. The task then continues executing.

   For those familiar with ``async/await`` in other languages like Rust or
   Python, this macro serves a similar purpose to the ``await`` keyword.
   It's the point at which your task can be suspended.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint3/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> VendingMachineTask::DoPend
         :emphasize-lines: 8
         :linenos:

----------
Next steps
----------
You've now implemented a task that waits for an asynchronous event and
correctly manages its state. In :ref:`module-pw_async2-codelab-step3`, you'll
learn how to write your own pendable functions.

.. _module-pw_async2-codelab-step2-checkpoint:

----------
Checkpoint
----------
At this point, your code should look similar to the files below.

.. tab-set::

   .. tab-item:: main.cc

      .. literalinclude:: ./checkpoint3/main.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.cc

      .. literalinclude:: ./checkpoint3/vending_machine.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.h

      .. literalinclude:: ./checkpoint3/vending_machine.h
         :language: cpp
         :start-after: // the License.
