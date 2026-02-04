.. _module-pw_multibuf-concepts:

========
Concepts
========
.. pigweed-module-subpage::
   :name: pw_multibuf

-------------
Why MultiBuf?
-------------
On embedded devices, as on other platforms, it is often convenient to associate
multiple regions of memory. Scatter-gather I/O, in which several buffers are
read or written in a single operation is one such use case, assembly of network
protocol messages from different buffers representing different layers in a
protocol stack is another. Ideally, these collections of memory regions should
be dynamic, allowing regions to be added or removed with few restrictions.

Simply aggregating memory buffers fails to provide a convenient interface for
interacting with this memory. Ideally, the container of buffers should provide
callers with ways to access the memory byte-by-byte and buffer-by-buffer. If
these memory regions are to be used with core platform I/O, they should also
avoid copying data unnecessarily to keep performance high.

Finally, different memory regions may have different ownership semantics. Some
memory may be uniquely owned, and should be freed when the collection goes out
of scope. Others may be externally owned, and should be left intact. Still
others may have shared pointer semantics. The collection should correctly handle
memory ownership in all these cases to preserve correctness and prevent leaks.

:cc:`GenericMultiBuf <pw::multibuf::v2::internal::GenericMultiBuf>` and its
derived types have been designed to meet these needs. These types are
collectively referred to as "MultiBuf" in the following sections, except where
details of the derived types differ.

--------------
MultiBuf ideas
--------------
In order to reduce ambiguity, it is important to establish the meaning of some
terms throughout this module:

.. _module-pw_multibuf-concepts-entries:

Entries
=======
A MultiBuf instance manages an internal deque of metadata values. These values
describe part of a memory region. Each entry holds either a pointer, or an
offset, length, and flags. The entry type is internal to ``pw_multibuf``, and
represents the primary contribution to memory overhead for the type.

.. TODO: b/444237874 - Render this diagram directly once the mermaid plugin is
   updated. For now, use a pre-rendered image.
   block
       block:deque
           E1["entry1"]
           E2["entry2"]
           E3["entry3"]
           E4["entry4"]
       end

.. figure:: https://www.gstatic.com/pigweed/pw_multibuf/entries.png
   :alt: MultiBufs have an internal deque of entries

.. _module-pw_multibuf-concepts-chunks:

Chunks
======
Chunks are contiguous memory regions in a MultiBuf instance. They are
represented by two or more entries. The first entry will be the pointer to
memory region. The remaining will correspond to each layer. Each chunk is
conceptually smiliar to a ``ByteSpan`` instance.

.. TODO: b/444237874 - Render this diagram directly once the mermaid plugin is
   updated. For now, use a pre-rendered image.
   block
       columns 2
       view1["offset: 0x0\nlength: 0x400"]
       view2["offset: 0x0\nlength: 0x1C0"]
       base1["0x5CAFE000"]
       base2["0x5CAFE800"]
       block:deque:2
           E1["entry1:\nbase1"]
           E2["entry2:\nview1"]
           E3["entry3:\nbase2"]
           E4["entry4:\nview2"]
       end
       E1-->base1
       E2-->view1
       E3-->base2
       E4-->view2

.. figure:: https://www.gstatic.com/pigweed/pw_multibuf/chunks.png
   :alt: Entries store memory addresses and views into those regions

.. _module-pw_multibuf-concepts-layers:

Layers
======
Every MultiBuf instance provides a view of its aggregated memory regions.
If a MultiBuf type includes :cc:`kLayerable <pw::multibuf::v2::Property>` as one
of its :ref:`module-pw_multibuf-design-properties`, then it supports adding
layers that restrict the view of the memory. Each layer represents a subset of
the layer below, with the bottom-most layer being the memory regions themselves.
Each layer adds one entry to each chunk, describing the offset and length of the
chunk that is visible at that layer.

Layers add new chunks rather than modifying existing ones. This allows "popping"
the top layer to return the MultiBuf instance to the state it was in before the
layer was added. For example, popping a TCP segment layer might return a
MultiBuf instance to a larger view that encompasses an IP packet.

.. TODO: b/444237874 - Render this diagram directly once the mermaid plugin is
   updated. For now, use a pre-rendered image.
   block
       columns 2
       layer1.2["offset: 0x40\nlength: 0x100"]
       layer2.2["offset: 0xC0\nlength: 0x80"]
       layer1.1["offset: 0x20\nlength: 0x200"]
       layer2.1["offset: 0x80\nlength: 0x100"]
       view1["offset: 0x0\nlength: 0x400"]
       view2["offset: 0x0\nlength: 0x1C0"]
       base1["0x5CAFE000"]
       base2["0x5CAFE800"]
       block:deque:2
           E1["entry1:\nbase1"]
           E2["entry2:\nview1"]
           E3["entry3:\nlayer1.1"]
           E4["entry4:\nlayer1.2"]
           E5["entry5:\nbase2"]
           E6["entry6:\nview2"]
           E7["entry6:\nlayer2.1"]
           E8["entry6:\nlayer2.2"]
       end
       E1-->base1
       E2-->view1
       E3-->layer1.1
       E4-->layer1.2
       E5-->base2
       E6-->view2
       E7-->layer2.1
       E8-->layer2.2

.. figure:: https://www.gstatic.com/pigweed/pw_multibuf/layers.png
   :alt: Layers add additional entries specifying narrower views of memory

.. _module-pw_multibuf-concepts-fragments:

Fragments
=========

Each time a layer is added, all the chunks in a MultiBuf instance are considered
part of the same fragment at the new layer.

Thus, if several frames are combined and a layer added to make a packet, the
packet is considered a single fragment at that layer. If several packets are
combined and a layer added to make a segment, the segment is considered a single
fragment at that layer. This can be used when decomposing higher level protocol
messages back into lower level ones, e.g. a segment back into multiple packets.

.. TODO: b/444237874 - Render this diagram directly once the mermaid plugin is
   updated. For now, use a pre-rendered image.
   block
       columns 4
       block:segment:4
           layer1.2["segment1.1"]
           layer2.2["segment1.2"]
           layer3.2["segment1.2"]
           layer4.2["segment1.4"]
       end
       block:packet1:2
           layer1.1["packet1.1"]
           layer2.1["packet1.2"]
       end
       block:packet2:2
           layer3.1["packet2.1"]
           layer4.1["packet2.2"]
       end
       view1["frame1"]
       view2["frame2"]
       view3["frame3"]
       view4["frame4"]

.. figure:: https://www.gstatic.com/pigweed/pw_multibuf/fragments.png
   :alt: Fragments group chunks together at different levels

.. _module-pw_multibuf-concepts-ownership:

Memory ownership
================
MultiBuf instances can hold memory regions with different ownership semantics.
Ownership here refers to what object is responsible for freeing the memory when
it is no longer needed. A single MultiBuf instance can hold memory from each of
three categories:

  - **Unique Ownership**: Memory is provided as a :cc:`pw::UniquePtr`. A
    flag in the corresponding entry marks the memory as "owned". The
    MultiBuf instance will deallocate the memory when it is no longer
    referenced.
  - **Shared Ownership**: Memory is provided as a :cc:`pw::SharedPtr`. A
    flag in the corresponding entry marks the memory as "shared". The MultiBuf
    instance will deallocate the memory when it is discarded, but only if no
    other existing objects share ownership.
  - **No Ownership**: Memory is provided as a ``ByteSpan``, and treated as
    unowned. The MultiBuf instance simply holds a reference, and the caller
    is responsible for managing the memory's lifetime.
