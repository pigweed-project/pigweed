.. _module-pw_async2-codelab-step3:

===========================
3. Create and wake a Future
===========================
.. pigweed-module-subpage::
   :name: pw_async2

Currently, your vending machine immediately dispenses an item after receiving
coins. In this step, you modify your app to let customers choose what to buy.
Along the way you will learn how to correctly wake up a task that is waiting for
input like this. You will also gain some experience implementing a ``Future``
yourself.

In practice, you would not write a custom ``Future`` for such a common case as
this. Pigweed provides ``ValueFuture`` for the purpose of returning a single
async value. At the end of this step, we will use ``FutureCore`` to simplify
our custom ``Future`` implementation.

---------------------
Stub the Keypad class
---------------------
The hardware simulator sends you keypad events via async calls to
``key_press_isr()``. The simulator sends integer values between 0 and 9
that indicate which keypad button was pressed. Your mission is to process
these keypad events safely and to allow your task to wait for keypad
numbers after receiving coins.

#. Use the keypad in ``main.cc``:

   * Declare a global ``keypad`` instance

   * Update ``key_press_isr()`` to handle keypad input events by invoking
     ``keypad.Press(key)``

   * Pass a reference to the keypad when creating your task instance

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/main.cc
         :language: cpp
         :start-at: namespace {
         :linenos:
         :emphasize-lines: 4,14,29

#. Set up the ``Keypad`` and ``KeyPressFuture`` classes in
   ``vending_machine.h``:

   * Declare the stub ``Keypad`` and ``KeyPressFuture`` classes:

     .. literalinclude:: ./checkpoint1/vending_machine.h
        :language: cpp
        :start-at: class Keypad {
        :end-at: static_assert(pw::async2::Future<KeyPressFuture>);
        :linenos:

     Futures are required to expose four members:

     - ``value_type``: The type returned by the future when it completes
     - ``Pend(Context& cx)``: Pend until the future is ready
     - ``is_pendable()``: Return true if the future is valid and has not yet
       completed (i.e. handed out by some async operation rather than default
       constructed)
     - ``is_complete()``: Return true if the future completed and its value
       has been consumed

     The concept ``pw_async2::Future`` can be used to ensure that your type
     conforms to the ``Future`` interface.

   * Set up a private ``keypad_`` and ``key_future_`` member
     for ``VendingMachineTask``

   * Add a ``Keypad&`` argument to the ``VendingMachineTask`` constructor
     parameters and use it to initialize the ``keypad_`` member

   * Set up an ``unsigned coins_inserted_`` data member in
     ``VendingMachineTask`` that tracks how many coins have been inserted and
     initialize ``coins_inserted_`` to ``0`` in the constructor

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :start-at: namespace codelab {
         :end-at: }  // namespace codelab
         :linenos:
         :emphasize-lines: 3-55,62,63,73,74

#. Create a stub implementation and integrate it (as well
   as the coin count data member) in ``vending_machine.cc``:

   * Create a stub ``KeyPressFuture`` implementation:

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<int> KeyPressFuture::Pend
         :end-at: void Keypad::Press(int key) {}
         :linenos:

      In the ``Pend`` member function we return the value of the key if it
      exists, or ``Pending`` otherwise. ``WaitForKeyPress()`` always returns a
      future its value set to ``-1``, so it completes immediately. We will fix
      that later.

   * Update ``VendingMachineTask::DoPend()`` to check if coins have already
     been inserted before it pends for coins (``coin_future_.Pend(cx)``)

   * Obtain a ``KeyPressFuture`` and wait for keypad input by invoking
     ``key_future_.Pend(cx)`` after coins have been inserted

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-after: namespace codelab {
         :end-before: }  // namespace codelab
         :linenos:
         :emphasize-lines: 13,20-23,26-28,30-31

------------------------------
Verify the stub implementation
------------------------------
#. Run the app:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to insert a coin.

   You should see a log stating that ``-1`` was pressed. This is expected since
   the ``KeyPad::Pend()`` stub implementation returns ``key_pressed_``, which
   was initialized to ``kNone`` (``-1``).

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.
      INF  Keypad -1 was pressed. Dispensing an item.

--------------------
Handle keypad events
--------------------
Now, let's update our ``Keypad`` and ``KeyPressFuture`` implementations to
actually handle key presses.

#. Track the active ``KeyPressFuture`` in ``Keypad``:

   In order to resolve a future when a key is pressed, they ``Keypad`` needs to
   be aware of the active future. However, as the future is owned by the caller,
   they can freely move it around. Therefore, we also need ``KeyPressFuture``
   to keep a reference to its ``Keypad`` to update its pointer if moved.

   The full code for these operations is given below:

   .. literalinclude:: ./checkpoint2/vending_machine.cc
      :language: cpp
      :start-at: KeyPressFuture::KeyPressFuture(KeyPressFuture&& other) noexcept
      :end-before: pw::async2::Poll<int> KeyPressFuture::Pend(pw::async2::Context& cx) {
      :linenos:

   .. note::

      You should rarely have to write these boilerplate move operations yourself
      as Pigweed provides utilities to handle this for you, which will be
      introduced in a later step.

   * Add a ``Keypad*`` pointer to ``KeyPressFuture`` and replace the stub
     value constructor with a constructor that accepts a ``Keypad*``,
     setting the state to ``kInitialized`` and the new future as the keypad's
     active future.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint2/vending_machine.h
         :language: cpp
         :start-at: explicit KeyPressFuture(Keypad& keypad)
         :end-before: // Possible states of the future.
         :linenos:

   * Update ``WaitForKeyPress()`` to pass ``this`` to the ``KeyPressFuture``
     constructor.

#. Protect keypad data access in ``vending_machine.h``:

   * Include the ``pw_sync/interrupt_spin_lock.h`` and
     ``pw_sync/lock_annotations.h`` headers

   * Define an internal ``KeypadLock`` function to return a reference to a
     static ``pw::sync::InterruptSpinLock``.

     .. note::

        We use a global lock here to safely handle destructor ordering issues.
        This logic mimics :cc:`pw::async2::ValueFuture`, which uses a similar
        pattern. While we are building this manually for learning purposes, in
        real code you would just use ``ValueFuture`` directly, which handles
        this for you.

   * Guard ``key_press_future_`` with the spin lock with :cc:`PW_GUARDED_BY`

   Since the keypad ISR is asynchronous, you'll need to synchronize access to
   the stored event data. For this codelab, we use
   :cc:`pw::sync::InterruptSpinLock` which is safe to acquire from an ISR in
   production use. Alternatively you can use atomic operations.

   We use :cc:`PW_GUARDED_BY` to add a compile-time check to ensure that
   the protected key future is only accessed when the spin lock is held.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint2/vending_machine.h
         :language: cpp
         :start-after: #pragma once
         :end-before: class KeyPressFuture {
         :linenos:
         :emphasize-lines: 8-9,12-17,38-39

#. Implement keypress handling in ``vending_machine.cc``:

   * In ``Keypad::Press()``, store the keypress data in the active future if one
     exists. Remember to hold the lock!

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint2/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<int> KeyPressFuture::Pend(pw::async2::Context& cx) {
         :end-before: pw::async2::Poll<> VendingMachineTask::DoPend
         :linenos:
         :emphasize-lines: 10-15

And we're done… what could go wrong?

------------------------------
Test the keypad implementation
------------------------------
#. Run the app:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to insert a coin.

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.

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

      pw_async2/task.cc:186: PW_CHECK() or PW_DCHECK() FAILED!

        FAILED ASSERTION

          !wakers_.empty()

        FILE & LINE

          pw_async2/task.cc:186

        FUNCTION

          Task::RunResult pw::async2::Task::RunInDispatcher()

        MESSAGE

          Task VendingMachineTask:0x16b48e5b0 returned Pending() without registering a waker

   To prevent obviously incorrect usage, the ``pw_async2`` module asserts if
   you return ``Pending()`` without actually storing a ``Waker``, because it
   means your task has no way of being woken back up.

-------------
Store a waker
-------------
In general, you should always store a  :cc:`Waker <pw::async2::Waker>` before
returning ``Pending()``. A waker is a lightweight object that allows you to
tell the dispatcher to wake a task. When a ``Task::DoPend()`` call returns
``Pending()``, the task is put to sleep so that the dispatcher doesn't have to
repeatedly poll the task. Without a waker, the task will sleep forever.

#. Declare a waker in ``vending_machine.h``:

   * Include the ``pw_async2/waker.h`` header

   * Add a ``pw::async2::Waker waker_`` private member to the ``KeyPressFuture``
     class

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint3/vending_machine.h
         :language: cpp
         :start-after: #pragma once
         :end-before: // The main task that drives the vending machine.
         :linenos:
         :emphasize-lines: 8,77

#. Use the waker in ``vending_machine.cc``:

   * In ``KeyPressFuture::Pend()`` store the waker before returning ``Pending()``:

     .. code-block:: cpp

        PW_ASYNC_STORE_WAKER(cx, waker_, "Waiting for keypad press");

   The last argument should always be a meaningful string describing the
   wait reason. In the next section you'll see how this string can help you
   debug issues.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint3/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<int> KeyPressFuture::Pend(pw::async2::Context& cx) {
         :end-before: pw::async2::Poll<> VendingMachineTask::DoPend
         :linenos:
         :emphasize-lines: 5

.. tip::

   See :ref:`module-pw_async2-guides-primitives-wakers` for an overview of all
   of the different ways that you can set up wakers.

-----------------------
Forget to wake the task
-----------------------
You've set up the waker but you're not using it yet. Let's see what happens.

#. Run the app:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to insert a coin.

#. Press :kbd:`1` :kbd:`Enter` to select an item.

   You should see output like this:

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.
      1

   Nothing happens, not even an assertion! This time there is no crash!
   Why??

   .. dropdown:: Answer

      The problem is that ``pw_async2`` has **no way of knowing when the task
      is ready to be woken up**.

      The reason ``pw_async2`` can detect forgetting to store the waker, on
      the other hand, is because it happens during a ``pw_async2``-initiated
      call into your code, so there can be a postcondition check.

#. Debug this issue by pressing :kbd:`d` :kbd:`Enter`:

   You should see output like this:

   .. code-block:: none

      d
      INF  pw::async2::Dispatcher
      INF  Woken tasks:
      INF  Sleeping tasks:
      INF    - VendingMachineTask:0x7ffeec48fd90 (1 wakers)
      INF      * Waker 1: Waiting for keypad press

   This shows the state of all the tasks registered with the dispatcher.
   The last line is the wait reason string that you provided when you registered
   the waker. We can see that the vending machine task is still sleeping.

   .. tip::

      If you use ``pw_async2`` in your own project, you can get this kind of
      debug information by calling :cc:`LogRegisteredTasks
      <pw::async2::Dispatcher::LogRegisteredTasks>`.

      If you don't see the reason messages, make sure that
      :cc:`PW_ASYNC2_DEBUG_WAIT_REASON` is not set to ``0``.

-------------
Wake the task
-------------
#. Fix the issue in ``vending_machine.cc``:

   * When the keypad is pressed, invoke the
     :cc:`Wake() <pw::async2::Waker::Wake>` method on the ``Waker``:

     .. literalinclude:: ./checkpoint4/vending_machine.cc
        :language: cpp
        :start-at: void Keypad::Press(int key) {
        :end-at: }
        :linenos:
        :emphasize-lines: 5

     By design, the ``Wake()`` call consumes the ``Waker``, leaving it in an
     empty state. Additionally, wakers are default-constructed in an empty
     state, and moving the value means the location that is moved from is reset
     to an empty state. If you invoke ``Wake()`` on an empty ``Waker``, the call
     is a no-op.

#. Verify the fix:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to insert a coin.

#. Press :kbd:`1` :kbd:`Enter` to select an item.

   You should see keypad input working correctly now:

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.
      1
      INF  Keypad 1 was pressed. Dispensing an item.

.. tip::

   You can also end up in a "task not waking up" state if you destroy or
   otherwise clear the ``Waker`` instance that pointed at the task to wake.
   ``LogRegisteredTasks()`` can also help here by pointing to a problem related
   to waking your task.

------------------------
Simplify with FutureCore
------------------------
The manual implementation of ``KeyPressFuture`` is complex and error-prone due
to the need to manage move semantics and pointers between the future and the
keypad, as well as having to manage a ``Waker``.

Pigweed provides :cc:`pw::async2::FutureCore` to handle these details for you.
``FutureCore`` manages the listing of futures in a linked list owned by a future
provider like the ``Keypad`` and automatically stores a ``Waker`` when returning
``Pending``.

#. Update ``vending_machine.h`` to use ``FutureCore``:

   * Include ``pw_async2/future.h``.

   * Replace the ``waker_`` member in your custom ``KeyPressFuture`` with a
     ``pw::async2::FutureCore core_``, then delete your custom move constructor
     and move assignment operator. The default move operations provided by the
     compiler will handle everything for you.

   * Make ``pw::async2::FutureCore`` a friend of ``KeyPressFuture``.

   * Replace the ``key_press_future_`` pointer in ``Keypad`` with a
     ``pw::async2::FutureList<&KeyPressFuture::core_> futures_``. By templating
     it on a member pointer to a concrete future's ``FutureCore``, the
     ``FutureList`` can retrieve the ``KeyPressFuture`` itself.

   * To set the waker's wait reason, define a
     ``static constexpr const char kWaitReason[]`` member in
     ``KeyPressFuture`` with the wait reason string.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint5/vending_machine.h
         :language: cpp
         :start-after: class Keypad;
         :end-before: // The main task that drives the vending machine.
         :linenos:
         :emphasize-lines: 16,18,24,50-51

#. Update ``vending_machine.cc`` to implement the simplified logic:

   * Delete all the manual move logic.

   * In the ``KeyPressFuture`` constructor, initialize the list and push
     the future to the keypad's ``futures_`` list.

   * In ``Keypad::Press``, use ``futures_.PopIfAvailable()`` to retrieve the
     active future, then set its ``key_pressed_`` member and wake it.

   * Rename your ``KeyPressFuture::Pend`` to ``DoPend``, then add a simple
     ``Pend`` method that delegates to ``core_.DoPend(*this, cx)``.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint5/vending_machine.cc
         :language: cpp
         :start-at: KeyPressFuture::KeyPressFuture(Keypad& keypad)
         :end-before: pw::async2::Poll<> VendingMachineTask::DoPend
         :linenos:
         :emphasize-lines: 3-4,7-9,11,22-26

----------
Next steps
----------
Continue to :ref:`module-pw_async2-codelab-step4` to learn how to manage the
rapidly increasing complexity of your code.

.. _module-pw_async2-codelab-step3-checkpoint:

----------
Checkpoint
----------
At this point, your code should look similar to the files below.

.. tab-set::

   .. tab-item:: main.cc

      .. literalinclude:: ./checkpoint5/main.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.cc

      .. literalinclude:: ./checkpoint5/vending_machine.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.h

      .. literalinclude:: ./checkpoint5/vending_machine.h
         :language: cpp
         :start-after: // the License.
