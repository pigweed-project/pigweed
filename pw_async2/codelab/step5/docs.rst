.. _module-pw_async2-codelab-step5:

============================
5. Communicate between tasks
============================
.. pigweed-module-subpage::
   :name: pw_async2

Your vending machine is ready to handle more functionality. In this step,
you'll write code to handle a dispenser mechanism. The vending machine uses a
motor to push the selected product into a chute. A sensor detects when the item
has dropped, then the motor is turned off.

The dispenser mechanism is complex enough to merit a task of its own. The
``VendingMachineTask`` will notify the new ``DispenserTask`` about which items
to dispense, and ``DispenserTask`` will send confirmation back.

---------------------------
Set up the item drop sensor
---------------------------
The ``ItemDropSensor`` class that we've provided is similar to the
``CoinSlot`` and ``Keypad`` classes.

#. Integrate the item drop sensor into ``main.cc``:

   * Include ``item_drop_sensor.h``

   * Create a global ``codelab::ItemDropSensor item_drop_sensor`` instance

   * Call the item drop sensor's ``Drop()`` method in the interrupt handler
     (``item_drop_sensor_isr()``)

   When an item is dispensed successfully, the item drop sensor triggers an
   interrupt, which is handled by the ``item_drop_sensor_isr()`` function.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/main.cc
         :language: cpp
         :linenos:
         :start-after: // the License.
         :end-before: int main() {
         :emphasize-lines: 3,11,26

-----------------------------
Set up communication channels
-----------------------------
We'll be adding a new ``DispenserTask`` soon. To get ready for that, let's set
up communications channels between ``VendingMachineTask`` and the new task.

The mechanism through which async tasks communicate in ``pw_async2`` is
``pw::async2::Channel``. At its core, a channel is a asynchronous, threadsafe
queue that can have one or more senders and one or more receivers.

For our vending machine, we'll use two channels:

* A ``Channel<int>`` for the vending machine to send dispense requests (item
  numbers) to the dispenser task.

* A ``Channel<bool>`` for the dispenser task to send dispense responses (success
  or failure) back to the vending machine task.

#. Update ``vending_machine.h`` to allow communications:

   * Include ``pw_async2/channel.h``

   * Update your ``VendingMachineTask`` to accept and store a
     ``pw::async2::Sender<int>`` to send requests and a
     ``pw::async2::Receiver<bool>`` to receive responses.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :linenos:
         :lines: 1-24,27-38
         :emphasize-lines: 5-6,10-11,23-24
         :start-at: class VendingMachineTask
         :end-before: class DispenserTask

#. Create the channels in ``main.cc``:

   * Define a local ``pw::async2::ChannelStorage<int, 1> dispense_requests``
     and a ``pw::async2::ChannelStorage<bool, 1> dispense_responses`` in your
     ``main()`` function. These serve as the buffers for the channels.

     .. note:: Channels optionally can dynamically allocate their storage,
       removing the need for a ChannelStorage declaration. Refer to the
       :ref:`Channel docs <module-pw_async2-channels>` for more details.

   * Create the channels themselves using the ``pw::async2::CreateSpscChannel()``
     function. ``Spsc`` means "single producer, single consumer", which is what
     we need to communicate between the two tasks.

     ``CreateSpscChannel`` returns a tuple of three items: a handle to the
     channel, a sender, and a receiver.

     The channel handle is used if you need to close the channel early, and to
     create additional senders and receivers if the channel supports it.

     Since we have an SPSC channel, no new senders or receivers can be created,
     and we don't need to close our channels, we don't need the handle here.
     To indicate this, we immediately call ``Release`` on the two handles we
     created, telling the channel that it should manage its lifetime
     automatically via its active senders and receivers.

   * Pass the sender for the requests channel and the receiver for the
     responses channel to the ``VendingMachineTask`` constructor

   * Create a ``DispenserTask`` instance (we'll define this in the next
     section), passing the drop sensor and the opposite sender/receiver pair
     to its constructor

   * Post the new ``DispenserTask`` to the dispatcher

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/main.cc
         :language: cpp
         :linenos:
         :start-at: int main() {
         :emphasize-lines: 5-13,17-18,21-24

.. topic:: Lifecycle of an async2 Channel

   Channels begin open when created through ``CreateSpscChannel``,
   ``CreateMpscChannel``, ``CreateSpmcChannel``, or ``CreateMpmcChannel``.
   Each of these functions returns a handle to the channel, optionally alongside
   a sender and/or a receiver depending on the channel type.

   Senders and receivers are movable but not copyable. If a specific channel
   type supports multiple senders or receivers, they are created by calling
   ``CreateSender()`` or ``CreateReceiver()`` on the handle.

   In addition to creating senders and receivers, the channel handle has a
   ``Close()`` function which is used to close the channel early, bypassing its
   natural lifecycle. Channel handles are copyable, so they can be handed out
   to different parts of the system that need to manage the channel.

   As long as handles exist, the channel remains open. When the last handle is
   released or destroyed, the channel's lifecycle is controlled by the active
   senders and receivers. As long as at least one sender and one receiver
   exists, the channel is open. When either side fully hangs up, the channel
   closes. Receivers can drain the channel of all buffered messages even after
   the senders have hung up.

   Since channel handles keep a channel alive, it is important to get rid of
   them when they're no longer needed. The ``Release()`` function is used for
   this purpose. It is common to immediately release the handle returned by a
   channel creation function if you don't need to close the channel early.

-----------------------------
Create the new dispenser task
-----------------------------
The ``DispenserTask`` will turn the dispenser motor on and off in response to
dispense requests from the ``VendingMachineTask``.

#. Declare a new ``DispenserTask`` class in ``vending_machine.h``:

   * The ``DispenserTask`` constructor should accept references to the drop
     sensor, the requests channel receiver, and the responses channel sender
     as args.

   * Create a ``State`` enum member with these states:

     * ``kIdle``: Waiting for a dispense request (motor is off)

     * ``kDispensing``: Actively dispensing an item (motor is on)

     * ``kReportDispenseSuccess``: Waiting to report success (motor is off)

   * Create a data member to store the current state

   * Remember to define your ``DoPend`` override

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :start-at: class DispenserTask
         :end-before: }  // namespace codelab
         :linenos:

#. Implement the dispenser's state machine in ``vending_machine.cc``:

   * In ``kIdle``, call ``Receive`` to get a future for the next dispense
     request and pend it to completion, then turn on the dispenser motor
     using ``SetDispenserMotorState``, which is provided in ``hardware.h``.

   * In ``kDispensing``, wait for the item drop sensor to trigger then turn
     off the dispenser motor and transition to ``kReportDispenseSuccess``.

   * In ``kReportDispenseSuccess``, use the response channel sender to send
     ``true`` to the ``VendingMachineTask`` and transition to ``kIdle``.

   .. note::

      Dispensing can't fail yet. We'll get to that later.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> DispenserTask::DoPend
         :end-before: }  // namespace codelab
         :linenos:

-------------------------
Communicate between tasks
-------------------------
Now, let's get ``VendingMachineTask`` communicating with ``DispenserTask``.

Instead of just logging when a purchase is made, ``VendingMachineTask`` will
send the selected item to the ``DispenserTask`` through the dispense requests
channel. Then it will wait for a response with the dispense responses channel.

#. Prepare ``VendingMachineTask`` for comms in ``vending_machine.h``:

   * Add new states: ``kAwaitingDispenseIdle`` (dispenser is ready for a request)
     and ``kAwaitingDispense`` (waiting for dispenser to finish dispensing an
     item)

   * Define member variables for the request send and response receive futures.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :start-at: class VendingMachineTask
         :end-before: class DispenserTask
         :linenos:
         :emphasize-lines: 25-26,34-35

#. Update the vending machine task's state machine in ``vending_machine.cc``:

   * Transition the ``kAwaitingSelection`` state to ``kAwaitingDispenseIdle``,
     storing the selected item in a member variable.

   * Implement the ``kAwaitingDispenseIdle`` and ``kAwaitingDispense`` states:

     * In ``kAwaitingDispenseIdle``, send the selected item to the
       ``DispenserTask`` using the dispense requests sender.

     * In ``kAwaitingDispense``, wait for a response from the ``DispenserTask``
       and log whether the dispense was successful.

       Once complete, you can return to ``kWelcome`` to start the process over.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-at: case kAwaitingSelection: {
         :end-before: pw::async2::Poll<> DispenserTask::DoPend
         :linenos:
         :emphasize-lines: 8-11,22-33,35-53

------------------
Test the dispenser
------------------
#. Run the app:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to input a coin.

#. Press :kbd:`1` :kbd:`Enter` to make a selection.

#. Press :kbd:`i` :kbd:`Enter` to trigger the item drop sensor, signaling that
   the item has finished dispensing.

   You should see the vending machine display it's welcome message again.

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      INF  Dispenser task awake
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.
      1
      INF  Keypad 1 was pressed. Dispensing an item.
      INF  Dispenser task awake
      INF  [Motor for item 1 set to On]
      i
      INF  Dispenser task awake
      INF  [Motor for item 1 set to Off]
      INF  Dispense succeeded. Thanks for your purchase!
      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.

Congratulations! You now have a fully functioning vending machine! Or do you…?

------------------------------------------
Handle unexpected situations with timeouts
------------------------------------------
What if you press the wrong button and accidentally buy an out-of-stock item?
As of now, the dispenser will just keep running forever. The vending
machine will eat your money while you go hungry.

Let's fix this. We can add a timeout to the ``kDispensing`` state. If the
``ItemDropSensor`` hasn't triggered after a certain amount of time, then
something has gone wrong. The ``DispenserTask`` should stop the motor and tell
the ``VendingMachineTask`` what happened. Pigweed provides a
:cc:`Timeout <pw::async2::Timeout>` helper which works with various common
future types.

#. Prepare ``DispenserTask`` to support timeouts in ``vending_machine.h``:

   * Include the header that provides timeout-related features:

     * ``pw_async2/future_timeout.h``

     .. dropdown:: Hint

        .. literalinclude:: ./checkpoint2/vending_machine.h
           :language: cpp
           :start-after: // the License.
           :end-before: namespace codelab {
           :linenos:
           :emphasize-lines: 11

   * Create a new ``kReportDispenseFailure`` state to represent dispense
     failures, a new ``kDispenseTimeout`` data member in ``DispenserTask`` that
     holds the timeout duration (``std::chrono::seconds(5)`` is a good value),
     and update the ``drop_future_`` member to be a
     :cc:`ValueFutureWithTimeout <pw::async2::ValueFutureWithTimeout>`:

     .. dropdown:: Hint

        .. literalinclude:: ./checkpoint2/vending_machine.h
           :language: cpp
           :start-at: class DispenserTask
           :end-before: }  // namespace codelab
           :linenos:
           :emphasize-lines: 17,27,31

     For testing purposes, make sure that the timeout period is long enough for
     a human to respond.

#. Implement the timeout support in ``vending_machine.cc``:

   * When you start dispensing an item (in your transition from ``kIdle`` to
     ``kDispensing``), initialize ``drop_future_`` using
     :cc:`Timeout <pw::async2::Timeout>`.

     ``Timeout`` takes the future to wrap and the timeout duration as arguments.

   * In the ``kDispensing`` state, pend the ``drop_future_``.

   * Check the result of the future:

     * If the result is OK, then the item dropped successfully.
       Proceed to the ``kReportDispenseSuccess`` state.

     * If the result status is ``DeadlineExceeded``, then the timeout occurred.
       Proceed to the ``kReportDispenseFailure`` state.

     * In either case, be sure to turn off the motor and clear the dispense
       request.

   * Handle the dispense failure state.

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint2/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> DispenserTask::DoPend
         :end-at: }  // namespace codelab
         :linenos:
         :emphasize-lines: 21-22,27-31,48-59

--------------------------------
Test the dispenser with timeouts
--------------------------------
#. Run the app:

   .. code-block:: console

      bazelisk run //pw_async2/codelab

#. Press :kbd:`c` :kbd:`Enter` to input a coin.

# Press :kbd:`1` :kbd:`Enter` to make a selection.

#. Wait for the timeout.

   After 5 seconds you should see a message about the dispense failing.

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      INF  Dispenser task awake
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.
      1
      INF  Keypad 1 was pressed. Dispensing an item.
      INF  [Motor for item 1 set to On]
      INF  [Motor for item 1 set to Off]
      INF  Dispense failed. Choose another selection.

#. Try again, but this time press :kbd:`i` :kbd:`Enter` in 5 seconds or less
   so that the dispense succeeds.

----------
Next steps
----------
Congratulations! You completed the codelab. Click **DISPENSE PRIZE** to
retrieve your prize.

.. raw:: html

   <div style="margin-bottom:1em">
     <button id="dispense">DISPENSE PRIZE</button>
     <p id="prize" style="display:none">🍭</p>
   </div>
   <script>
     document.querySelector("#dispense").addEventListener("click", (e) => {
       e.target.disabled = true;
       document.querySelector("#prize").style.display = "block";
     });
   </script>

You now have a solid foundation in ``pw_async2`` concepts, and quite a bit
of hands-on experience with the framework. Try building something yourself
with ``pw_async2``! As always, if you get stuck, or if anything is unclear,
or you just want to run some ideas by us, :ref:`we would be happy to chat
<module-pw_async2-codelab-help>`.

.. _module-pw_async2-codelab-step5-checkpoint:

----------
Checkpoint
----------
At this point, your code should look similar to the files below.

.. tab-set::

   .. tab-item:: main.cc

      .. literalinclude:: ./checkpoint2/main.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.cc

      .. literalinclude:: ./checkpoint2/vending_machine.cc
         :language: cpp
         :start-after: // the License.

   .. tab-item:: vending_machine.h

      .. literalinclude:: ./checkpoint2/vending_machine.h
         :language: cpp
         :start-after: // the License.
