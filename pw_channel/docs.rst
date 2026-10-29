.. _module-pw_channel:

==========
pw_channel
==========
.. admonition:: ``pw_channel`` is deprecated
   :class: warning

   ``pw_channel`` is deprecated. Do not use it for new code. Use
   :cc:`pw::transport::Socket` or
   :cc:`pw::transport::ReliableSocket` in :ref:`module-pw_transport`
   instead.

   Note that :ref:`pw_async2 Channels <module-pw_async2-channels>` are not
   related to ``pw_channel``.

.. pigweed-module::
   :name: pw_channel

   ``pw_channel`` provides async data channels. It is deprecated and replaced by
   :cc:`pw::transport::Socket` and
   :cc:`pw::transport::ReliableSocket` in :ref:`module-pw_transport`.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Design
      :link: module-pw_channel-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Channel attributes and the hourglass inheritance pattern.

   .. grid-item-card:: :octicon:`code-square` Reference
      :link: ../api/cc/group__pw__channel.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      API reference for:

      * ``Channel``
      * ``AnyChannel``
      * ``ByteChannel``
      * ``DatagramChannel``

      And more.

.. toctree::
   :hidden:

   design
