.. _module-pw_transfer:

===========
pw_transfer
===========
.. pigweed-module::
   :name: pw_transfer

``pw_transfer`` is a reliable data transfer protocol which runs on top of
Pigweed RPC.

-----
Usage
-----

C++
===

Transfer thread
---------------
To run transfers as either a client or server (or both), a dedicated thread is
required. The transfer thread is used to process all transfer-related events
safely. The same transfer thread can be shared by a transfer client and service
running on the same system.

.. note::

   All user-defined transfer callbacks (i.e. the virtual interface of a
   :cc:`Handler <pw::transfer::Handler>` or completion function in a transfer
   client) will be invoked from the transfer thread's context.

In order to operate, a transfer thread requires two buffers:

- The first is a *chunk buffer*. This is used to stage transfer packets received
  by the RPC system to be processed by the transfer thread. It must be large
  enough to store the largest possible chunk the system supports.

- The second is an *encode buffer*. This is used by the transfer thread to
  encode outgoing RPC packets. It is necessarily larger than the chunk buffer.
  Typically, this is sized to the system's maximum transmission unit at the
  transport layer.

A transfer thread is created by instantiating a
:cc:`Thread <pw::transfer::Thread>`. This class derives from
``pw::thread::ThreadCore``, allowing it to directly be used when creating a
system thread. Refer to :ref:`module-pw_thread-thread-creation` for additional
information.

**Example thread configuration**

.. code-block:: cpp

   #include "pw_transfer/transfer_thread.h"

   namespace {

   // The maximum number of concurrent transfers the thread should support as
   // either a client or a server. These can be set to 0 (if only using one or
   // the other).
   constexpr size_t kMaxConcurrentClientTransfers = 5;
   constexpr size_t kMaxConcurrentServerTransfers = 3;

   // The maximum payload size that can be transmitted by the system's
   // transport stack. This would typically be defined within some transport
   // header.
   constexpr size_t kMaxTransmissionUnit = 512;

   // The maximum amount of data that should be sent within a single transfer
   // packet. By necessity, this should be less than the max transmission unit.
   //
   // pw_transfer requires some additional per-packet overhead, so the actual
   // amount of data it sends may be lower than this.
   constexpr size_t kMaxTransferChunkSizeBytes = 480;

   // Buffers for storing and encoding chunks (see documentation above).
   std::array<std::byte, kMaxTransferChunkSizeBytes> chunk_buffer;
   std::array<std::byte, kMaxTransmissionUnit> encode_buffer;

   pw::transfer::Thread<kMaxConcurrentClientTransfers,
                        kMaxConcurrentServerTransfers>
       transfer_thread(chunk_buffer, encode_buffer);

   }  // namespace

   // pw::transfer::TransferThread is the generic, non-templated version of the
   // Thread class. A Thread can implicitly convert to a TransferThread.
   pw::transfer::TransferThread& GetSystemTransferThread() {
     return transfer_thread;
   }

.. _pw_transfer-transfer-server:

Transfer server
---------------
``pw_transfer`` provides an RPC service for running transfers through an RPC
server.

To know how to read data from or write data to device, a
:cc:`Handler <pw::transfer::Handler>` interface is defined
(``pw_transfer/public/pw_transfer/handler.h``). Transfer handlers represent a
transferable resource, wrapping a stream reader and/or writer with
initialization and completion code. Custom transfer handler implementations
should derive from :cc:`ReadOnlyHandler <pw::transfer::ReadOnlyHandler>`,
:cc:`WriteOnlyHandler <pw::transfer::WriteOnlyHandler>`, or
:cc:`ReadWriteHandler <pw::transfer::ReadWriteHandler>` as appropriate and
override ``Prepare`` and ``Finalize`` methods if necessary.

A transfer handler should be implemented and instantiated for each unique
resource that can be transferred to or from a device. Each instantiated handler
must have a globally-unique integer ID used to identify the resource.

Handlers are registered with the transfer service. This may be done during
system initialization (for static resources), or dynamically at runtime to
support ephemeral transfer resources. A boolean is returned with registration/
unregistration to indicate success or failure.

**Example transfer handler implementation**

.. code-block:: cpp

   #include "pw_stream/memory_stream.h"
   #include "pw_transfer/transfer.h"

   // A simple transfer handler which reads data from an in-memory buffer.
   class SimpleBufferReadHandler : public pw::transfer::ReadOnlyHandler {
    public:
     SimpleReadTransfer(uint32_t resource_id, pw::ConstByteSpan data)
         : ReadOnlyHandler(resource_id), reader_(data) {
       set_reader(reader_);
     }

    private:
     pw::stream::MemoryReader reader_;
   };

Handlers may optionally implement a
:cc:`GetStatus <pw::transfer::Handler::GetStatus>` method, which allows clients
to query the status of a resource with a handler registered. The application
layer above transfer can choose how to fill and interpret this information. The
status information is ``readable_offset``, ``writeable_offset``,
``read_checksum``, and ``write_checksum``.

**Example GetStatus implementation**

.. code-block:: cpp

   Status GetStatus(uint64_t& readable_offset,
                    uint64_t& writeable_offset,
                    uint64_t& read_checksum,
                    uint64_t& write_checksum) {
     readable_offset = resource.get_size();
     writeable_offset = resource.get_writeable_offset();
     read_checksum = resource.get_crc();
     write_checksum = resource.calculate_crc(0, writeable_offset);

     return pw::OkStatus();
   }

The transfer service is instantiated with a reference to the system's transfer
thread and registered with the system's RPC server.

**Example transfer service initialization**

.. code-block:: cpp

   #include "pw_transfer/transfer.h"

   namespace {

   // In a write transfer, the maximum number of bytes to receive at one time
   // (potentially across multiple chunks), unless specified otherwise by the
   // transfer handler's stream::Writer. Should be set reasonably high; the
   // transfer will attempt to determine an optimal window size based on the
   // link.
   constexpr size_t kDefaultMaxBytesToReceive = 16384;

   pw::transfer::TransferService transfer_service(GetSystemTransferThread(),
                                                  kDefaultMaxBytesToReceive);

   // Instantiate a handler for the data to be transferred. The resource ID will
   // be used by the transfer client and server to identify the handler.
   constexpr uint32_t kMagicBufferResourceId = 1;
   char magic_buffer_to_transfer[256] = {/* ... */};
   SimpleBufferReadHandler magic_buffer_handler(kMagicBufferResourceId,
                                                magic_buffer_to_transfer);

   }  // namespace

   void InitTransferService() {
     // Register the handler with the transfer service, then the transfer service
     // with an RPC server.
     bool success = transfer_service.RegisterHandler(magic_buffer_handler);
     GetSystemRpcServer().RegisterService(transfer_service);
   }

Lifecycle callbacks
^^^^^^^^^^^^^^^^^^^
:cc:`TransferService <pw::transfer::TransferService>` supports optional
callbacks that are triggered when a server transfer starts and ends.

   **Callback implementation warnings:**

   The two lifecycle callbacks are invoked directly from the context of the
   transfer thread while it is processing events. The implementation of the
   callbacks must adhere to the following constraints:

   * No re-entrancy: Callbacks must not interact with the transfer thread,
     e.g. starting new transfers via a :cc:`Client <pw::transfer::Client>` or
     calling other transfer methods that use the same thread. Doing so can
     cause deadlock or internal state corruption.

   * Lightweight: Callbacks must be lightweight and return quickly.
     The callbacks execute on the transfer thread, any delay stalls all ongoing
     transfer packets and may block other threads that interact with the transfer
     packets, such as RPC handlers.


Dynamic handler allocation
^^^^^^^^^^^^^^^^^^^^^^^^^^
For ephemeral resources, ``pw_transfer`` provides a
:cc:`TransferHandlerAllocator <pw::transfer::TransferHandlerAllocator>` class
to simplify the management of dynamic transfer handlers. This class allocates
handlers from a :cc:`pw::Allocator`, assigns them a unique resource ID, and
registers them with the transfer service.

When a handler is allocated, a
:cc:`TransferResource <pw::transfer::TransferResource>` is returned. This is a
RAII handle that automatically unregisters and deallocates the handler when it
goes out of scope, ensuring proper cleanup of resources.

**Example of dynamic handler allocation**

.. code-block:: cpp

   #include "pw_allocator/best_fit.h"
   #include "pw_allocator/block/small_block.h"
   #include "pw_stream/memory_stream.h"
   #include "pw_transfer/handler_allocator.h"

   namespace {

   // A transfer service would be defined elsewhere in the system.
   pw::transfer::TransferService& GetSystemTransferService();

   // Size allocator for kMaxHandlers support
   constexpr size_t kMaxHandlers = 4;
   std::array<std::byte,
              pw::transfer::TransferHandlerAllocator::GetAllocatorSize(
                  kMaxHandlers, pw::allocator::SmallBlock::kBlockOverhead)>
       memory_pool_;
   pw::allocator::BestFitAllocator<pw::allocator::SmallBlock> allocator;

   pw::transfer::TransferHandlerAllocator handler_allocator(
       GetSystemTransferService(), allocator);

   }  // namespace

   void TransferEphemeralResource() {
     pw::stream::MemoryReader reader(...);

     // Allocate a handler for the reader. The handler is automatically
     // registered with the transfer service.
     pw::Result<pw::transfer::TransferResource> resource =
         handler_allocator.AllocateReader(reader);
     ASSERT_TRUE(resource.ok());

     // The resource can now be accessed by a client using the assigned
     // resource->resource_id().

     // When 'resource' goes out of scope, the handler is automatically
     // unregistered and the memory is freed.
   }

Transfer client
---------------
``pw_transfer`` provides a transfer client capable of running transfers through
an RPC client.

.. note::

   Currently, a transfer client is only capable of running transfers on a single
   RPC channel. This may be expanded in the future.

The :cc:`Client <pw::transfer::Client>` class provides the following APIs for
managing data transfers:

* :cc:`Read <pw::transfer::Client::Read>`: Reads data from a transfer server to
  the specified :cc:`stream::Writer <pw::stream::Writer>`. Invokes the provided
  callback function with the overall status of the transfer.

  Due to the asynchronous nature of transfer operations, this function will only
  return a non-OK status if it is called with bad arguments. Otherwise, it will
  return OK and errors will be reported through the completion callback.

  For using the offset parameter, please see
  :ref:`pw_transfer-nonzero-transfers`.

* :cc:`Write <pw::transfer::Client::Write>`: Writes data from a source
  :cc:`stream::Reader <pw::stream::Reader>` to a transfer server. Invokes the
  provided callback function with the overall status of the transfer.

  Due to the asynchronous nature of transfer operations, this function will only
  return a non-OK status if it is called with bad arguments. Otherwise, it will
  return OK and errors will be reported through the completion callback.

  For using the offset parameter, please see
  :ref:`pw_transfer-nonzero-transfers`.

Transfer handles
^^^^^^^^^^^^^^^^
Each transfer session initiated by a client returns a
:cc:`Handle <pw::transfer::Client::Handle>` object which is used to manage the
transfer. These handles support the following operations:

* :cc:`Cancel <pw::transfer::Client::Handle::Cancel>`: Terminates the ongoing
  transfer.
* :cc:`SetTransferSize <pw::transfer::Client::Handle::SetTransferSize>`: In a
  write transfer, indicates the total size of the transfer resource.

**Example client setup**

.. code-block:: cpp

   #include "pw_transfer/client.h"

   namespace {

   // RPC channel on which transfers should be run.
   constexpr uint32_t kChannelId = 42;

   // In a read transfer, the maximum number of bytes to receive at one time
   // (potentially across multiple chunks), unless specified otherwise by the
   // transfer's stream. Should be set reasonably high; the transfer will
   // attempt to determine an optimal window size based on the link.
   constexpr size_t kDefaultMaxBytesToReceive = 16384;

   pw::transfer::Client transfer_client(GetSystemRpcClient(),
                                        kChannelId,
                                        GetSystemTransferThread(),
                                        kDefaultMaxBytesToReceive);

   }  // namespace

   Status ReadMagicBufferSync(pw::ByteSpan sink) {
     pw::stream::Writer writer(sink);

     struct {
       pw::sync::ThreadNotification notification;
       pw::Status status;
     } transfer_state;

     Result<pw::transfer::Client::Handle> handle = transfer_client.Read(
         kMagicBufferResourceId, writer, [&transfer_state](pw::Status status) {
           transfer_state.status = status;
           transfer_state.notification.release();
         });
     if (!handle.ok()) {
       return handle.status();
     }

     // Block until the transfer completes.
     transfer_state.notification.acquire();
     return transfer_state.status;
   }

Specifying Resource Sizes
-------------------------
Transfer data is sent and received through the
:cc:`stream::Stream <pw::stream::Stream>` interface, which does not have a
concept of overall stream size. Users of transfers that are fixed-size may
optionally indicate this to the transfer client and server, which will be shared
with the transfer peer to enable features such as progress reporting.

The transfer size can only be set on the transmitting side of the transfer;
that is, the client in a ``Write`` transfer or the server in a ``Read``
transfer.

If the specified resource size is smaller than the available transferrable data,
only a slice of the data up to the resource size will be transferred. If the
specified size is equal to or larger than the data size, all of the data will
be sent.

**Setting a transfer size from a transmitting client**

.. code-block:: c++

   Result<pw::transfer::Client::Handle> handle = client.Write(...);
   if (handle.ok()) {
     handle->SetTransferSize(kMyResourceSize);
   }

**Setting a transfer size on a server resource**

  The :cc:`Handler <pw::transfer::Handler>` interface allows overriding its
  :cc:`ResourceSize <pw::transfer::Handler::ResourceSize>` function to return
  the size of its transfer resource.

.. code-block:: c++

   class MyResourceHandler : public pw::transfer::ReadOnlyHandler {
    public:
     Status PrepareRead() final;

     virtual size_t ResourceSize() const final { return kMyResourceSize; }

  };

Atomic File Transfer Handler
----------------------------
Transfers are handled using the generic :cc:`Handler <pw::transfer::Handler>`
interface. A specialized :cc:`Handler <pw::transfer::Handler>`,
:cc:`AtomicFileTransferHandler <pw::transfer::AtomicFileTransferHandler>`, is
available to handle file transfers with atomic semantics. It guarantees that the
target file of the transfer is always in a correct state. A temporary file is
written to prior to updating the target file. If any transfer failure occurs,
the transfer is aborted and the target file is either not created or not
updated.

.. _module-pw_transfer-config:

Module Configuration Options
----------------------------
The following configurations can be adjusted via compile-time configuration of
this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

* :cc:`PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES`: Default maximum number of times
  a transfer client retries sending a chunk when no response is received.
* :cc:`PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES`: Default maximum number of times
  a transfer server retries sending a chunk when no response is received.
* :cc:`PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES`: Default maximum number of
  times a transfer retries sending any chunk over its entire lifetime.
* :cc:`PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS`: Default time in milliseconds a
  transfer client waits for a chunk to arrive before retrying.
* :cc:`PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS`: Default time in milliseconds a
  transfer server waits for a chunk to arrive before retrying.
* :cc:`PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS`: Default time in milliseconds a
  transfer client waits for an initial server response before retrying.
* :cc:`PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR`: Fractional position within a
  receive window at which the window size is extended.
* :cc:`PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT`: Number of chunks to
  log at full rate before rate-limiting repetitive logs.
* :cc:`PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS`: Minimum time in milliseconds
  between repetitive logs once rate-limited.
* :cc:`PW_TRANSFER_CONFIG_LOG_LEVEL`: Log level for the ``pw_transfer`` module.
* :cc:`PW_TRANSFER_CONFIG_DEBUG_CHUNKS`: Enables logging of individual non-data
  and non-parameter chunks.
* :cc:`PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS`: Enables rate-limited logging of
  individual data and parameter chunks.
* :cc:`PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS`: Maximum time in milliseconds
  to wait for a transfer event to be processed before dropping queued events.

.. _pw_transfer-nonzero-transfers:

Non-zero Starting Offset Transfers
----------------------------------
``pw_transfer`` provides for transfers which read from or
write to a server resource starting from a point after the beginning.
Handling of read/write/erase boundaries of the resource storage backend must
be handled by the user through the transfer handler interfaces of
:cc:`GetStatus <pw::transfer::Handler::GetStatus>`,
:cc:`PrepareRead <pw::transfer::Handler::PrepareRead>`, and
:cc:`PrepareWrite <pw::transfer::Handler::PrepareWrite>`.

A resource can be read or written from a non-zero starting offset simply by
having the transfer client calling ``read()`` or ``write()`` with an offset
parameter. The offset gets included in the starting handshake.

.. note::
  The data or stream passed to ``read()`` or ``write()`` will be used as-is.
  I.e. no seeking will be applied; the user is expected to seek to the desired
  location.

On the server side, the offset is accepted, and passed to the transfer
handler's :cc:`PrepareRead <pw::transfer::Handler::PrepareRead>` or
:cc:`PrepareWrite <pw::transfer::Handler::PrepareWrite>` method. This method
must be implemented specifically by the handler in order to support the offset
transfer. The transfer handler confirms that the start offset is valid for the
read/write operation, and the server responds with the offset to confirm the
non-zero transfer operation. Older server sw will ignore the offset, so the
clients check that the server has accepted the non-zero offset during the
handshake, so users may elect to catch such errors. Clients return
``Status.UNIMPLEMENTED`` in such cases.

Due to the need to seek streams by the handler to support the non-zero offset,
it is recommended to return ``Status.RESOURCE_EXHAUSTED`` if a seek is requested
past the end of the stream.

See the :ref:`transfer handler <pw_transfer-transfer-server>` documentation for
further information about configuring resources for non-zero transfers and the
interface documentation in :cc:`Handler <pw::transfer::Handler>`.

API reference
-------------
Moved: :cc:`pw_transfer`

Python
======
.. automodule:: pw_transfer
  :members: ProgressStats, ProtocolVersion, Manager, Error

**Example**

.. code-block:: python

   import pw_transfer

   # Initialize a Pigweed RPC client; see pw_rpc docs for more info.
   rpc_client = CustomRpcClient()
   rpcs = rpc_client.channel(1).rpcs

   transfer_service = rpcs.pw.transfer.Transfer
   transfer_manager = pw_transfer.Manager(transfer_service)

   try:
     # Read the transfer resource with ID 3 from the server.
     data = transfer_manager.read(3)
   except pw_transfer.Error as err:
     print('Failed to read:', err.status)

   try:
     # Send some data to the server. The transfer manager does not have to be
     # reinitialized.
     transfer_manager.write(2, b'hello, world')
   except pw_transfer.Error as err:
     print('Failed to write:', err.status)

Typescript
==========
Provides a simple interface for transferring bulk data over pw_rpc.

**Example**

.. code-block:: typescript

   import { pw_transfer } from 'pigweedjs';
   const { Manager } from pw_transfer;

   const client = new CustomRpcClient();
   service = client.channel()!.service('pw.transfer.Transfer')!;

   const manager = new Manager(service, DEFAULT_TIMEOUT_S);

   manager.read(3, (stats: ProgressStats) => {
     console.log(`Progress Update: ${stats}`);
   }).then((data: Uint8Array) => {
     console.log(`Completed read: ${data}`);
   }).catch(error => {
     console.log(`Failed to read: ${error.status}`);
   });

   manager.write(2, textEncoder.encode('hello world'))
     .catch(error => {
       console.log(`Failed to read: ${error.status}`);
     });

Java
====
pw_transfer provides a Java client. The transfer client returns a
`ListenableFuture <https://guava.dev/releases/21.0/api/docs/com/google/common/util/concurrent/ListenableFuture>`_
to represent the results of a read or write transfer.

.. code-block:: java

   import dev.pigweed.pw_transfer.TransferClient;

   public class TheClass  {
     public void DoTransfer(MethodClient transferReadMethodClient,
                            MethodClient transferWriteMethodClient) {
       // Create a new transfer client.
       TransferClient client = new TransferClient(
           transferReadMethodClient,
           transferWriteMethodClient,
           TransferTimeoutSettings.builder()
               .setTimeoutMillis(TRANSFER_TIMEOUT_MS)
               .setMaxRetries(MAX_RETRIES)
               .build());

       // Start a read transfer.
       ListenableFuture<byte[]> readTransfer = client.read(123);

       // Start a write transfer.
       ListenableFuture<Void> writeTransfer = client.write(123, dataToWrite);

       // Get the data from the read transfer.
       byte[] readData = readTransfer.get();

       // Wait for the write transfer to complete.
       writeTransfer.get();
     }
   }

--------
Protocol
--------

Chunks
======
Transfers run as a series of *chunks* exchanged over an RPC stream. Chunks can
contain transferable data, metadata, and control parameters. Each chunk has an
associated type, which determines what information it holds and the semantics of
its fields.

The chunk is a protobuf message, whose definition can be found
:ref:`here <module-pw_transfer-proto-definition>`.

Resources and sessions
======================
Transfers are run for a specific *resource* --- a stream of data which can be
read from or written to. Resources have a system-specific integral identifier
defined by the implementers of the server-side transfer node.

The series of chunks exchanged in an individual transfer operation for a
resource constitute a transfer *session*. The session runs from its opening
chunk until either a terminating chunk is received or the transfer times out.
Sessions are assigned IDs by the client that starts them, which are unique over
the RPC channel between the client and server, allowing the server to identify
transfers across multiple clients.

Reliability
===========
``pw_transfer`` attempts to be a reliable data transfer protocol.

As Pigweed RPC is considered an unreliable communications system,
``pw_transfer`` implements its own mechanisms for reliability. These include
timeouts, data retransmissions, and handshakes.

.. note::

   A transfer can only be reliable if its underlying data stream is seekable.
   A non-seekable stream could prematurely terminate a transfer following a
   packet drop.

At present, ``pw_transfer`` requires in-order data transmission. If packets are
received out-of-order, the receiver will request that the transmitter re-send
data from the last received position.

Opening handshake
=================
Transfers begin with a three-way handshake, whose purpose is to identify the
resource being transferred, assign a session ID, and synchronize the protocol
version to use.

A read or write transfer for a resource is initiated by a transfer client. The
client sends the ID of the resource to the server alongside a unique session ID
in a ``START`` chunk, indicating that it wishes to begin a new transfer. This
chunk additionally encodes the protocol version which the client is configured
to use.

Upon receiving a ``START`` chunk, the transfer server checks whether the
requested resource is available. If so, it prepares the resource for the
operation, which typically involves opening a data stream, alongside any
additional user-specified setup. The server accepts the client's session ID,
then responds to the client with a ``START_ACK`` chunk containing the resource,
session, and configured protocol version for the transfer.

.. _module-pw_transfer-windowing:

Windowing
=========
Throughout a transfer, the receiver maintains a window of how much data it can
receive at a given time. This window is a multiple of the maximum size of a
single data chunk, and is adjusted dynamically in response to the ongoing status
of the transfer.

pw_transfer uses a congestion control algorithm similar to that of TCP
`(RFC 5681 §3.1) <https://datatracker.ietf.org/doc/html/rfc5681#section-3.1>`_,
adapted to pw_transfer's mode of operation that tunes parameters per window.

Once a portion of a window has successfully been received, it is acknowledged by
the receiver and the window size is extended. Transfers begin in a "slow start"
phase, during which the window is doubled on each ACK. This continues until the
transfer detects a packet loss or times out. Once this occurs, the window size
is halved and the transfer enters a "congestion avoidance" phase for the
remainder of its run. During this phase, successful ACKs increase the window
size by a single chunk, whereas packet loss continues to half it.

Transfer completion
===================
Either side of a transfer can terminate the operation at any time by sending a
``COMPLETION`` chunk containing the final status of the transfer. When a
``COMPLETION`` chunk is sent, the terminator of the transfer performs local
cleanup, then waits for its peer to acknowledge the completion.

Upon receving a ``COMPLETION`` chunk, the transfer peer cancels any pending
operations, runs its set of cleanups, and responds with a ``COMPLETION_ACK``,
fully ending the session from the peer's side.

The terminator's session remains active waiting for a ``COMPLETION_ACK``. If not
received after a timeout, it re-sends its ``COMPLETION`` chunk. The session ends
either following receipt of the acknowledgement or if a maximum number of
retries is hit.

.. _module-pw_transfer-proto-definition:

Server to client transfer (read)
================================
.. image:: https://www.gstatic.com/pigweed/pw_transfer/read.svg

Client to server transfer (write)
=================================
.. image:: https://www.gstatic.com/pigweed/pw_transfer/write.svg

Protocol buffer definition
==========================
.. literalinclude:: transfer.proto
  :language: protobuf
  :lines: 14-

Errors
======

Protocol errors
---------------
The following table describes the meaning of each status code when sent by the
sender or the receiver (see `Transfer roles`_).

+-------------------------+-------------------------+-------------------------+
| Status                  | Sent by sender          | Sent by receiver        |
+=========================+=========================+=========================+
| ``OK``                  | (not sent)              | All data was received   |
|                         |                         | and handled             |
|                         |                         | successfully.           |
+-------------------------+-------------------------+-------------------------+
| ``ABORTED``             | The service aborted the transfer because the      |
|                         | client restarted it. This status is passed to the |
|                         | transfer handler, but not sent to the client      |
|                         | because it restarted the transfer.                |
+-------------------------+---------------------------------------------------+
| ``CANCELLED``           | The client cancelled the transfer.                |
+-------------------------+-------------------------+-------------------------+
| ``DATA_LOSS``           | Failed to read the data | Failed to write the     |
|                         | to send. The            | received data. The      |
|                         | :cc:`Reader             | :cc:`Writer             |
|                         | <pw::stream::Reader>`   | <pw::stream::Writer>`   |
|                         | returned an error.      | returned an error.      |
+-------------------------+-------------------------+-------------------------+
| ``FAILED_PRECONDITION`` | Received chunk for transfer that is not active.   |
+-------------------------+---------------------------------------------------+
| ``INVALID_ARGUMENT``    | Received a malformed packet.                      |
+-------------------------+---------------------------------------------------+
| ``INTERNAL``            | An assumption of the protocol was violated.       |
|                         | Encountering ``INTERNAL`` indicates that there is |
|                         | a bug in the service or client implementation.    |
+-------------------------+---------------------------------------------------+
| ``PERMISSION_DENIED``   | The transfer does not support the requested       |
|                         | operation (either reading or writing).            |
+-------------------------+-------------------------+-------------------------+
| ``RESOURCE_EXHAUSTED``  | The receiver requested  | Storage is full.        |
|                         | zero bytes, indicating  |                         |
|                         | their storage is full,  |                         |
|                         | but there is still data |                         |
|                         | to send.                |                         |
+-------------------------+-------------------------+-------------------------+
| ``UNAVAILABLE``         | The service is busy with other transfers and      |
|                         | cannot begin a new transfer at this time.         |
+-------------------------+-------------------------+-------------------------+
| ``UNIMPLEMENTED``       | Out-of-order chunk was  | (not sent)              |
|                         | requested, but seeking  |                         |
|                         | is not supported.       |                         |
+-------------------------+-------------------------+-------------------------+

Transfer roles
==============
Every transfer has two participants: the sender and the receiver. The sender
transmits data to the receiver. The receiver controls how the data is
transferred and sends the final status when the transfer is complete.

In read transfers, the client is the receiver and the service is the sender. In
write transfers, the client is the sender and the service is the receiver.

Sender flow
-----------
.. mermaid::

  graph TD
    start([Client initiates<br>transfer]) -->data_request
    data_request[Receive transfer<br>parameters]-->send_chunk

    send_chunk[Send chunk]-->sent_all

    sent_all{Sent final<br>chunk?} -->|yes|wait
    sent_all-->|no|sent_requested

    sent_requested{Sent all<br>pending?}-->|yes|data_request
    sent_requested-->|no|send_chunk

    wait[Wait for receiver]-->is_done

    is_done{Received<br>final chunk?}-->|yes|done
    is_done-->|no|data_request

    done([Transfer complete])

Receiver flow
-------------
.. mermaid::

  graph TD
    start([Client initiates<br>transfer]) -->request_bytes
    request_bytes[Set transfer<br>parameters]-->wait

    wait[Wait for chunk]-->received_chunk

    received_chunk{Received<br>chunk by<br>deadline?}-->|no|request_bytes
    received_chunk-->|yes|check_chunk

    check_chunk{Correct<br>offset?} -->|yes|process_chunk
    check_chunk --> |no|request_bytes

    process_chunk[Process chunk]-->final_chunk

    final_chunk{Final<br>chunk?}-->|yes|signal_completion
    final_chunk{Final<br>chunk?}-->|no|received_requested

    received_requested{Received all<br>pending?}-->|yes|request_bytes
    received_requested-->|no|wait

    signal_completion[Signal completion]-->done

    done([Transfer complete])

Legacy protocol
===============
``pw_transfer`` was initially released into production prior to several of the
reliability improvements of its modern protocol. As a result of this, transfer
implementations support a "legacy" protocol mode, in which transfers run without
utilizing these features.

The primary differences between the legacy and modern protocols are listed
below.

- There is no distinction between a transfer resource and session --- a single
  ``transfer_id`` field represents both. Only one transfer for a given resource
  can run at a time, and it is not possible to determine where one transfer for
  a resource ends and the next begins.
- The legacy protocol has no opening handshake phase. The client initiates with
  a transfer ID and starting transfer parameters (during a read), and the data
  transfer phase begins immediately.
- The legacy protocol has no terminating handshake phase. When either end
  completes a transfer by sending a status chunk, it does not wait for the peer
  to acknowledge. Resources used by the transfer are immediately freed, and
  there is no guarantee that the peer is notified of completion.

Transfer clients request the latest transfer protocol version by default, but
may be configured to request the legacy protocol. Transfer server and client
implementations detect if their transfer peer is running the legacy protocol and
automatically switch to it if required, even if they requested a newer protocol
version. It is **strongly** unadvised to use the legacy protocol in new code.

.. _module-pw_transfer-integration-tests:

-----------------
Integration tests
-----------------
The ``pw_transfer`` module has a set of integration tests that verify the
correctness of implementations in different languages.
`Test source code <https://cs.pigweed.dev/pigweed/+/main:pw_transfer/integration_test/>`_.

To run the tests on your machine, run

.. code-block:: bash

   $ bazel test \
         pw_transfer/integration_test:cross_language_small_test \
         pw_transfer/integration_test:cross_language_medium_test

.. note:: There is a large test that tests transfers that are megabytes in size.
  These are not run automatically, but can be run manually via the
  ``pw_transfer/integration_test:cross_language_large_test`` test. These are
  VERY slow, but exist for manual validation of real-world use cases.

The integration tests permit injection of client/server/proxy binaries to use
when running the tests. This allows manual testing of older versions of
pw_transfer against newer versions.

.. code-block:: bash

   # Test a newer version of pw_transfer against an old C++ client that was
   # backed up to another directory.
   $ bazel run pw_transfer/integration_test:cross_language_medium_test -- \
       --cpp-client-binary ../old_pw_transfer_version/cpp_client

Backwards compatibility tests
=============================
``pw_transfer`` includes a `suite of backwards-compatibility tests
<https://cs.pigweed.dev/pigweed/+/main:pw_transfer/integration_test/legacy_binaries_test.py>`_
that are intended to continuously validate a degree of backwards-compatibility
with older pw_transfer servers and clients. This is done by retrieving older
binaries hosted in CIPD and running tests between the older client/server
binaries and the latest binaries.

The CIPD package contents can be created with this command:

.. code-block::bash

   $ bazel build pw_transfer/integration_test:server \
                 pw_transfer/integration_test:cpp_client
   $ mkdir pw_transfer_test_binaries
   $ cp bazel-bin/pw_transfer/integration_test/server \
        pw_transfer_test_binaries
   $ cp bazel-bin/pw_transfer/integration_test/cpp_client \
        pw_transfer_test_binaries

To update the CIPD package itself, follow the `internal documentation for
updating a CIPD package <http://go/pigweed-cipd#installing-packages-into-cipd>`_.

CI/CQ integration
=================
`Current status of the test in CI <https://ci.chromium.org/ui/p/pigweed/builders/luci.pigweed.pigweed.ci/pigweed-linux-bzl-integration>`_.

By default, these tests are not run in CQ (on presubmit) because they are too
slow. However, you can request that the tests be run in presubmit on your
change by adding to following line to the commit message footer:

.. code-block::

   Cq-Include-Trybots: luci.pigweed.try:pigweed-linux-bzl-integration

.. _module-pw_transfer-parallel-tests:

Running the tests many times
============================
Because the tests bind to network ports, you cannot run more than one instance
of each test in parallel. However, you might want to do so, e.g. to debug
flakes. This section describes a manual process that makes this possible.

Linux
-----
On Linux, you can add the ``"block-network"`` tag to the tests (`example
<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/181297>`_). This
enables network isolation for the tests, allowing you to run them in parallel
via,

.. code-block::

   bazel test --runs_per_test=10 //pw_transfer/integration_tests/...

MacOS
-----
Network isolation is not supported on MacOS because the OS doesn't support
network virtualization (`gh#2669
<https://github.com/bazelbuild/bazel/issues/2669>`_). The best you can do is to
tag the tests ``"exclusive"``. This allows you to use ``--runs_per_test``, but
will force each test to run by itself, with no parallelism.

Why is this manual?
-------------------
Ideally, we would apply either the ``"block-network"`` or ``"exclusive"`` tag
to the tests depending on the OS. But this is not supported, `gh#2971
<https://github.com/bazelbuild/bazel/issues/2971>`_.

We don't want to tag the tests ``"exclusive"`` by default because that will
prevent *different* tests from running in parallel, significantly slowing them
down.
