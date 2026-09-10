.. _module-pw_containers-lists:

=====
Lists
=====
.. pigweed-module-subpage::
   :name: pw_containers

A linked list is an ordered collection of items in which each item is associated
with pointers to one or more of its adjacent items. Pigweed provides intrusive
lists, meaning the pointers are stored within the items themselves, allowing an
arbitrary number of items to be added without requiring additional memory
beyond that of the items themselves. Pigweed also provides
:cc:`pw::ForwardList`, a dynamically allocated singly linked list backed by a
:cc:`pw::Allocator`.

.. _module-pw_containers-intrusive_list:

-----------------
pw::IntrusiveList
-----------------
:cc:`pw::IntrusiveList` provides an embedded-friendly, double-linked,
intrusive list implementation. An intrusive list is a type of linked list that
embeds list metadata, such as a "next" pointer, into the list object itself.
This allows the construction of a linked list without the need to dynamically
allocate list entries.

This class is similar to ``std::list<T>``, except that the type of items to be
added must derive from ``pw::IntrusiveList<T>::Item``.

In C, an intrusive list can be made by manually including the "next" pointer as
a member of the object's struct. ``pw::IntrusiveList`` uses C++ features to
simplify the process of creating an intrusive list. It provides classes that
list elements can inherit from, protecting the list metadata from being accessed
by the item class.

.. note::
   Originally, ``pw::IntrusiveList<T>`` was implemented as a singly linked list.
   To facilitate migration to ``pw::IntrusiveForwardList<T>::Item``, this
   original implementation can still be temporarily used by enabling the
   ``PW_CONTAINERS_USE_LEGACY_INTRUSIVE_LIST`` module configuration option.

See also :ref:`module-pw_containers-multiple_containers`.

.. _module-pw_containers-intrusive_list-example:

Example
=======
.. literalinclude:: examples/intrusive_list.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-intrusive_list]
   :end-before: [pw_containers-intrusive_list]

If you need to add this item to containers of more than one type, see
:ref:`module-pw_containers-multiple_containers`,

------------------------
pw::IntrusiveForwardList
------------------------
:cc:`pw::IntrusiveForwardList` provides an embedded-friendly, singly linked,
intrusive list implementation. It is very similar to
:ref:`module-pw_containers-intrusive_list`, except that it is singly rather than
doubly linked.

This class is similar to ``std::forward_list<T>``, but items to be added must
derive from ``pw::IntrusiveForwardList<T>::Item``.

See also :ref:`module-pw_containers-multiple_containers`.

Example
=======
.. literalinclude:: examples/intrusive_forward_list.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-intrusive_forward_list]
   :end-before: [pw_containers-intrusive_forward_list]

If you need to add this item to containers of more than one type, see
:ref:`module-pw_containers-multiple_containers`.

.. _module-pw_containers-forward_list:

---------------
pw::ForwardList
---------------
:cc:`pw::ForwardList` provides an embedded-friendly, singly linked list that
owns its elements and allocates them dynamically using a :cc:`pw::Allocator`.
It provides an interface similar to ``std::forward_list<T>``. Unlike
:cc:`pw::IntrusiveForwardList`, items do not need to inherit from a certain
base.

Internally, ``pw::ForwardList`` wraps :cc:`pw::IntrusiveForwardList` to maximize
code reuse.

Key features of :cc:`pw::ForwardList`:

* **Allocator-driven**: Uses a :cc:`pw::Allocator` to allocate each node.
* **Infallible and fallible APIs**: Provides a standard
  ``std::forward_list``-like API (such as ``push_front`` and ``resize``), as
  well as fallible ``try_*`` versions (such as ``try_push_front`` and
  ``try_resize``) that return a boolean or iterator on allocation failure
  without exceptions.

.. note::
   The allocator associated with a ``ForwardList`` must outlive the list.

------------------
pw::IntrusiveQueue
------------------
:cc:`pw::IntrusiveQueue` provides an embedded-friendly, singly linked, intrusive
queue implementation. It wrapper around :cc:`pw::IntrusiveForwardList` that
tracks the tail element, allowing for ``O(1)`` ``push_back()`` operations. Note
that like a standard queue, it does not support ``pop_back()`` since doing so
would require an ``O(n)`` traversal to update the tail pointer.

This class is most similar to ``std::deque<T>``.  It offers ``push_back()``,
``push_front()``, and ``pop_front()``, but not ``pop_back()``.  Because it is an
intrusive list, its elements must derive from ``pw::IntrusiveQueue<T>::Item``.

-------------
API reference
-------------
Moved: :cc:`pw_containers_lists`

Performance considerations
==========================
Items only include pointers to the next item. To reach previous items, the list
maintains a cycle of items so that the first item can be reached from the last.
This structure means certain operations have linear complexity in terms of the
number of items in the list, i.e. they are "O(n)":

- Removing an item from a list using
  ``pw::IntrusiveForwardList<T>::remove(const T&)``.
- Getting the list size using ``pw::IntrusiveForwardList<T>::size()``.

When using a ``pw::IntrusiveForwardList<T>`` in a performance-critical section
or with many items, authors should prefer to avoid these methods. For example,
it may be preferable to create items that together with their storage outlive
the list.

Notably, ``pw::IntrusiveForwardList<T>::end()`` is constant complexity (i.e.
"O(1)"). As a result iterating over a list does not incur an additional penalty.

.. _module-pw_containers-intrusivelist-size-report:

Size reports
------------
The tables below illustrate the following scenarios:

* Scenarios related to ``IntrusiveList``:

  * The memory and code size cost incurred by a adding a single
    ``IntrusiveList``.
  * The memory and code size cost incurred by adding another ``IntrusiveList``
    with a different type. As ``IntrusiveList`` is templated on type, this
    results in additional code being generated.

* Scenarios related to ``IntrusiveForwardList``:

  * The memory and code size cost incurred by a adding a single
    ``IntrusiveForwardList``.
  * The memory and code size cost incurred by adding another
    ``IntrusiveForwardList`` with a different type. As ``IntrusiveForwardList``
    is templated on type, this results in additional code being generated.

* The memory and code size cost incurred by a adding both an ``IntrusiveList``
  and an ``IntrusiveForwardList`` of the same type. These types reuse code, so
  the combined sum is less than the sum of its parts.

.. include:: lists_size_report
