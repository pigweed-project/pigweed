.. _module-pw_containers-maps:

====
Maps
====
.. pigweed-module-subpage::
   :name: pw_containers

A map is an associative collection of keys that map to values. Pigweed provides
an implementation of a constant "flat" map that can find values by key in
constant time. It also provides implementations of dynamic maps that can insert,
find, and remove key-value pairs in logarithmic time.

-----------------------
pw::containers::FlatMap
-----------------------
:cc:`FlatMap` provides a simple, fixed-size associative array with ``O(log n)``
lookup by key.

``pw::containers::FlatMap`` contains the same methods and features for looking
up data as ``std::map``. However, modification of the underlying data is limited
to the mapped values, via ``.at()`` (key must exist) and ``mapped_iterator``
objects returned by ``.mapped_begin()`` and ``.mapped_end()``.
``mapped_iterator`` objects are bidirectional iterators that can be dereferenced
to access and mutate the mapped value objects.

The underlying array in ``pw::containers::FlatMap`` does not need to be sorted.
During construction, ``pw::containers::FlatMap`` will perform a constexpr
insertion sort.

Examples
========
A ``FlatMap`` can be created in one of several ways. Each of the following
examples defines a ``FlatMap`` with two items.

.. literalinclude:: examples/flat_map.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-flat_map]
   :end-before: [pw_containers-flat_map]

.. _module-pw_containers-intrusive_map:

----------------
pw::IntrusiveMap
----------------
:cc:`pw::IntrusiveMap` provides an embedded-friendly, tree-based, intrusive
map implementation. The intrusive aspect of the map is very similar to that of
:ref:`module-pw_containers-intrusive_list`.

This class is similar to ``std::map<K, V>``. Items to be added must derive from
``pw::IntrusiveMap<K, V>::Item`` or an equivalent type.

See also :ref:`module-pw_containers-multiple_containers`.

Example
=======
.. literalinclude:: examples/intrusive_map.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-intrusive_map]
   :end-before: [pw_containers-intrusive_map]

If you need to add this item to containers of more than one type, see
:ref:`module-pw_containers-multiple_containers`,

---------------------
pw::IntrusiveMultiMap
---------------------
:cc:`pw::IntrusiveMultiMap` provides an embedded-friendly, tree-based, intrusive
multimap implementation. This is very similar to
:ref:`module-pw_containers-intrusive_map`, except that the tree may contain
multiple items with equivalent keys.

This class is similar to ``std::multimap<K, V>``. Items to be added must derive
from ``pw::IntrusiveMultiMap<K, V>::Item`` or an equivalent type.

See also :ref:`module-pw_containers-multiple_containers`.

Example
=======
.. literalinclude:: examples/intrusive_multimap.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-intrusive_multimap]
   :end-before: [pw_containers-intrusive_multimap]

If you need to add this item to containers of more than one type, see
:ref:`module-pw_containers-multiple_containers`.

------------------
pw::DynamicHashMap
------------------
:cc:`pw::DynamicHashMap` is an unordered associative container, similar to
``std::unordered_map``, but optimized for memory-constrained environments.

Key features of :cc:`pw::DynamicHashMap`:

* **Allocator-driven**: Uses a :cc:`pw::Allocator` for all memory operations.
* **Hybrid Storage**:
    * **Nodes**: Stored in a dense ``pw::DynamicPtrVector`` to enable efficient,
      linear iteration.
    * **Buckets**: Stored in a ``pw::DynamicDeque`` for ``O(1)`` average lookup.
* **Fallible API**: Adds ``try_*`` versions of operations (e.g., ``try_insert``,
  ``try_emplace``, ``try_rehash``) that return ``std::nullopt`` or ``false`` on
  allocation failure instead of crashing.
* **Unstable Iteration**: Uses "swap-and-pop" erasure for efficiency.
  Erasing an element moves the last element of the map into the erased
  position, changing the iteration order.
* **Flexible Load Factor**: Supports a load factor up to 500%. While 75% is
  standard for speed, higher limits allow shrinking the bucket array's
  footprint when RAM is more scarce than CPU cycles.

Example
=======
.. literalinclude:: examples/dynamic_hash_map.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-dynamic_hash_map]
   :end-before: [pw_containers-dynamic_hash_map]

-------------
API reference
-------------
Moved: :cc:`pw_containers_maps`

------------
Size reports
------------
The tables below illustrate the following scenarios:

* Scenarios related to ``FlatMap``:

  * The memory and code size cost incurred by adding another ``FlatMap``
  * The memory and code size cost incurred by adding another ``FlatMap`` with
    different key and value types. As ``FlatMap`` is templated on both key and
    value types, this results in additional code being generated.

* Scenarios related to ``IntrusiveMap``:

  * The memory and code size cost incurred by a adding a single
    ``IntrusiveMap``.
  * The memory and code size cost incurred by adding another ``IntrusiveMap``
    with the same key type, but a different value type. As ``IntrusiveMap`` is
    templated on both key and value types, this results in additional code being
    generated.
  * The memory and code size cost incurred by adding another ``IntrusiveMap``
    with the same value type, but a different key type. As ``IntrusiveMap`` is
    templated on both key and value types, this results in additional code being
    generated.

* Scenarios related to ``IntrusiveMultiMap``:

  * The memory and code size cost incurred by a adding a single
    ``IntrusiveMultiMap``.
  * The memory and code size cost incurred by adding another
    ``IntrusiveMultiMap`` with the same key type, but a different value type. As
    ``IntrusiveMultiMap`` is templated on both key and value types, this results
    in additional code being generated.
  * The memory and code size cost incurred by adding another
    ``IntrusiveMultiMap`` with the same value type, but a different key type. As
    ``IntrusiveMultiMap`` is templated on both key and value types, this results
    in additional code being generated.

* The memory and code size cost incurred by a adding both an ``IntrusiveMap``
  and an ``IntrusiveMultiMap`` of the same type. These types reuse code, so the
  combined sum is less than the sum of its parts.

.. include:: maps_size_report
