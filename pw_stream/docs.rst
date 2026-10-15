.. _module-pw_stream:

.. cpp:namespace-push:: pw::stream

=========
pw_stream
=========
.. pigweed-module::
   :name: pw_stream

``pw_stream`` provides a foundational interface for streaming data from one part
of a system to another. In the simplest use cases, this is basically a memcpy
behind a reusable interface that can be passed around the system. On the other
hand, the flexibility of this interface means a ``pw_stream`` could terminate is
something more complex, like a UART stream or flash memory.

--------
Overview
--------
At the most basic level, ``pw_stream``'s interfaces provide very simple handles
to enabling streaming data from one location in a system to an endpoint.

Example:

.. code-block:: cpp

   Status DumpSensorData(pw::stream::Writer& writer) {
     static char temp[64];
     ImuSample imu_sample;
     imu.GetSample(&info);
     size_t bytes_written = imu_sample.AsCsv(temp, sizeof(temp));
     return writer.Write(temp, bytes_written);
   }

In this example, ``DumpSensorData()`` only cares that it has access to a
:cc:`Writer <pw::stream::Writer>` that it can use to stream data to using
``Writer::Write()``.  The :cc:`Writer <pw::stream::Writer>` itself can be
backed by anything that can act as a data "sink."

---------------------
pw::stream Interfaces
---------------------
There are three basic capabilities of a stream:

* Reading -- Bytes can be read from the stream.
* Writing -- Bytes can be written to the stream.
* Seeking -- The position in the stream can be changed.

``pw_stream`` provides a family of stream classes with different capabilities.
The most basic class, :cc:`Stream <pw::stream::Stream>` guarantees no
functionality, while the most capable class, :cc:`SeekableReaderWriter
<pw::stream::SeekableReaderWriter>` supports reading, writing, and seeking.

Usage overview
==============
.. list-table::
   :header-rows: 1

   * - pw::stream Interfaces
     - Accept in APIs?
     - Extend to create new stream?
   * - :cc:`Stream <pw::stream::Stream>`
     - ❌
     - ❌
   * - | :cc:`Reader <pw::stream::Reader>`
       | :cc:`Writer <pw::stream::Writer>`
       | :cc:`ReaderWriter <pw::stream::ReaderWriter>`
     - ✅
     - ❌
   * - | :cc:`SeekableReader <pw::stream::SeekableReader>`
       | :cc:`SeekableWriter <pw::stream::SeekableWriter>`
       | :cc:`SeekableReaderWriter <pw::stream::SeekableReaderWriter>`
     - ✅
     - ✅
   * - | :cc:`RelativeSeekableReader <pw::stream::RelativeSeekableReader>`
       | :cc:`RelativeSeekableWriter <pw::stream::RelativeSeekableWriter>`
       | :cc:`RelativeSeekableReaderWriter <pw::stream::RelativeSeekableReaderWriter>`
     - ✅ (rarely)
     - ✅
   * - | :cc:`NonSeekableReader <pw::stream::NonSeekableReader>`
       | :cc:`NonSeekableWriter <pw::stream::NonSeekableWriter>`
       | :cc:`NonSeekableReaderWriter <pw::stream::NonSeekableReaderWriter>`
     - ❌
     - ✅

-------------
API reference
-------------
Moved: :cc:`pw_stream`

------------------
Why use pw_stream?
------------------

Standard API
============
``pw_stream`` provides a standard way for classes to express that they have the
ability to write data. Writing to one sink versus another sink is a matter of
just passing a reference to the appropriate :cc:`Writer
<pw::stream::Writer>`.

As an example, imagine dumping sensor data. If written against a random HAL or
one-off class, there's porting work required to write to a different sink
(imagine writing over UART vs dumping to flash memory). Building a "dumping"
implementation against the :cc:`Writer <pw::stream::Writer>` interface
prevents a dependency on a bespoke API that would require porting work.

Similarly, after building a :cc:`Writer <pw::stream::Writer>`
implementation for a Sink that data could be dumped to, that same
:cc:`Writer <pw::stream::Writer>` can be reused for other contexts that
already write data to the :cc:`Writer <pw::stream::Writer>` interface.

Before:

.. code-block:: cpp

   // Not reusable, depends on `Uart`.
   void DumpSensorData(Uart& uart) {
     static char temp[64];
     ImuSample imu_sample;
     imu.GetSample(&info);
     size_t bytes_written = imu_sample.AsCsv(temp, sizeof(temp));
     uart.Transmit(temp, bytes_written, /*timeout_ms=*/200);
   }

After:

.. code-block:: cpp

   // Reusable; no more Uart dependency!
   Status DumpSensorData(Writer& writer) {
     static char temp[64];
     ImuSample imu_sample;
     imu.GetSample(&info);
     size_t bytes_written = imu_sample.AsCsv(temp, sizeof(temp));
     return writer.Write(temp, bytes_written);
   }

Reduce intermediate buffers
===========================
Often functions that write larger blobs of data request a buffer is passed as
the destination that data should be written to. This *requires* a buffer to be
allocated, even if the data only exists in that buffer for a very short period
of time before it's written somewhere else.

In situations where data read from somewhere will immediately be written
somewhere else, a :cc:`Writer <pw::stream::Writer>` interface can cut out
the middleman buffer.

Before:

.. code-block:: cpp

   // Requires an intermediate buffer to write the data as CSV.
   void DumpSensorData(Uart& uart) {
     char temp[64];
     ImuSample imu_sample;
     imu.GetSample(&info);
     size_t bytes_written = imu_sample.AsCsv(temp, sizeof(temp));
     uart.Transmit(temp, bytes_written, /*timeout_ms=*/200);
   }

After:

.. code-block:: cpp

   // Both DumpSensorData() and RawSample::AsCsv() use a Writer, eliminating the
   // need for an intermediate buffer.
   Status DumpSensorData(Writer& writer) {
     RawSample imu_sample;
     imu.GetSample(&info);
     return imu_sample.AsCsv(writer);
   }

Prevent buffer overflow
=======================
When copying data from one buffer to another, there must be checks to ensure the
copy does not overflow the destination buffer. As this sort of logic is
duplicated throughout a codebase, there's more opportunities for bound-checking
bugs to sneak in. ``Writers`` manage this logic internally rather than pushing
the bounds checking to the code that is moving or writing the data.

Similarly, since only the :cc:`Writer <pw::stream::Writer>` has access to
any underlying buffers, it's harder for functions that share a
:cc:`Writer <pw::stream::Writer>` to accidentally clobber data written by
others using the same buffer.

Before:

.. code-block:: cpp

   Status BuildPacket(Id dest,
                      span<const std::byte> payload,
                      span<std::byte> dest) {
     Header header;
     if (dest.size_bytes() + payload.size_bytes() < sizeof(Header)) {
       return Status::ResourceExhausted();
     }
     header.dest = dest;
     header.src = DeviceId();
     header.payload_size = payload.size_bytes();

     memcpy(dest.data(), &header, sizeof(header));
     // Forgetting this line would clobber buffer contents. Also, using
     // a temporary span instead could leave `dest` to be misused elsewhere in
     // the function.
     dest = dest.subspan(sizeof(header));
     memcpy(dest.data(), payload.data(), payload.size_bytes());
   }

After:

.. code-block:: cpp

   Status BuildPacket(Id dest, span<const std::byte> payload, Writer& writer) {
     Header header;
     header.dest = dest;
     header.src = DeviceId();
     header.payload_size = payload.size_bytes();

     writer.Write(header);
     return writer.Write(payload);
   }

------------
Design notes
------------

Sync & Flush
============
The :cc:`Stream <pw::stream::Stream>` API does not include ``Sync()`` or
``Flush()`` functions. There no mechanism in the :cc:`Stream
<pw::stream::Stream>` API to synchronize a :cc:`Reader <pw::stream::Reader>`'s
potentially buffered input with its underlying data source. This must be handled
by the implementation if required.  Similarly, the :cc:`Writer
<pw::stream::Writer>` implementation is responsible for flushing any buffered
data to the sink.

``Flush()`` and ``Sync()`` were excluded from :cc:`Stream
<pw::stream::Stream>` for a few reasons:

* The semantics of when to call ``Flush()``/``Sync()`` on the stream are
  unclear. The presence of these methods complicates using a :cc:`Reader
  <pw::stream::Reader>` or :cc:`Writer <pw::stream::Writer>`.
* Adding one or two additional virtual calls increases the size of all
  :cc:`Stream <pw::stream::Stream>` vtables.

.. _module-pw_stream-class-hierarchy:

Class hierarchy
===============
All ``pw_stream`` classes inherit from a single, common base with all possible
functionality: :cc:`Stream <pw::stream::Stream>`. This structure has
some similarities with Python's `io module
<https://docs.python.org/3/library/io.html>`_ and C#'s `Stream class
<https://docs.microsoft.com/en-us/dotnet/api/system.io.stream>`_.

An alternative approach is to have the reading, writing, and seeking portions of
the interface provided by different entities. This is how Go's `io
package <https://pkg.go.dev/io>`_ and C++'s `input/output library
<https://en.cppreference.com/w/cpp/io>`_ are structured.

We chose to use a single base class for a few reasons:

* The inheritance hierarchy is simple and linear. Despite the linear
  hierarchy, combining capabilities is natural with classes like
  :cc:`ReaderWriter <pw::stream::ReaderWriter>`.

  In C++, separate interfaces for each capability requires either a complex
  virtual inheritance hierarchy or entirely separate hierarchies for each
  capability. Separate hierarchies can become cumbersome when trying to
  combine multiple capabilities. A :cc:`SeekableReaderWriter
  <pw::stream::SeekableReaderWriter>` would have to implement three different
  interfaces, which means three different vtables and three vtable pointers in
  each instance.
* Stream capabilities are clearly expressed in the type system, while
  naturally supporting optional functionality. A :cc:`Reader
  <pw::stream::Reader>` may or may not support :cc:`Stream::Seek
  <pw::stream::Stream::Seek>`. Applications that can handle seek failures
  gracefully way use seek on any :cc:`Reader <pw::stream::Reader>`. If seeking
  is strictly necessary, an API can accept a :cc:`SeekableReader
  <pw::stream::SeekableReader>` instead.

  Expressing optional functionality in the type system is cumbersome when
  there are distinct interfaces for each capability. ``Reader``, ``Writer``,
  and ``Seeker`` interfaces would not be sufficient. To match the flexibility
  of the current structure, there would have to be separate optional versions
  of each interface, and classes for various combinations. :cc:`Stream
  <pw::stream::Stream>` would be an "OptionalReaderOptionalWriterOptionalSeeker"
  in this model.
* Code reuse is maximized. For example, a single
  :cc:`Stream::ConservativeLimit <pw::stream::Stream::ConservativeLimit>`
  implementation supports many stream implementations.

Virtual interfaces
==================
``pw_stream`` uses virtual functions. Virtual functions enable runtime
polymorphism. The same code can be used with any stream implementation.

Virtual functions have inherently has more overhead than a regular function
call. However, this is true of any polymorphic API. Using a C-style ``struct``
of function pointers makes different trade-offs but still has more overhead than
a regular function call.

For many use cases, the overhead of virtual calls insignificant. However, in
some extremely performance-sensitive contexts, the flexibility of the virtual
interface may not justify the performance cost.

Asynchronous APIs
=================
At present, ``pw_stream`` is synchronous. All :cc:`Stream
<pw::stream::Stream>` API calls are expected to block until the operation is
complete. This might be undesirable for slow operations, like writing to NOR
flash.

Pigweed has not yet established a pattern for asynchronous C++ APIs. The
:cc:`Stream <pw::stream::Stream>` class may be extended in the future to
add asynchronous capabilities, or a separate ``AsyncStream`` could be created.

.. cpp:namespace-pop::

------------
Dependencies
------------
* :ref:`module-pw_assert`
* :ref:`module-pw_preprocessor`
* :ref:`module-pw_status`
* :ref:`module-pw_span`

------
Zephyr
------
To enable ``pw_stream`` for Zephyr add ``CONFIG_PIGWEED_STREAM=y`` to the
project's configuration.

----
Rust
----
Pigweed centric analogs to Rust ``std``'s ``Read``, ``Write``, ``Seek`` traits
as well as a basic ``Cursor`` implementation are provided by the
`pw_stream crate </rustdoc/pw_stream/>`_.


.. toctree::
   :hidden:
   :maxdepth: 1

   backends

------
Python
------
There are legacy Python utilities used for reading and writing a serial device
for RPC purposes.

.. toctree::
   :hidden:

   Python <py/docs>
