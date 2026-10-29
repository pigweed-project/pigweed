.. _module-pw_hdlc-router:

======
Router
======
.. pigweed-module-subpage::
   :name: pw_hdlc

.. admonition:: ``pw::hdlc::Router`` is deprecated
   :class: warning

   ``pw::hdlc::Router`` is deprecated along with :ref:`module-pw_channel`.
   Use :ref:`module-pw_transport` (such as :cc:`pw::transport::Socket`
   from ``pw_transport:socket``) instead.

:cc:`pw::hdlc::Router` is an experimental asynchronous HDLC router using
``pw_channel``.

It sends and receives HDLC packets using an external byte-oriented channel
and routes the decoded packets to local datagram-oriented channels.

-------------
API reference
-------------
Moved: :cc:`pw::hdlc::Router`

-----------------
More pw_hdlc docs
-----------------
.. include:: docs.rst
   :start-after: .. pw_hdlc-nav-start
   :end-before: .. pw_hdlc-nav-end
