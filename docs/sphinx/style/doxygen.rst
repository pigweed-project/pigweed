.. _style-doxygen:

===================
Doxygen style guide
===================
Pigweed's :cc:`C/C++ API reference <index>` is powered by `Doxygen`_. All
Doxygen comments in :ref:`docs-glossary-upstream` must adhere to this style
guide.

If you're looking for help with contributing to the upstream Pigweed Doxygen
system, see :ref:`contrib-doxygen`.

.. _style-doxygen-adopt:

-------------------------
Adopting this style guide
-------------------------
Other projects are welcome to adopt this style guide. A few other Google
projects already have.

To propose changes to this style guide, `file a bug <https://pwbug.dev>`_,
:ref:`submit a change <docs-contributing>`, or `start a discussion
<https://discord.gg/M9NSeTA>`_.

.. note::

   The rules in :ref:`style-doxygen-pw` only apply to upstream Pigweed and
   aren't suitable for general adoption because they rely on Doxygen
   customizations that only exist in the upstream Pigweed repository.

.. _style-doxygen-comments:

--------
Comments
--------
Always use the ``///`` style. No other comment style is allowed.

.. admonition:: Yes
   :class: checkmark

   Triple slash style, single line:

   .. code-block:: cpp

      /// …

   Triple slash style, multiple lines:

   .. code-block:: cpp

      /// …
      /// …
      /// …

.. _style-doxygen-cmd:

--------
Commands
--------
Always use the ``@`` style for `special commands`_. The ``\`` style is not
allowed.

.. admonition:: Yes
   :class: checkmark

   .. code-block:: cpp

      /// @param[in] n The number of bytes to copy.

.. admonition:: No
   :class: error

   .. code-block:: cpp

      /// \param[in] n The number of bytes to copy.

.. _style-doxygen-returns:

@returns
========
Always use ``@returns``, never ``@return``.

.. _style-doxygen-cmd-param:

@param
======
Style rules related to the `@param`_ special command.

.. _style-doxygen-cmd-param-dir:

Direction
---------
Always provide the parameter's direction annotation.

.. admonition:: Yes
   :class: checkmark

   .. code-block:: cpp

      /// @param[in] n The number of bytes to copy.

.. admonition:: No
   :class: error

   .. code-block:: cpp

      /// @param n The number of bytes to copy.

.. _style-doxygen-cmd-param-dir-bi:

Bidirectional values
^^^^^^^^^^^^^^^^^^^^
Always use ``in,out``.

.. admonition:: Yes
   :class: checkmark

   .. code-block:: cpp

      /// @param[in,out] buffer The buffer to store data in.

.. admonition:: No
   :class: error

   Incorrect order:

   .. code-block:: cpp

      /// @param[out,in] buffer The buffer to store data in.

   Using space instead of comma:

   .. code-block:: cpp

      /// @param[in out] buffer The buffer to store data in.

   No whitespace between the values:

   .. code-block:: cpp

      /// @param[inout] buffer The buffer to store data in.

.. _style-doxygen-pw:

----------------------
Upstream Pigweed rules
----------------------
Additional style guide rules for Doxygen comments in upstream Pigweed.

The following style rules are not appropriate for general adoption because they
depend on Doxygen customizations that only exist within the upstream Pigweed
repository.

.. _style-doxygen-pw-status-aliases:

pw::Status aliases
==================
Use the following aliases when referring to :ref:`module-pw_status` codes:

* ``@OK``
* ``@CANCELLED``
* ``@UNKNOWN``
* ``@INVALID_ARGUMENT``
* ``@DEADLINE_EXCEEDED``
* ``@NOT_FOUND``
* ``@ALREADY_EXISTS``
* ``@PERMISSION_DENIED``
* ``@RESOURCE_EXHAUSTED``
* ``@FAILED_PRECONDITION``
* ``@ABORTED``
* ``@OUT_OF_RANGE``
* ``@UNIMPLEMENTED``
* ``@INTERNAL``
* ``@UNAVAILABLE``
* ``@DATA_LOSS``
* ``@UNAUTHENTICATED``

.. _style-doxygen-pw-returns-status:

Functions that return pw::Status
================================
Use the following pattern:

.. code-block:: cpp

   /// @returns
   /// * <alias>: <description>
   /// * <alias>: <description>

Where ``<alias>`` is one of the :ref:`style-doxygen-pw-status-aliases`
and ``<description>`` is an explanation of what
the status code means for this particular function or method.

.. important::

   There must be not be any blank lines between the bullet list items. Doxygen
   ends the ``@returns`` block when it encounteres a blank line.

Example:

.. code-block:: cpp

   /// @returns
   /// * @OK: The bulk read was successful.
   /// * @RESOURCE_EXHAUSTED: The remaining space is too small to hold a new
   ///   block.

.. _style-doxygen-pw-returns-result:

Functions that return pw::Result
================================
Use the following pattern:

.. code-block:: cpp

   /// @returns @Result{<value>}
   /// * <alias>: <description>
   /// * <alias>: <description>

Where ``<value>`` describes the value that's returned in the ``OK`` case,
``<alias>`` is one of the :ref:`pw_status error codes
<style-doxygen-pw-status-aliases>` and ``<description>`` is an explanation
of what the error code means for this particular function or method.

Example:

.. code-block:: cpp

   /// @returns @Result{a sample}
   /// * @RESOURCE_EXHAUSTED: ADC peripheral in use.
   /// * @DEADLINE_EXCEEDED: Timed out waiting for a sample.
   /// * Other statuses left up to the implementer.

See :cc:`TryReadFor() <pw::analog::AnalogInput::TryReadFor>` to view how
this example gets rendered.

.. _style-doxygen-pw-links:

@security
=========
Use ``@security`` and ``@endsecurity`` to annotate code that has been
:ref:`incorrectly flagged <docs-security-false_positives>` by automated security
scanning tools.

Use the following pattern:

.. code-block:: cpp

   /// @security
   ///
   /// <Explanation of why this code is not vulnerable.>
   ///
   /// See b/<bug_number> for additional details.
   ///
   /// @endsecurity

Links
=====
* For linking from Sphinx to Doxygen, see :ref:`contrib-doxygen-doxylink`.
* For linking from Doxygen to Sphinx, see :ref:`contrib-doxygen-links`.

.. _style-doxygen-pw-security-note:

.. _Doxygen: https://www.doxygen.nl/index.html
.. _special comment block: https://www.doxygen.nl/manual/docblocks.html
.. _special commands: https://www.doxygen.nl/manual/commands.html
.. _@param: https://www.doxygen.nl/manual/commands.html#cmdparam
