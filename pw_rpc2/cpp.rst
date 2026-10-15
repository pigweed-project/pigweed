.. _module-pw_rpc2-cpp:

=====================
C++ client and server
=====================
.. pigweed-module-subpage::
   :name: pw_rpc2

.. note::

   This page is incomplete and under active development.

.. _module-pw_rpc2-cpp-errors:

--------------
Error handling
--------------
``pw_rpc2`` APIs report errors as a ``pw::rpc2::RpcStatus``, or as a
``pw::rpc2::RpcResult<T>`` that holds either a value or an ``RpcStatus``. An
``RpcStatus`` is a single ``pw::rpc2::RpcCode``. Each code maps to one distinct
action, so the code alone tells the caller what to do next. Check for a
specific code with its ``Is*()`` function, such as ``status.IsEndOfStream()``.

A status does not say exactly what went wrong. ``pw_rpc2`` logs the precise
cause where it detects an error, including the precise error code when the peer
ended a call (see :ref:`module-pw_rpc2-protocol-error-codes`). The exceptions
are a closed connection, which is the most common cause of ``kDisconnected``,
and errors whose code already says what happened, such as ``kEndOfStream``,
``kClosed``, and ``kWriteClosed``. Use the code to decide how to respond, and
the logs to debug.

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Code
     - Meaning
     - What to do next
   * - ``kOk``
     - Success.
     - Continue.
   * - ``kEndOfStream``
     - The peer finished its stream and every message has been read. Not a
       failure.
     - Stop reading. For a client, the RPC finished successfully.
   * - ``kCancelled``
     - Application code on either side ended the call, explicitly or
       implicitly, such as by dropping a call handle.
     - Stop work on the call. Don't retry automatically; application logic
       chose to end it.
   * - ``kClosed``
     - You closed the ``Client`` or ``Server``, or the object you used is
       empty.
     - Nothing to recover; it's the result of your own action. Stop using the
       object, or create a new ``Client``.
   * - ``kWriteClosed``
     - The call's outbound direction is closed. The call itself may still be
       open.
     - Stop writing. If a response or more messages are expected, keep reading.
   * - ``kDisconnected``
     - The connection is gone or couldn't be made, for a reason that a new
       connection can fix. Whether the peer processed in-flight calls is
       unknown.
     - Create a new ``Client``, with backoff. Retry in-flight calls only if
       they're idempotent. On a server, stop work on the call.
   * - ``kResourceExhausted``
     - This endpoint couldn't allocate memory for the operation. The peer
       wasn't involved.
     - Free local resources (finish or drop other calls, or run fewer at once),
       then retry. Backing off against the server won't help.
   * - ``kUnavailable``
     - The server couldn't take the call right now. The method never ran.
     - Retry later with backoff. Retrying is safe even for methods that aren't
       idempotent.
   * - ``kAborted``
     - The server started the call, then ended it for a reason outside the
       method's control. The method may have partly run.
     - Retry only if the method is idempotent; otherwise, report the failure.
   * - ``kUnimplemented``
     - The server doesn't offer this method in the form the client expects.
     - Don't retry against this server. Fall back (for example, to an older API
       or a feature-detected path), or fix the deployment or schema mismatch.
   * - ``kMessageTooLarge``
     - An outbound message is larger than the transport or reservation allows.
       Nothing was sent, and the call is still open.
     - Send a smaller message (for example, split it), or raise the reservation
       or transport limit.
   * - ``kDataError``
     - A message couldn't be encoded or decoded. After a local write, nothing
       was sent and the call is still open. After a local read, that message is
       dropped, but a stream stays open and the next read may succeed. If the
       server couldn't decode the request, the call is over.
     - Don't resend the same message unchanged; fix it or the schema mismatch.
   * - ``kProtocolError``
     - An endpoint broke the protocol, the endpoints' protocol versions are
       incompatible, or an RPC implementation has a bug. If the error closed
       the connection, later calls on the same ``Client`` also report
       ``kProtocolError``.
     - Nothing fixes it at runtime: reconnecting and retrying won't help. Log
       or report it; it needs a code or deployment fix.

Codes that look similar call for different responses:

* ``kCancelled`` versus ``kAborted``: application code ended the call (don't
  retry automatically), or a server condition ended it after the method
  started (retry if idempotent).
* ``kClosed`` versus ``kDisconnected``: you closed it (nothing to do), or it
  went away on its own (reconnect).
* ``kClosed`` versus ``kWriteClosed``: the whole object is unusable, or only
  writing is over (keep reading).
* ``kResourceExhausted`` versus ``kUnavailable``: free local memory, or back
  off from the server.
* ``kUnavailable`` versus ``kAborted``: retrying is always safe, or only if
  the method is idempotent.
* ``kUnimplemented`` versus ``kProtocolError``: fall back to another method,
  or report a bug.
* ``kMessageTooLarge`` versus ``kDataError``: shrink the message, or fix its
  content or schema.
* ``kDisconnected`` versus ``kProtocolError``: reconnecting helps, or it
  doesn't.

Errors from the peer
====================
When the peer ends a call with an error code, ``pw_rpc2`` logs the code, and
the call completes with the ``RpcCode`` for the action it calls for:

..
   # LINT.IfChange(rpc2_peer_error_mapping)

* ``kCancelled``: ``CANCELLED``, ``DROPPED_WITHOUT_RESPONSE``, or
  ``METHOD_RETURNED_WITHOUT_FINISHING``.
* ``kUnavailable``: ``FAILED_TO_ALLOCATE_CALL``.
* ``kAborted``: ``FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING`` or
  ``SERVICE_UNREGISTERED``.
* ``kUnimplemented``: ``UNKNOWN_SERVICE``, ``UNKNOWN_METHOD``, or
  ``METHOD_TYPE_MISMATCH``.
* ``kDataError``: ``INVALID_REQUEST_PAYLOAD``.
* ``kProtocolError``: any other code, including a code that the peer may not
  send or that the protocol does not define.

..
   # LINT.ThenChange(//pw_rpc2/cpp/public/pw_rpc2/internal/protocol_error.h:cpp_rpc2_peer_error_mapping)

Server methods
==============
A server method can see ``kEndOfStream``, ``kCancelled``, ``kClosed``,
``kWriteClosed``, ``kDisconnected``, ``kMessageTooLarge``, ``kDataError``,
``kUnimplemented`` (the client reported that the method's type doesn't match),
and ``kProtocolError`` (including when the client reuses the call's ID). For a
server method, any status other than ``kOk`` or ``kEndOfStream`` means that it
should stop working on the call, with three exceptions:

* After ``kMessageTooLarge`` or a ``kDataError`` from a write, it can send a
  different message.
* After a ``kDataError`` from a read, the next read may succeed.
* After ``kWriteClosed``, it can keep reading.
