.. _module-pw_channel:

==========
pw_channel
==========
.. admonition:: ``pw_channel`` is deprecated
   :class: warning

   Due to changes in :ref:`module-pw_async2` and :ref:`module-pw_multibuf`,
   ``pw_channel`` in its current state is deprecated. Do not use it for new
   code.

   A new module that provides the functionality of ``pw_channel`` with
   :cc:`pw_multibuf version 2<pw::multibuf::v2::MultiBuf>` and ``pw_async2``
   futures will be created to replace ``pw_channel``.

   Note that :ref:`pw_async2 Channels <module-pw_async2-channels>` are not
   related to ``pw_channel``.

.. pigweed-module::
   :name: pw_channel

   ``pw_channel`` provides features that are essential for efficient,
   high-performance communications. The ``Channel`` API is:

   - **Flow-control-aware**: Built-in backpressure ensures that data is only
     requested when consumers are able to buffer and handle it.
   - **Zero-copy**: Data transfers seamlessly throughout the stack without
     copying between intermediate buffers or memory pools.
   - **Composable**: Layers of the communications stack are swappable, allowing
     more code reuse and configurability.
   - **Asynchronous**: No need for dedicated threads or nested callbacks.

Not sure if ``pw_channel`` is right for you? Check out
:ref:`module-pw_channel-design-why` to learn how ``pw_channel`` handles
flow control, backpressure, composability, and more.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Design
      :link: module-pw_channel-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How pw_channel handles:

      * Flow control
      * Backpressure
      * Composability
      * Asynchronous operations

      And more.

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

   guides
   design
