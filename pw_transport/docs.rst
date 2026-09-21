.. _module-pw_transport:

============
pw_transport
============
.. pigweed-module::
   :name: pw_transport

.. note::

   ``pw_transport`` is still considered experimental. While the core interfaces
   are likely stable, some changes are still expected to be made around the
   edges. The documentation is also incomplete.

``pw_transport`` defines interfaces for async-native, datagram-oriented,
reliable network sockets built on :ref:`module-pw_async2`.

The core interface in ``pw_transport`` is the :cc:`ReliableDatagramSocket`: an
abstract handle to some underlying transport, used for reading and writing.
Sockets make use of :ref:`module-pw_buf` to enable zero-copy reads and writes
directly into transport buffers.

Sockets are provisioned by one of two transport interfaces:
a :cc:`ReliableDatagramConnector` which establishes them, or a
:cc:`ReliableDatagramListener` which accepts them from a peer.

-----
Usage
-----
Once a :cc:`ReliableDatagramSocket` is obtained from a
:cc:`ReliableDatagramConnector` or :cc:`ReliableDatagramListener`, tasks read
and write messages asynchronously. Writes use a two-phase reservation model:
:cc:`ReliableDatagramSocket::ReserveWrite` returns a future that resolves to a
:cc:`WriteReservation` once buffer space is available. If the underlying
transport's outgoing queue is full, the future pends, naturally applying
backpressure to the sender.

For example, consider a task which sends a message and waits for the peer's
response:

.. tab-set::

   .. tab-item:: Standard polling

      .. code-block:: cpp

         class RequestResponseTask : public pw::async2::Task {
          public:
           explicit RequestResponseTask(
               pw::transport::ReliableDatagramConnector& connector)
               : connector_(connector) {}

          private:
           enum class State {
             kConnecting,
             kWriting,
             kReading,
           };

           pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
             while (true) {
               switch (state_) {
                 case State::kConnecting: {
                   if (!connect_future_.is_pendable()) {
                     connect_future_ = connector_.Connect();
                   }
                   PW_AWAIT(auto result, connect_future_, cx);
                   if (!result.ok()) {
                     return pw::async2::Ready();
                   }
                   socket_ = std::move(*result);
                   state_ = State::kWriting;
                   break;
                 }

                 case State::kWriting: {
                   // Reserve space in an outgoing transport buffer.
                   if (!reserve_future_.is_pendable()) {
                     reserve_future_ = socket_.ReserveWrite(kPayload.size());
                   }
                   PW_AWAIT(auto reservation, reserve_future_, cx);
                   if (!reservation.has_value()) {
                     return pw::async2::Ready();
                   }

                   // Write the payload directly into the transport buffer without
                   // intermediate storage or copying.
                   SerializePayload(pw::ByteSpan(*reservation));
                   if (!reservation->Commit(kPayload.size())) {
                     return pw::async2::Ready();
                   }
                   state_ = State::kReading;
                   break;
                 }

                 case State::kReading: {
                   // Read the peer's response.
                   if (!read_future_.is_pendable()) {
                     read_future_ = socket_.Read();
                   }
                   PW_AWAIT(pw::ConstBuf response, read_future_, cx);
                   if (response != nullptr) {
                     ProcessResponse(response);
                   }
                   return pw::async2::Ready();
                 }
               }
             }
           }

           pw::transport::ReliableDatagramConnector& connector_;
           pw::transport::ReliableDatagramSocket socket_;
           State state_ = State::kConnecting;
           pw::transport::ReliableDatagramConnector::ConnectFuture connect_future_;
           pw::transport::ReserveWriteFuture reserve_future_;
           pw::transport::ReadFuture read_future_;
         };

   .. tab-item:: C++20 coroutines

      .. code-block:: cpp

         pw::async2::Coro<pw::Status> SendAndReceive(
             pw::async2::CoroContext&,
             pw::transport::ReliableDatagramConnector& connector) {
           PW_CO_TRY_ASSIGN(pw::transport::ReliableDatagramSocket socket,
                            co_await connector.Connect());

           // Reserve space in an outgoing transport buffer.
           std::optional<pw::transport::WriteReservation> reservation =
               co_await socket.ReserveWrite(kPayload.size());
           if (!reservation.has_value()) {
             co_return pw::Status::Aborted();
           }

           // Write the payload directly into the transport buffer without
           // intermediate storage or copying.
           SerializePayload(pw::ByteSpan(*reservation));
           if (!reservation->Commit(kPayload.size())) {
             co_return pw::Status::Aborted();
           }

           // Read the peer's response.
           pw::ConstBuf response = co_await socket.Read();
           if (response == nullptr) {
             co_return pw::Status::Aborted();
           }

           ProcessResponse(response);
           co_return pw::OkStatus();
         }

Addressing
==========
``pw_transport`` does not define an addressing layer, as this is inherently
platform and protocol dependent. Where required, addressing should be built into
implementations of ``ReliableDatagramListener`` and
``ReliableDatagramConnector``. For example:

.. code-block:: cpp

   class FramedTcpConnector : public pw::transport::ReliableDatagramConnector {
    public:
     FramedTcpConnector(std::string_view host, uint16_t port);

     pw::transport::ReliableDatagramConnector::ConnectFuture Connect() override;
   };

Instances of these objects can then be configured for specific endpoints and
passed around into APIs that consume ``pw_transport``.

------------
Implementing
------------
Authors writing an implementation for your own transport are required to
implement three virtual interfaces:

- :cc:`ReliableDatagramListener`: the listening side of the transport.
- :cc:`ReliableDatagramConnector`: the establishing side of the transport.
- :cc:`ReliableDatagramSocketImpl`: the core socket interface backing
  ``ReliableDatagramSocket`` handles. Allocated and refcounted.

Refer to the doxygen docs for details of each of these, with requirements and
preconditions.
