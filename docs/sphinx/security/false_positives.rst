.. _docs-security-false_positives:

========================
Handling false positives
========================
Agentic security scanning occasionally finds certain issues that it thinks are
security vulnerabilities but a human reviewer can tell are not. Pigweed
developers are asked to take the following steps to minimize the agents
stumbling over these false positives and creating toil.

--------------
Add assertions
--------------
First, try to update the code to include a check or assertion that constrains
certain fields or parameters to non-vulnerable values. For example, if a
certain routine is only vulnerable if a member of a class is zero, but you know
that the value is never zero, add an assertion that it isn’t zero.

Typically, you can use :c:macro:`PW_DASSERT` or :c:macro:`PW_DCHECK`,
depending on if you are adding to a header or source file, respectively.
Consider the performance implications of these, as at least some projects leave
these debug assertions enabled.

Add a code comment (not a Doxygen comment) to the assertion referencing the
false positive bug report. For example:

.. code-block:: cpp

   // This code is only called when `completed_` is non-zero.
   // See b/8675309 for more details.
   PW_DCHECK_UINT_NE(completed_, 0);
   auto time_per_item = (chrono::SystemClock::now() - start) / completed_;

These should provide agents enough context to recognize the vulnerability as
unreachable.

------------------
Add security notes
------------------
Sometimes, there isn’t a clear condition to assert or the cost of asserting it
would be prohibitive. In these cases, you can inform the scanner agents
directly using comments, either code comments (``//``) or Doxygen comments
(``///``).

The agents are instructed to pay special attention to any comment lines between
``@security`` and ``@endsecurity``. Doxygen has been configured to ignore these
lines. For example:

.. code-block:: cpp

   /// Handles events when invoked by dispatcher-registered callbacks.
   ///
   /// @security
   ///
   /// This object is referenced by a callback owned by the dispatcher. It
   /// appears to have a use-after-free vulnerability if the object is
   /// destroyed before a callback is called, but the dispatcher is
   /// notified by the destructor and ensures all related callbacks are
   /// deregistered first and will never be called.
   ///
   /// See b/8675309 for more details.
   ///
   /// @endsecurity

As with the assertions, please include the bug number in the security note.

.. note::

   There is a doc skill to help add security notes. Simply use a prompt like:
   "Add a security note to MySymbol in my_module/my_file.cc for b/8675309."

--------------------
Update threat models
--------------------
If you find you have multiple cases of the same or similar false positives
within a model, you should update the module’s threat model with details
identifying the pattern of false positives.

The module threat models have names like ``my_module/security.rst`` and follow
the
:cs:`threat model template <docs/sphinx/security/security.template.rst>`.
