.. _module-pw_transport-python:

======
Python
======
.. pigweed-module-subpage::
   :name: pw_transport

The ``pw_transport`` Python package (``//pw_transport/py:pw_transport``)
defines synchronous, thread-safe, datagram-oriented socket and listener
interfaces matching the C++ ``pw::transport`` module:

* :py:class:`pw_transport.Socket` and :py:class:`pw_transport.ReliableSocket`
  read and write whole datagrams over an established connection.
* :py:class:`pw_transport.Listener` and
  :py:class:`pw_transport.ReliableListener` accept incoming connections.

Outgoing connections are opened with plain functions (such as
:py:func:`pw_transport.framed_tcp.connect`) returning a
:py:class:`~pw_transport.Socket` or :py:class:`~pw_transport.ReliableSocket`.

-----
Usage
-----
Implementing a custom transport
===============================
Protocol implementations (such as ``pw_rpc2``) depend only on
``//pw_transport/py:pw_transport``. Custom host transports subclass
:py:class:`pw_transport.ReliableSocket` (or :py:class:`pw_transport.Socket`) and
optionally provide a ``connect(...)`` function or
:py:class:`~pw_transport.Listener` subclass:

.. code-block:: py

   import pw_transport


   class CustomReliableSocket(pw_transport.ReliableSocket):
       @property
       def max_read_message_size(self) -> int | None:
           return 1024

       @property
       def max_write_message_size(self) -> int | None:
           return 512

       @property
       def closed(self) -> bool: ...

       def _read(self, deadline: pw_transport.Deadline) -> bytes:
           # Block until the next framed datagram arrives, or raise
           # TimeoutError / SocketClosed.
           ...

       def _write(self, data: bytes, deadline: pw_transport.Deadline) -> None:
           # Transmit one whole datagram, or raise TimeoutError /
           # SocketClosed.
           ...

       def close(self) -> None:
           # Close the link and unblock pending read() and write() calls.
           ...

Callers use the public :py:meth:`~pw_transport.Socket.read`,
:py:meth:`~pw_transport.Socket.write`, and
:py:meth:`~pw_transport.Listener.accept` methods, which validate their arguments
and then call the ``_read()``, ``_write()``, and ``_accept()`` methods that
implementations override. Implementations never see an invalid timeout or an
outgoing datagram larger than
:py:attr:`~pw_transport.Socket.max_write_message_size`.

Timeouts are passed to implementations as a :py:class:`pw_transport.Deadline`,
which reports whether the operation should give up and how long to wait
otherwise.

-------------
API reference
-------------
pw_transport
============
.. automodule:: pw_transport

.. autoclass:: pw_transport.Socket
   :members:
   :private-members: _read, _write

.. autoclass:: pw_transport.ReliableSocket
   :members:
   :inherited-members:
   :private-members: _read, _write

.. autoclass:: pw_transport.Listener
   :members:
   :private-members: _accept

.. autoclass:: pw_transport.ReliableListener
   :members:
   :inherited-members:
   :private-members: _accept

.. autoclass:: pw_transport.Deadline
   :members:
   :special-members: __init__

.. autoexception:: pw_transport.SocketClosed
   :members:
