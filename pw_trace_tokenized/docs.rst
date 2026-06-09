.. _module-pw_trace_tokenized:

==================
pw_trace_tokenized
==================
.. pigweed-module::
   :name: pw_trace_tokenized

Pigweed's tracing module provides facilities for applications to trace
information about the execution of their application. The module is split into
two components:

1. The facade, provided elsewhere, which is only a macro interface layer
2. The backend (this module), is one implemention of the low level tracing.

------
Status
------
This module is currently in development, and is therefore still undergoing
significant changes.

Future work will:

1. Add a more complete API for how to retrieve data from ring_buffer.
2. Add examples with sample output (especially for filtering and triggering).
3. Add tools to retrieve trace data.
4. Add more sinks, such as RTT.
5. Add support to more platforms.
6. Improve the locking behaviour and provide default trace locking
   implementions.

--------
Overview
--------
The tokenized trace backend aims to be a reasonable tradeoff of trace features
and event size for most applications. It works by encoding all compile time data
for a trace event into a tokenized number. This provides a good amount of
compression, while maintaining the full trace feature set.

In addition the tokenized trace backend adds flexibility through callbacks,
which allows the application to do things such as filtering trace_events and
triggering tracing to turn on and off. This flexibility can help maximize the
effectiveness of a limited trace buffer as well as be a valuable tool while
debugging.


Compatibility
=============
Most of this module is compatible with C and C++, the only exception to this is
the ``RegisterCallbackWhenCreated`` helper class.

Dependencies
============
``pw_assert``
``pw_log``
``pw_preprocessor``
``pw_status``
``pw_tokenizer``
``pw_trace:facade``
``pw_varint``

---------
Macro API
---------
All code should use the trace API facade directly. This backend fully
implements all features of the tracing facade.


Event Callbacks & Data Sinks
============================
The tokenized trace module adds both event callbacks and data sinks which
provide hooks into tracing.

The *event callbacks* are called when trace events occur, with the trace event
data, before the event is encoded or sent to the sinks. The callbacks may
modify the run-time fields of the trace event, i.e. ``trace_id``,
``data_buffer`` and ``data_size``. Using the return flags, these callbacks can
be used to adjust the trace behaviour at runtime in response to specific events.

If requested (using ``called_on_every_event``) the callback will be called on
every trace event regardless if tracing is currently enabled or not. Using this,
the application can trigger tracing on or off when specific traces or patterns
of traces are observed, or can selectively filter traces to preserve the trace
buffer.

The event callback is called in the context of the traced task. It must be
ISR-safe to support tracing within ISRs. It must be lightweight to prevent
performance issues in the trace tasks.

The return flags ``pw_trace_TraceEventReturnFlags`` support the following
behaviors:

* ``PW_TRACE_EVENT_RETURN_FLAGS_SKIP_EVENT`` can be set true to skip this
  sample.
* ``PW_TRACE_EVENT_RETURN_FLAGS_DISABLE_AFTER_PROCESSING`` can be set true to
  disable tracing after this sample.

.. cpp:function:: pw_trace_TraceEventReturnFlags pw_trace_EventCallback( \
    void* user_data,  \
    pw_trace_tokenized_TraceEvent* event)
.. cpp:function:: pw_Status pw_trace_RegisterEventCallback( \
    pw_trace_EventCallback callback, \
    pw_trace_EventCallbackFlags flags, \
    void* user_data, \
    pw_trace_EventCallbackHandle* handle)
.. cpp:function:: pw_Status pw_trace_UnregisterEventCallback( \
    pw_trace_EventCallbackHandle handle)


The *data sinks* are called only for trace events which get processed (tracing
is enabled, and the sample not skipped). The sink callback is called with the
encoded bytes of the trace event, which can be used by the application to
connect different data sinks. The callback is broken into three callbacks
``pw_trace_SinkStartBlock``, ``pw_trace_SinkAddBytes``, and
``pw_trace_SinkEndBlock``. ``Start`` is called with the size of the block,
before any bytes are emitted and can be used if needed to allocate space.
``AddBytes`` is then called multiple times with chunks of bytes. Finally ``End``
is called to allow any cleanup to be done by the sink if neccessary. Not all
callbacks are required, it is acceptible to provide nullptr for any callbacks
which you don't require.

.. cpp:function:: void pw_trace_SinkStartBlock(void* user_data, size_t size)
.. cpp:function:: void pw_trace_SinkAddBytes( \
    void* user_data, \
    const void* bytes, \
    size_t size)
.. cpp:function:: void pw_trace_SinkEndBlock(void* user_data)
.. cpp:function:: pw_Status pw_trace_RegisterSink( \
    pw_trace_SinkStartBlock start, \
    pw_trace_SinkAddBytes add_bytes, \
    pw_trace_SinkEndBlock end_block, \
    void* user_data, \
    pw_trace_SinkHandle* handle)
.. cpp:function:: pw_Status pw_trace_UnregisterSink(pw_trace_SinkHandle handle)

Trace Reference
===============
Some use-cases might involve referencing a specific trace event, for example
to use it as a trigger or filtering. Since the trace events are tokenized, a
macro is provided to generate the token to use as a reference. All the fields
must match exactly to generate the correct trace reference. If the trace does
not have a group, use ``PW_TRACE_GROUP_LABEL_DEFAULT``.

.. cpp:function:: PW_TRACE_REF(event_type, module, label, flags, group)
.. cpp:function:: PW_TRACE_REF_DATA( \
   event_type, module, label, flags, group, type)

-----------
Time source
-----------
Tracing requires the platform to provide the time source for tracing, this can
be done in one of a few ways.

1. Create a file with the default time functions, and provide as build variable
   ``pw_trace_tokenized_time``, which will get pulled in as a dependency.
2. Provide time functions elsewhere in project, and ensure they are included.
3. Provide definitions of the following trace time functions.

.. cpp:function:: PW_TRACE_TIME_TYPE pw_trace_GetTraceTime()
.. cpp:function:: size_t pw_trace_GetTraceTimeTicksPerSecond()

In the Bazel build, set the ``@pigweed//pw_trace_tokenized:trace_time`` `label
flag <https://bazel.build/extending/config#label-typed-build-settings>`__ to
select the implementation of trace time to use. This label flag should point to
a ``cc_library`` target that provides implementations of the two functions
above.

------
Buffer
------
The optional trace buffer adds a ring buffer which contains the encoded trace
data. This is still a work in progress, in particular better methods for
retrieving the data still need to be added. Currently there is an accessor for
the underlying ring buffer object, but this is a short term solution.

.. cpp:function:: void ClearBuffer()
.. cpp:function:: pw::trace::TraceBuffer* GetBuffer()

The buffer has two configurable options:

1. PW_TRACE_BUFFER_SIZE_BYTES: The total size of the ring buffer in bytes.
2. PW_TRACE_BUFFER_MAX_BLOCK_SIZE_BYTES: The maximum single trace object size.
   Including the token, time, and any attached data. Any trace object larger
   then this will be dropped.

In the Bazel build, set the ``@pigweed//pw_trace_tokenized:config_backend`` to
define the value of PW_TRACE_BUFFER_SIZE_BYTES. This label flag should point to
a ``cc_library`` target that has a "defines" field for PW_TRACE_BUFFER_SIZE_BYTES.

.. cpp:function:: ConstByteSpan DeringAndViewRawBuffer()

The DeringAndViewRawBuffer function can be used to get bulk access of the full
deringed prefixed-ring-buffer data. This might be neccessary for large zero-copy
bulk transfers. It is the caller's responsibility to disable tracing during
access to the buffer. The data in the block is defined by the
prefixed-ring-buffer format without any user-preamble.

Added dependencies
==================
``pw_ring_buffer``
``pw_varint``

-------
Locking
-------
``pw_trace_tokenized`` uses a lock facade to protect access to internal data
structures. This allows the locking implementation to be replaced depending on
the application's requirements. The lock facade is configured by setting the
``pw_trace_tokenized_LOCK_BACKEND`` build variable.

Two backends are provided:

*   **``interrupt_spin_lock`` (Default):** This backend uses a
    ``pw::sync::InterruptSpinLock`` to provide thread-safe and ISR-safe
    locking. This is the default and recommended backend for most use cases.

*   **``no_lock``:** This backend provides no locking. It is suitable for
    single-threaded environments where locking is not required, or when the
    application can guarantee that tracing functions are not called from
    multiple threads or ISRs simultaneously.

-------
Logging
-------
The optional trace buffer logging adds support to dump trace buffers to the log.
Buffers are converted to base64-encoding then split across log lines. Trace logs
are surrounded by 'begin' and 'end' tags.

Ex. Invoking PW_TRACE_INSTANT with 'test1' and 'test2', then calling this
function would produce this in the output logs:

.. code-block:: text

   [TRACE] begin
   [TRACE] data: BWdDMRoABWj52YMB
   [TRACE] end

Added dependencies
==================
``pw_base64``
``pw_log``
``pw_ring_buffer``
``pw_string``
``pw_tokenizer``
``pw_varint``

-------
Decoder
-------

C++ decoder
===========
The :cc:`pw::trace::TokenizedDecoder` class will decode binary trace data into
:cc:`pw::trace::DecodedEvent` objects for custom consumption.

Python decoder
==============
The python decoder can be used to convert the binary trace data into json data
which can be viewed in chrome://tracing.

``get_trace.py`` can be used for retrieveing trace data from devices which are
using the trace_rpc_server.

``trace_tokenized.py`` can be used to decode a binary file of trace data.

``token_label.py`` can be used to register a handler for
decoding tokenized labels (format ``@pw_trace_tokenized_token_label``).

--------
Examples
--------
The examples all use `pw_trace` sample app to provide the trace data. Details
for how to build, run, and decode the traces are included at the top of each
example. This is early work, and is provided as an example of how different
tracing concepts can look.

Basic
=====
The basic example turns on tracing and dumps all trace output to a file provided
on the command line.

Trigger
=======
The trigger example demonstrates how a trace event can be used as a trigger to
start and stop capturing a trace. The examples makes use of ``PW_TRACE_REF``
and ``PW_TRACE_REF_DATA`` to specify a start and stop event for the capture.
This can be useful if the trace buffer is small and you wish to capture a
specific series of events.

Filter
======
The filter example demonstrates how a callback can be used to filter which trace
events get processed and saved. In this example all events from the processing
task which don't have traceId equal to 3 are removed. Both the other task traces
are not removed. This can be a useful feature while debugging as it limits the
amount of events which get stored to the buffer, and only saves the events of
interest.

--------------------
Snapshot integration
--------------------
Tokenized trace buffers can be captured to a ``pw.snapshot.Snapshot`` or
``pw.trace.SnapshotTraceInfo`` proto in the ``trace_data`` field. The expected
format is a de-ringed raw tokenized trace buffer, which can be retrieved via
``pw::trace::DeringAndViewRawBuffer()``.

``pw_trace_tokenized`` does not yet have Python tooling integration for
interpretation of serialized snapshots with a populated ``trace_data`` field.

-------------
API reference
-------------
Moved: :cc:`pw_trace_tokenized`
