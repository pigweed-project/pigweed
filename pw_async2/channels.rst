.. _module-pw_async2-channels:

========
Channels
========
.. pigweed-module-subpage::
   :name: pw_async2

Channels are the primary mechanism for communicating between asynchronous tasks
or threads in ``pw_async2``.

A channel is a fixed-capacity queue that supports multiple senders and multiple
receivers. Channels can be used between async tasks on the same dispatcher,
between tasks on different dispatchers, or between tasks and non-async code.
There are two types of channel: static channels, which have user-managed
storage, and dynamic channels which are allocated and automatically manage their
lifetimes.

.. _module-pw_async2-channels-create:

------------------
Creating a channel
------------------
Channels can be created in four configurations which control the number of
senders and receivers supported. The "multi-" configurations support up to 255
producers or consumers.

.. list-table:: Channel Configurations
   :widths: 30 10 15 15
   :header-rows: 1

   * - Name
     - Initials
     - Max Producers
     - Max Consumers
   * - Single-producer, single-consumer
     - SPSC
     - 1
     - 1
   * - Single-producer, multi-consumer
     - SPMC
     - 1
     - 255
   * - Multi-producer, single-consumer
     - MPSC
     - 255
     - 1
   * - Multi-producer, multi-consumer
     - MPMC
     - 255
     - 255

Examples of creating each channel type are shown below.

.. tab-set::

   .. tab-item:: SPSC

      .. code-block:: cpp

         // Single-producer, single-consumer

         #include "pw_async2/channel.h"

         // Create storage for a static channel with a capacity of 10 integers.
         // The storage must outlive the channel for which it is used.
         pw::async2::ChannelStorage<int, 10> storage;

         // Create a channel using the storage.
         //
         // In this example, we create a single-producer, single-consumer channel and
         // are given the sole sender and receiver.
         auto [channel, sender, receiver] = pw::async2::CreateSpscChannel(storage);

         // Hand the sender and receiver to various parts of the system.
         MySenderTask sender_task(std::move(sender));
         MyReceiverTask receiver_task(std::move(receiver));

         // You can hold onto the channel handle if you want to use it to
         // manually close the channel before all senders and receivers have
         // completed.
         //
         // If you want the channel to close automatically once either end hangs
         // up, you should `Release` the handle immediately to disassociate its
         // reference to the channel.
         channel.Release();

   .. tab-item:: SPMC

      .. code-block:: cpp

         // Single-producer, multi-consumer

         #include "pw_async2/channel.h"

         // Create storage for a static channel with a capacity of 10 integers.
         // The storage must outlive the channel for which it is used.
         pw::async2::ChannelStorage<int, 10> storage;

         // Create a channel using the storage.
         //
         // In this example, we create a single-producer, multi-consumer channel and
         // are given the sole sender. Receivers are created from the channel handle.
         auto [channel, sender] = pw::async2::CreateSpmcChannel(storage);

         // Hand the sender and receiver to various parts of the system.
         MySenderTask sender_task(std::move(sender));
         MyReceiverTask receiver_task_1(channel.CreateReceiver());
         MyReceiverTask receiver_task_2(channel.CreateReceiver());

         // You can hold onto the channel handle if you want to use it to
         // manually close the channel before all senders and receivers have
         // completed.
         //
         // If you want the channel to close automatically once either end hangs
         // up, you should `Release` the handle after all desired receivers are
         // created to disassociate its reference to the channel.
         channel.Release();

   .. tab-item:: MPSC

      .. code-block:: cpp

         // Multi-producer, single-consumer

         #include "pw_async2/channel.h"

         // Create storage for a static channel with a capacity of 10 integers.
         // The storage must outlive the channel for which it is used.
         pw::async2::ChannelStorage<int, 10> storage;

         // Create a channel using the storage.
         //
         // In this example, we create a multi-producer, single-consumer channel and
         // are given the sole receiver. Senders are created from the channel handle.
         auto [channel, receiver] = pw::async2::CreateMpscChannel(storage);

         // Hand the sender and receiver to various parts of the system.
         MySenderTask sender_task_1(channel.CreateSender());
         MySenderTask sender_task_2(channel.CreateSender());
         MyReceiverTask receiver_task(std::move(receiver));

         // You can hold onto the channel handle if you want to use it to
         // manually close the channel before all senders and receivers have
         // completed.
         //
         // If you want the channel to close automatically once either end hangs
         // up, you should `Release` the handle after all desired senders are
         // created to disassociate its reference to the channel.
         channel.Release();

   .. tab-item:: MPMC

      .. code-block:: cpp

         // Multi-producer, multi-consumer

         #include "pw_async2/channel.h"

         // Create storage for a static channel with a capacity of 10 integers.
         // The storage must outlive the channel for which it is used.
         pw::async2::ChannelStorage<int, 10> storage;

         // Create a channel using the storage.
         //
         // In this example, we create a multi-producer, multi-consumer channel.
         // Both senders and receivers are created from the channel handle.
         pw::async2::MpmcChannelHandle<int> channel =
             pw::async2::CreateMpmcChannel(storage);

         // Hand the sender and receiver to various parts of the system.
         MySenderTask sender_task_1(channel.CreateSender());
         MySenderTask sender_task_2(channel.CreateSender());
         MyReceiverTask receiver_task_1(channel.CreateReceiver());
         MyReceiverTask receiver_task_2(channel.CreateReceiver());

         // You can hold onto the channel handle if you want to use it to
         // manually close the channel before all senders and receivers have
         // completed.
         //
         // If you want the channel to close automatically once either end hangs
         // up, you should `Release` the handle after all desired senders and
         // receivers are created to disassociate its reference to the channel.
         channel.Release();

.. _module-pw_async2-channels-handles:

---------------
Channel handles
---------------
Each channel creation function returns a handle to the channel. This handle is
used for two operations:

1. Creating senders and receivers, if allowed by the channel configuration
   (``CreateSender``, ``CreateReceiver``).

2. Forcefully closing the channel while senders and receivers are still active
   (``Close``).

Handles are movable and copyable, so they can be given to any parts of the
system which need to perform these operations.

As long as any handle to a channel is active, the channel will not automatically
close. If the system relies on the channel closing (for example, a receiving
task reading values until a ``std::nullopt``), it is essential to ``Release``
all handles once you are done creating senders/receivers from them.

.. _module-pw_async2-channels-txrx:

---------------------
Sending and receiving
---------------------
Senders and receivers provide asynchronous APIs for interacting with the
channel.

- ``Sender::Send(T value)``: Returns a ``Future<bool>`` which resolves to
  ``true`` when the value has been written to the channel. If the channel is
  full, the future waits until space is available. If the channel closes, the
  future resolves to ``false``.
- ``Receiver::Receive()``: Returns a ``Future<std::optional<T>>`` which waits
  until a value is available, or resolves to ``std::nullopt`` if the channel is
  closed and empty.

.. tab-set::

   .. tab-item:: Standard polling

      .. literalinclude:: examples/channel.cc
         :language: cpp
         :linenos:
         :start-after: // DOCSTAG: [pw_async2-examples-channel-manual]
         :end-before: // DOCSTAG: [pw_async2-examples-channel-manual]

   .. tab-item:: C++20 coroutines

      .. literalinclude:: examples/channel.cc
         :language: cpp
         :linenos:
         :start-after: // DOCSTAG: [pw_async2-examples-channel-coro]
         :end-before: // DOCSTAG: [pw_async2-examples-channel-coro]

.. _module-pw_async2-channels-reservesend:

ReserveSend
===========
``Sender::ReserveSend()`` is an alternative API for writing data to a channel.
Unlike the regular ``Send``, which takes a value immediately and stages it in
its future, ``ReserveSend`` allows writing a value directly into the channel
once space is available. This can be useful for values which are expensive to
construct/move or rapidly changing. By waiting for a reservation, you can defer
capturing the value until you are guaranteed to be able to send it immediately.

``ReserveSend`` returns a ``Future<std::optional<SendReservation<T>>>``. The
``SendReservation`` object is used to emplace a value directly into the channel.
If the reservation is dropped, it automatically releases the channel space.
If the channel closes, the future resolves to ``std::nullopt``.

It is possible to use both ``Send`` and ``ReserveSend`` concurrently on the same
channel.

.. tab-set::

   .. tab-item:: Standard polling

      .. code-block:: cpp

         using pw::async2::ReserveSendFuture;
         using pw::async2::Sender;

         class ReservedSenderTask : public pw::async2::Task {
          public:
           explicit ReservedSenderTask(Sender<int>&& sender)
               : sender(std::move(sender)) {}

          private:
           Poll<> DoPend(pw::async2::Context& cx) override {
             // Reserve space for a value in the channel.
             if (!reservation_future_.is_pendable()) {
               reservation_future_ = sender.ReserveSend();
             }

             PW_TRY_READY_ASSIGN(auto reservation, reservation_future_.Pend(cx));
             if (!reservation.has_value()) {
               PW_LOG_ERROR("Channel is closed");
               return;
             }

             // Emplace a value into the channel.
             reservation->Commit(42);
             return pw::async2::Ready();
           }

           Sender<int> sender;
           ReserveSendFuture<int> reservation_future_;
         };

   .. tab-item:: C++20 coroutines

      .. code-block:: cpp

         using pw::async2::Coro;
         using pw::async2::CoroContext;
         using pw::async2::Sender;

         Coro<Status> ReservedSenderExample(CoroContext&, Sender<int> sender) {
           // Wait for space to become available.
           auto reservation = co_await sender.ReserveSend();
           if (!reservation.has_value()) {
             PW_LOG_ERROR("Channel is closed");
             co_return pw::Status::FailedPrecondition();
           }

           // Emplace a value into the channel.
           reservation->Commit(42);
           co_return pw::OkStatus();
         }

.. _module-pw_async2-channels-lifetime:

----------------
Channel lifetime
----------------
A channel remains open as long as it has at least one active sender and at least
one active receiver.

- If all receivers are destroyed, the channel closes. Subsequent ``Send``
  attempts will fail (the future resolves to ``false``).
- If all senders are destroyed, the channel closes. Subsequent ``Receive`` calls
  will drain any remaining items, then resolve to ``std::nullopt``.

.. _module-pw_async2-channels-alloc:

------------------
Dynamic allocation
------------------
In systems that have dynamic allocation, you can pass an :cc:`Allocator` and
channel capacity to any of the channel creation functions to allocate a managed
channel.

The dynamic functions wrap the returned tuple in a ``std::optional``. If the
allocation fails, the optional will be empty.

.. code-block:: cpp

   #include "pw_async2/channel.h"

   constexpr size_t kCapacity = 10;
   auto result =
       pw::async2::CreateSpscChannel<int>(GetSystemAllocator(), kCapacity);
   if (!result.has_value()) {
     PW_LOG_ERROR("Out of memory");
     return;
   }

   // Hand the sender and receiver to various parts of the system.
   auto&& [channel, sender, receiver] = *result;

   // As with the static channel, release the handle once all desired senders
   // and receivers are created unless you intend to use it to manually close
   // the channel. As we created an SPSC channel here, there are no more senders
   // or receivers so we release the handle immediately.
   //
   // The channel remains allocated and open as long as any senders, receivers,
   // or futures to it are alive.
   channel.Release();

.. _module-pw_async2-channels-sync:

------------------
Synchronous access
------------------
If you need to write to a channel from a non-async context, such as a
separate thread or an interrupt handler, you can use ``TrySend``.

- :cc:`Sender::TrySend`: Attempts to send the value immediately. Returns
  a :cc:`pw::Status` indicating success.

- :cc:`Sender::TryReserveSend`: Attempts to reserve a slot in the channel
  immediately. Returns a ``std::optional<SendReservation<T>>`` which contains
  a reservation if successful, or ``std::nullopt`` if the channel is full or
  closed.

- :cc:`Sender::BlockingSend`: Blocks the running thread until the value is sent
  or an optional timeout elapses. Returns a status indicating success or
  whether the channel is closed or the operation timed out.

- :cc:`Receiver::TryReceive`: Attempts to read a value from the channel
  immediately. Returns a ``pw::Result<T>`` containing the value if successful,
  or an error if the channel is empty or closed.

- :cc:`Receiver::BlockingReceive`: Blocks the running thread until a value is
  received or an optional timeout elapses. Returns a ``pw::Result<T>``
  containing either the value read or the error in case of timeout or channel
  closure.

-----------
Size report
-----------
See the :ref:`pw_async2 channels size report
<module-pw_async2-channels-size-report>` for code size information.
