.. _module-pw_multibuf:

===========
pw_multibuf
===========
.. pigweed-module::
   :name: pw_multibuf

Many forms of device I/O, including sending or receiving messages via RPC,
transfer, or sockets, need to deal with multiple buffers or a series of
intermediate buffers, each requiring their own copy of the data. ``pw_multibuf``
allows data to be written *once*, eliminating the memory, CPU and latency
overhead of copying, and aggregates the memory regions in a manner
that is:

- **Flexible**: Memory regions can be discontiguous and have different ownership
  semantics. Memory regions can be added and removed with few restrictions.
- **Copy-averse**: Users can pass around and mutate MultiBuf instances without
  copying or moving data in-memory.
- **Compact**: The sequence of memory regions and details about them are stored
  in only a few words of additional metadata.


.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-basic]
   :end-before: [pw_multibuf-examples-basic]

For the complete example, see :cs:`pw_multibuf/examples/basic.cc`.

-------------------------------
What kinds of data is this for?
-------------------------------
``pw_multibuf`` is best used in code that wants to read, write, or pass along
data which are one or more of the following:

- **Large**: The MultiBuf type allows breaking up data into multiple chunks.
- **Heterogeneous**: MultiBuf instances allow combining data that is uniquely
  owned, shared, or externally managed, and encapsulates the details of
  deallocating the memory it owns.
- **Latency-sensitive**: Since they are copy-averse, MultiBuf instances are
  useful when working in systems that need to pass large amounts of data, or
  when memory usage is constrained.
- **Discontiguous**: MultiBuf instances provide an interface to accessing and
  modifying memory regions that encapsulates where the memory actually resides.
- **Communications-oriented**: Data which is being received or sent across
  sockets, various packets, or shared-memory protocols can benefit from the
  fragmentation, multiplexing, and layering features of the MultiBuf type.

.. LINT.IfChange(buf_relationship)

----------------------
Relationship to pw_buf
----------------------
Conceptually, a ``MultiBuf`` is a sequence of layered :cc:`pw::Buf` or
:cc:`pw::ConstBuf` instances. While :ref:`module-pw_buf` manages a *single,
contiguous* block of memory, ``pw_multibuf`` aggregates a sequence of such
buffers into a unified virtual span, with support for non-destructive layering
and fragmentation across the collection.

The two modules complement each other and integrate directly:

* **Ingestion:** :cc:`pw::Buf` and :cc:`pw::ConstBuf` instances can be added to
  a MultiBuf using ``PushBack()`` or ``Insert()``. Unowned bufs are added as
  spans, while allocated bufs transfer their ownership to the MultiBuf.
* **Extraction:** Contiguous owned allocations can be extracted from a MultiBuf
  back into a :cc:`pw::Buf` or :cc:`pw::ConstBuf` using ``Release()``.
  (To release the raw chunk as a :cc:`pw::UniquePtr`, use ``ReleaseChunk()``.)

Use ``pw_buf`` when a single contiguous buffer is sufficient, especially for
in-place slicing and prefix/suffix reclamation. Use ``pw_multibuf`` when
aggregating multiple buffers, decomposing packets across layers, or performing
scatter-gather I/O.

.. LINT.ThenChange(//pw_buf/docs.rst:multibuf_relationship)

.. toctree::
   :hidden:
   :maxdepth: 1

   guide
   concepts
   design
   code_size

.. grid:: 3

   .. grid-item-card:: :octicon:`rocket` Examples
      :link: module-pw_multibuf-guide
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to use pw_multibuf through a series of examples

   .. grid-item-card:: :octicon:`light-bulb` Concepts
      :link: module-pw_multibuf-concepts
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Explore the ideas behind pw_multibuf

   .. grid-item-card:: :octicon:`pencil` Design
      :link: module-pw_multibuf-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn why pw_multibuf is designed the way it is

.. grid:: 3

   .. grid-item-card:: :octicon:`code` API reference
      :link: ../api/cc/group__pw__multibuf__v2.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Detailed description of pw_multibuf's current API

   .. grid-item-card:: :octicon:`code-square` Legacy API
      :link: ../api/cc/group__pw__multibuf__v1.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Detailed description of pw_multibuf's legacy API

   .. grid-item-card:: :octicon:`beaker` Code size analysis
      :link: module-pw_multibuf-size-reports
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand pw_multibuf's code and memory footprint
