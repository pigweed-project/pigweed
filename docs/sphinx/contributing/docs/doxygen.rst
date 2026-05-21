.. _contrib-doxygen:

===========================
Doxygen contributor's guide
===========================
This guide shows :ref:`docs-glossary-upstream` maintainers how to contribute
content to Pigweed's :cc:`C/C++ API reference <index>`, which is powered by
`Doxygen`_.

For Doxygen style rules, see :ref:`style-doxygen`.

--------
Overview
--------
Most of ``pigweed.dev`` is built with Sphinx. The C/C++ API reference however is
built with Doxygen. `rules_doxygen <https://github.com/TendTo/rules_doxygen>`_
orchestrates the Doxygen build (``//docs/doxygen:build``) hermetically within
Bazel.

The C/C++ API reference is organized along the lines of Pigweed modules. This is
achieved through Doxygen `groups
<https://www.doxygen.nl/manual/grouping.html>`_. The groups are defined in
:cs:`//docs/doxygen/modules.h`. Header files are annotated with ``@module`` and
``@submodule`` (aliases defined in :cs:`docs/doxygen/Doxyfile`) to group APIs
into their respective Pigweed modules.

.. _contrib-doxygen-quickstart:

----------
Quickstart
----------
#. Check if your module is defined in :cs:`docs/doxygen/modules.h`. If not,
   define it now.

   .. literalinclude:: ../../docs/doxygen/modules.h
      :language: cpp
      :start-at: /// @defgroup pw_bytes pw_bytes
      :end-at: /// @endmaindocs

   The pattern is ``@defgroup <id> <title>``. If ``<title>`` is omitted,
   Doxygen mistakenly capitalizes the module as ``Pw_bytes``.

   ``@brief`` should contain the module's tagline as defined in
   :cs:`docs/sphinx/module_metadata.json`. If your module doesn't have a
   tagline, now is a good time to create one!

   The content between ``@maindocs`` and ``@endmaindocs`` should be a complete
   list of links to the module's Sphinx docs.

#. (Optional) If your module has a large, complex API, define "submodule"
   groups (such as the ``pw_bytes_ptr`` subgroup shown below) in
   :cs:`docs/doxygen/modules.h` to help further organize your module's API
   reference:

   .. literalinclude:: ../../docs/doxygen/modules.h
      :language: cpp
      :start-at: /// @defgroup pw_bytes_ptr Packed pointers
      :end-at: /// @brief Store data in unused pointer bits

   See :cc:`pw_bytes` to view the rendered example. The ``pw_bytes`` group
   controls the module's API reference landing page. The main page is
   organized by "submodules". The ``pw_bytes_ptr`` group is one of the
   submodules. On that page, all APIs related to packed pointers are
   presented.

#. In your module's main ``BUILD.bazel`` file (e.g. ``//pw_bytes/BUILD.bazel``)
   create a ``filegroup`` named ``doxygen``. For ``srcs``, list all Doxygen
   inputs:

   .. literalinclude:: ../../pw_bytes/BUILD.bazel
      :language: cpp
      :start-after: # DOCSTAG: [doxygen]
      :end-before: # DOCSTAG: [doxygen]

   .. important::

      The Doxygen build is hermetic and all inputs are explicitly defined. If
      you forget to include a header at this stage, Doxygen won't know that the
      header exists.

#. Add your target to ``//docs/doxygen:srcs``:

   .. code-block:: py

      filegroup(
          name = "srcs",
          srcs = [
              # …
              "//pw_bytes:doxygen",
              # …
          ]
      )

#. In each of the headers that you've listed in your module's ``doxygen``
   target, add ``@module`` or ``@submodule`` annotations. Always explicitly
   close with ``@endmodule`` or ``@endsubmodule``.

   For simple modules where you want the entire API listed on a single page,
   use ``@module``:

   .. code-block:: cpp

      namespace pw {

      /// @module{pw_foo}

      // …

      /// @}

      }  // namespace pw

   For more complex modules with large APIs, use ``@submodule``:

   .. code-block:: cpp

      namespace pw {

      /// @submodule{pw_bytes,ptr}

      // …

      /// @}

      }  // namespace pw

   ``@submodule{pw_bytes,ptr}`` will organize the API under the
   ``pw_bytes_ptr`` group that's defined in :cs:`docs/doxygen/modules.h`.

   The lack of whitespace between ``pw_bytes`` and ``ptr`` is important!

   Remember to use ``@}`` to close off your annotation!

#. Follow the style rules in :ref:`style-doxygen`.

.. _contrib-doxygen-doxylink:

---------------------------
Link from Sphinx to Doxygen
---------------------------
To link from the Sphinx docs to a Doxygen page, use the ``:cc:`` role. See
:ref:`docs-style-rest-doxylink` for usage and style guidance.

.. _Doxygen: https://www.doxygen.nl/index.html

.. _contrib-doxygen-links:

---------------------------
Link from Doxygen to Sphinx
---------------------------
Use normal Markdown links with relative paths. There is not yet any utility
like ``:cc:`` for managing Doxygen-to-Sphinx links.

.. literalinclude:: ../../docs/doxygen/modules.h
   :language: cpp
   :start-at: /// [Home](../../pw_bytes/docs.html)
   :end-at: /// [Home](../../pw_bytes/docs.html)

.. tip::

   Doxygen outputs all files into a single directory, so the Sphinx docs are
   always available two directories below (``../..``).

.. _Doxygen: https://www.doxygen.nl/index.html

.. _contrib-doxygen-trouble:

---------------
Troubleshooting
---------------
Each section below is a verbatim error message thrown by Doxygen.
``…`` represents variable text. The list at the start of a section
explains how to resolve the error. The first item is the most probable
fix, the second item the second most probable, etc.

General tips:

* The `tagfile <https://www.doxygen.nl/manual/external.html>`_ generated at
  ``//bazel-bin/docs/sphinx/_docs/_sources/doxygen/api/cc/index.tag`` is
  essentially an authoritative index of the entire public API, as Doxygen
  understands it.

* Tweak the Doxygen `config <https://www.doxygen.nl/manual/config.html>`_
  (``//docs/doxygen/Doxyfile``) as needed and then re-run the build.

``… has @param documentation sections but no arguments``
========================================================
* Remove all ``@param`` documentation because the function has no args.

``@copybrief or @copydoc target … not found``
=============================================
* Make sure that the target exists. It may have been removed recently.

* Disambiguate the namespace of the target:

  .. code-block:: diff

     -  /// @copydoc internal::GenericIntrusiveList<ItemBase>::clear
     +  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::clear

* Fully qualify the target:

  .. code-block:: diff

     -  /// @copydoc MultiBufChunks::insert
     +  /// @copydoc pw::multibuf::v1::MultiBufChunks::insert

* Remove backticks from the target name.

  .. code-block:: diff

     -  /// @copydoc `BucketBase::Remove`
     +  /// @copydoc BucketBase::Remove

* Specify the exact signature of the target method.

  .. code-block:: diff

     -  /// @copydoc Base::Method
     +  /// @copydoc Base::Method(int)
        void Method(int x);

* Check if the target has a custom annotation and then look for that annotation
  in the ``PREDEFINED`` list in ``//docs/doxygen/Doxyfile``. For example, the
  following ``@copydoc`` was technically correct:

  .. code-block:: cpp

     /// @copydoc MultiBufChunks::push_front
     void PushFrontChunk(OwnedChunk&& chunk) {
       MultiBufChunks::push_front(std::move(chunk));
     }

   The ``MultiBufChunks`` class did indeed have a ``push_front`` method. The
   issue was that ``MultiBufChunks`` was annotated with
   ``PW_MULTIBUF_DEPRECATED`` and Doxygen was instructed to ignore this
   definition. See `Preprocessing
   <https://www.doxygen.nl/manual/preprocessing.html>`_.

   .. code-block:: text

      PREDEFINED             = … \
                               PW_MULTIBUF_DEPRECATED=

``Argument … from the argument list of … has multiple @param documentation sections``
=====================================================================================
* Document each ``@param`` name only once.

* There must be a space between ``@param`` and the parameter name. The text
  in brackets (e.g. ``@param[*]``) is only for specifying direction. The only 3
  valid direction values are ``in``, ``out``, and ``in,out``.

  .. code-block:: diff

     -  /// @param[dest] The memory area to copy to.
     +  /// @param dest The memory area to copy to.

``Argument … of command @param is not found in the argument list``
==================================================================
* Match the ``@param`` name to the function signature name exactly.

  .. code-block:: diff

     -  /// @param foo The integer...
     -  constexpr size_t EncodedSize(T value)
     +  /// @param value The integer...
     +  constexpr size_t EncodedSize(T value)

* If the function signature does not name the parameter, provide a
  name and use that for ``@param``. Avoid unused parameter warnings by
  keeping it unnamed in the implementation.

  .. code-block:: diff

     -  /// @param Status A status...
     -  void FinalizeRead(Status) override;
     +  /// @param status A status...
     +  void FinalizeRead(Status status) override;

* Delete the ``@param`` documentation if the parameter has been recently
  removed from the code.

* Use ``@tparam`` for template parameters, not ``@param``.

  .. code-block:: diff

     -  /// @param kMaxSize Size of the largest allocation...
     +  /// @tparam kMaxSize Size of the largest allocation...
        template <size_t kMaxRequests, size_t kMaxSize>
        auto ArbitraryRequests()

* When using ``@copydoc``, the function signatures need to match.

``Detected potential recursive class relation``
===============================================
* Hide the inheritance from Doxygen using the ``@cond`` and
  ``@endcond`` directives. Also leave a comment pointing to
  :bug:`513051956`.

  .. code-block:: text

     template <typename T, size_t kCapacity>
     class MyClass
     // TODO: b/513051956 - Fix `recursive class relation` error
     /// @cond
         : public MyClass<T, kGenericSized>
     /// @endcond
     {

.. note::

   This warning occurs when a class template inherits from another instantiation
   of itself (for example, a capacity-specific version inheriting from a
   generic-capacity version). Doxygen's parser struggles to resolve template
   specializations and suspects circular inheritance.

.. _contrib-doxygen-trouble-symbol:

``Documented symbol … was not declared or defined``
===================================================
* Instruct Doxygen to expand or ignore the symbol by adding it to the
  ``PREDEFINED`` list in :cs:`docs/doxygen/Doxyfile`.

  To ignore the symbol (expand to nothing):

  .. code-block:: text

     PREDEFINED             = ... \
                              MY_SYMBOL(...)= \

  To define it to a specific value:

  .. code-block:: text

     PREDEFINED             = ... \
                              MY_SYMBOL(...)=value \

.. note::

   Doxygen's parser can get confused by macros that look like function calls
   or are placed in locations where they might resemble C++ syntax (e.g.
   trailing return types). This often happens when macro arguments contain
   complex C++ syntax (e.g., ``->`` inside arguments).

.. _contrib-doxygen-trouble-group:

``End of file while inside a group``
====================================
* Close all ``@module`` and ``@submodule`` groups with ``@endmodule`` or
  ``@endsubmodule``.

* Close all explicit group start markers (``@{``) with group end markers
  (``@}``).

* If a file contains ``@defgroup`` and ``@{``, check if the group should be
  removed from the header and instead defined centrally in
  ``//docs/doxygen/modules.h`` via ``@module`` or ``@submodule``.

* Avoid nesting ``@cond`` blocks inside ``@submodule`` or
  ``@module`` groups. Close the group with ``@endsubmodule`` or ``@endmodule``
  before starting a ``@cond`` block, and reopen the group after ``@endcond`` if
  necessary.

* Avoid C-style strings inside of ``@code`` blocks.

  .. code-block:: diff

     /// @code

     -  ///   Layout MyGetLayoutFromPointer(const void* ptr) { /* ... */ }
     +  ///   Layout MyGetLayoutFromPointer(const void* ptr) {
     +  ///     …
     +  ///   }
        /// @endcode

.. note::

   Pigweed does a lot of customization to force Doxygen to organize everything
   along the lines of Pigweed modules.  The module-based organization is mainly
   achieved through the ``@module`` and ``@submodule`` groups defined in
   :cs:`docs/doxygen/modules.h`. These are custom aliases; see
   :cs:`docs/doxygen/Doxyfile` to understand what the aliases expand to. Most
   grouping errors are fixed by using ``@module`` and ``@submodule`` properly.
   Headers occasionally use explicit start and end markers (``@{`` and ``@}``) to
   create custom groups, which can also lead to grouping errors.

``Explicit link request to … could not be resolved``
====================================================
* Use ``@code`` and ``@endcode`` blocks instead of inline backticks
  to demonstrate the code.

  .. code-block:: diff

     -  /// To use, add ``using ::pw::operator""_b;``
     +  /// To use, add:
     +  /// @code
     +  /// using ::pw::operator""_b;
     +  /// @endcode

.. note::

   Doxygen's autolink feature might try to resolve links for operator names
   (like ``operator""_b``) even when they are in inline code backticks, and
   fail if it cannot resolve them.

``Found ')' without opening '(' for trailing return type``
==========================================================
* See :ref:`contrib-doxygen-trouble-symbol`.

``Found documented return type for … that does not return anything``
====================================================================
* Do not use ``@return`` or ``@returns`` for constructors, destructors, or
  functions returning ``void``.

``Found recursive @copybrief or @copydoc relation for argument …``
==================================================================
* Break the cycle by documenting one of the entities directly instead
  of using ``@copydoc`` or ``@copybrief``, and have the other entity copy from
  the now-directly documented one.

``Found unknown command``
=========================
* Check `Special Commands <https://www.doxygen.nl/manual/commands.html>`_ to
  verify that the command is valid.

* Use ``@p <name>`` to refer to a parameter, or wrap ``<name>`` in single backticks.
  Do not use ``@<name>`` because Doxygen will interpret it as a command.

  .. code-block:: diff

     -  /// Creates a combined view of a @code_point and its encoded @size.
     +  /// Creates a combined view of a @p code_point and its encoded `size`.

* Do not use ``@important``. Use ``@attention`` or ``@warning``` instead. ``@important``
  was introduced in a later version of Doxygen than what Pigweed uses.

  .. code-block:: diff

     -  /// @important This is...
     +  /// @attention This is...

``Include file … not found``
============================
* Make sure that all inputs that Doxygen depends on are explicitly
  declared in the ``doxygen`` target of a relevant ``BUILD.bazel`` file.

``Missing title after \defgroup``
=================================
* Usually, ``defgroup`` should not be used at all. Instead, define
  a ``@submodule`` in ``//docs/doxygen/modules.h``.

* If the ``defgroup`` must exist in the header for some reason,
  adding a title after the group ID should fix the missing title warning.

  .. code-block:: text
     :caption: A diff demonstrating how to add a title to a group

     -  ///   @defgroup foo
     +  ///   @defgroup foo Foo

``No uniquely matching class member found``
===========================================
* If the error is related to a ``friend`` declaration, wrap the ``friend`` line in
  ``@cond`` and ``@endcond`` directives and add a comment explaining the suppression.

  .. code-block:: diff

     +  // Suppress `no uniquely matching class member found` Doxygen error
     +  /// @cond
        friend typename Base::Base;
     +  /// @endcond

.. note::

   This error occurs when Doxygen's parser fails to resolve a type or member
   name, often in ``friend`` declarations involving nested template aliases (e.g.
   ``friend typename Base::Base;``) or common names (e.g.
   ``friend internal::Channel<T>;``). Because Doxygen doesn't require ``friend``
   declarations to generate public API documentation, you can safely hide them
   from Doxygen.

``Refusing to add group … to itself``
=====================================
* Look for an extra ``@module`` or ``@submodule``. E.g. 2 ``@module``
  annotations but only 1 ``@endmodule``.

``Unbalanced grouping commands``
================================
* Check if another file in the same ``@module`` or ``@submodule`` group is
  not explicitly closing with ``@endmodule`` or ``@endsubmodule``. **The error
  may not actually be in the file that Doxygen is reporting.**

``Unsupported xml/html tag``
============================
* Wrap any text that uses angle brackets in single backticks to format
  it as inline code. Otherwise Doxygen will interpret the text as inline
  HTML or XML.

  .. code-block:: diff

     -  /// Container-based version of the <algorithm> functions
     +  /// Container-based version of the `<algorithm>` functions
