.. _module-pw_async2-codelab-step4:

======================
4. Use a state machine
======================
.. pigweed-module-subpage::
   :name: pw_async2

The ``DoPend()`` implementation in your ``VendingMachineTask`` now:

* Displays a welcome message
* Prompts the user to insert a coin… unless they have been prompted already
* Waits for the user to insert a coin… unless coins have been inserted already
* Waits for the user to select an item with the keypad

Implementing ``DoPend()`` like this is valid, but you can imagine the chain of
checks growing ever longer as the complexity increases. You'd end up with a
long list of specialized conditional checks to skip the early stages before you
handle the later stages. It's also not ideal that you can't process keypad input
while waiting for coins to be inserted.

To manage this complexity, we recommend explicitly structuring your tasks as
state machines.

-------------------------------------
Refactor your task as a state machine
-------------------------------------
#. Set up the state machine data in ``vending_machine.h``:

   * Declare a private ``State`` enum in ``VendingMachineTask`` that represents
     all possible states:

     * ``kWelcome`` (to explicitly handle the welcome message)

     * ``kAwaitingPayment``

     * ``kAwaitingSelection``

   * Declare a private ``State state_`` data member that stores the current
     state

   * Initialize ``state_`` in the ``VendingMachineTask`` constructor

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.h
         :language: cpp
         :start-at: class VendingMachineTask
         :end-before: }  // namespace codelab
         :linenos:
         :emphasize-lines: 8,21-26

#. Refactor the ``DoPend()`` implementation in ``vending_machine.cc`` as an
   explicit state machine:

   * Handle the ``kWelcome``, ``kAwaitingPayment``, and ``kAwaitingSelection``
     states (as well as the transitions between states)

   .. dropdown:: Hint

      .. literalinclude:: ./checkpoint1/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> VendingMachineTask::DoPend
         :end-before: }  // namespace codelab
         :linenos:

   This isn't the only way to do it, but it is perhaps the easiest to understand
   since there isn't a lot of hidden machinery.

.. topic:: State machines in C++

   Two other options for implementing a state machine in C++ include:

   * Define a type tag for each state, use ``std::variant`` to represent the
     possible states, and use ``std::visit`` to dispatch to a handler for each
     state. The compiler effectively generates the switch statement for you at
     compile-time.

   * Use runtime dispatch through a function pointer to handle each state.
     Usually you create a base class that defines a virtual function, and you
     derive each state by overriding the virtual function.

------------------------
Problem with linear flow
------------------------
Even with the task refactored as an explicit state machine, there's still a
problem with the simple linear flow we've implemented so far. For example,
what happens when you make a selection before inserting coins?

#. Press :kbd:`1` :kbd:`c` :kbd:`Enter`.

   You should see output like this:

   .. code-block:: none

      INF  Welcome to the Pigweed Vending Machine!
      INF  Please insert a coin.
      1
      c
      INF  Received 1 coin.
      INF  Please press a keypad key.

After pressing :kbd:`1`, nothing happens. You get no feedback, and it's not
clear whether the machine is still waiting or stuck. Ideally, the machine would
detect the keypress and tell the user that it still needs coins.

In other words, how do you make your task better at handling multiple inputs
when ``CoinFuture`` and ``KeyPressFuture`` can each only wait on one thing?

The answer is to use the :cc:`Select <pw::async2::Select>` helper function
to wait on multiple futures, returing a result whenever the first future
is ``Ready()``.

------------------------
Wait on a set of futures
------------------------
When multiple states use the same set of futures (e.g. ``kAwaitingPayment``
and ``kAwaitingSelection`` both use ``CoinSlot`` and ``Keypad``) it's efficient
to combine them into a single future that pends on both.

For the vending machine, we can unify coin insertions and keypad selections
using :cc:`SelectFuture <pw::async2::SelectFuture>`, which completes when the
first of the input futures completes, returning an
:cc:`OptionalTuple <pw::OptionalTuple>` of results.

#. Include ``"pw_async2/select.h"`` in ``vending_machine.h``.

#. Add a ``select_future_`` data member to ``VendingMachineTask`` to hold both
   a ``CoinFuture`` and a ``KeyPressFuture``:

   .. literalinclude:: ./checkpoint2/vending_machine.h
      :language: cpp
      :start-at: CoinSlot& coin_slot_;
      :end-at: pw::async2::SelectFuture<CoinFuture, KeyPressFuture> select_future_;
      :linenos:
      :emphasize-lines: 3

   Just as with the separate futures, we store the ``SelectFuture`` as a member
   to hold the state of the operations across ``Pend()`` calls.

   .. note::
      ``SelectFuture`` automatically resets all of its child futures when it
      returns ``Ready()``, so we must assign a new one (using
      :cc:`Select <pw::async2::Select>`) when we want to wait again.

#. Update the ``VendingMachineTask`` implementation in ``vending_machine.cc``:

   *  Refactor ``DoPend()`` to use ``select_future_``:

      .. literalinclude:: ./checkpoint2/vending_machine.cc
         :language: cpp
         :start-at: pw::async2::Poll<> VendingMachineTask::DoPend
         :end-before: }  // namespace codelab
         :linenos:

Grab yourself a ``$FAVORITE_BEVERAGE`` and let's dig into our new ``DoPend()``
implementation.

In the ``kAwaitingPayment`` state, we first check if ``select_future_`` can be
pended. If not, we initialize it using :cc:`Select <pw::async2::Select>`,
providing a new ``CoinFuture`` and ``KeyPressFuture``.

.. literalinclude:: ./checkpoint2/vending_machine.cc
   :language: cpp
   :start-at: if (!select_future_.is_pendable()) {
   :end-at: }
   :linenos:

Then, we pend the ``select_future_``:

.. literalinclude:: ./checkpoint2/vending_machine.cc
   :language: cpp
   :start-at: PW_TRY_READY_ASSIGN(auto result, select_future_.Pend(cx));
   :end-at: PW_TRY_READY_ASSIGN(auto result, select_future_.Pend(cx));
   :linenos:

Previously, we used ``PW_TRY_READY_ASSIGN`` to wait for a single future. Here,
we use it to wait for both the ``CoinFuture`` and ``KeyPressFuture`` at once.
When the first of these futures completes, ``SelectFuture`` also completes and
returns its result.

The type of this result is a ``pw::OptionalTuple<unsigned, int>``, where
``unsigned`` and ``int`` are the return types of ``CoinFuture`` and
``KeyPressFuture``, respectively. This utility behaves similarly to a
``std::tuple<std::optional<unsigned>, std::optional<int>>``, but is optimized
for space by using a bitfield to track presence.

We then check which future completed using ``has_value()``. There are two
overloads of this method:

- ``has_value<size_t>()``: Returns true if the future at the given index
  (in the order they were passed to ``Select()``) completed.
- ``has_value<T>()``: Returns true if the future with return type ``T``
  completed. This can only be used if each future has a distinct return type.

Once we know which future completed, we can retrieve its result using
``result.value<T>()`` (or ``result.value<size_t>()``) and act on it:

.. literalinclude:: ./checkpoint2/vending_machine.cc
   :language: cpp
   :start-at: if (result.has_value<0>()) {
   :end-before: break;
   :linenos:

The logic for ``kAwaitingSelection`` is similar, but we first check for
the result of ``KeyPressFuture``:

.. literalinclude:: ./checkpoint2/vending_machine.cc
   :language: cpp
   :start-at: if (result.has_value<1>()) {
   :end-at: break;
   :linenos:

By using ``Select``, we can now handle either coin insertions or keypad
selections no matter which state the vending machine is in.

----------
Next steps
----------
Continue to :ref:`module-pw_async2-codelab-step5` to learn how to spin up a
new task and get your tasks communicating with each other.

.. _module-pw_async2-codelab-step4-checkpoint:

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
