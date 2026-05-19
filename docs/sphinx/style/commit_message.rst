.. _docs-pw-style-commit-message:

====================
Commit message style
====================
Pigweed commit message bodies and summaries are limited to 72 characters wide to
improve readability, and should be prefixed with the name of the module that the
commit is affecting. The commits should describe what is changed, and why. When
writing long commit messages, consider whether the content should go in the
documentation or code comments instead. For example:

.. code-block:: none

   pw_some_module: Short capitalized description

   Details about the change here. Include a summary of what changed, and a clear
   description of why the change is needed. Consider what parts of the commit
   message are better suited for documentation or code.

   - Added foo, to fix issue bar
   - Improved speed of qux
   - Refactored and extended qux's test suite

-----------------------------
Include both "what" and "why"
-----------------------------
It is important to include a "why" component in most commits. Sometimes, why is
evident - for example, reducing memory usage, optimizing, or fixing a bug.
Otherwise, err on the side of over-explaining why, not under-explaining why.

When adding the "why" to a commit, also consider if that "why" content should go
into the documentation or code comments.

.. admonition:: **Yes**: Reasoning in commit message
   :class: checkmark

   .. code-block:: none

      pw_sync_xrtos: Invoke deadlock detector

      During locking, run the deadlock detector if there are enough cycles.
      Though this costs performance, several bugs that went unnoticed would have
      been caught by turning this on earlier. Take the small hit by default to
      better catch issues going forward; see extended docs for details.

.. admonition:: **No**: Reasoning omitted
   :class: error

   .. code-block:: none

      pw_sync_xrtos: Invoke deadlock detector

      During locking, run the deadlock detector if there are enough cycles.

.. _docs-pw-style-commit-message-imperative:

---------------
Imperative mood
---------------
Use `imperative mood <https://en.wikipedia.org/wiki/Imperative_mood>`_
in the commit message title. Do not use third-person singular present tense.
In general, the commit message title should start with a verb, and that verb
should not end in ``s``.

.. tab-set::

   .. tab-item:: Yes

      pw_string: Update docs

   .. tab-item:: No

      pw_string: Updates docs

---------------------------------------
Documentation instead of commit content
---------------------------------------
Consider whether any of the commit message content should go in the
documentation or code comments and have the commit message reference it.
Documentation and code comments are durable and discoverable; commit messages
are rarely read after the change lands.

.. admonition:: **Yes**: Created docs and comments
   :class: checkmark

   .. code-block:: none

      pw_i2c: Add and enforce invariants

      Precisely define the invariants around certain transaction states, and
      extend the code to enforce them. See the newly extended documentation in
      this change for details.

.. admonition:: **No**: Important content only in commit message
   :class: error

   .. code-block:: none

      pw_i2c: Add and enforce invariants

      Add a new invariant such that before a transaction, the line must be high;
      and after, the line must be low, due to XXX and YYY. Furthermore, users
      should consider whether they could ever encounter XXX, and in that case
      should ZZZ instead.

---------------------------
Lists instead of paragraphs
---------------------------
Use bulleted lists when multiple changes are in a single change. Ideally, try to
create smaller changes so this isn't needed, but larger changes are a practical
reality.

.. admonition:: **Yes**: Uses bulleted lists
   :class: checkmark

   .. code-block:: none

      pw_complicated_module: Pre-work for refactor

      Prepare for a bigger refactor by reworking some arguments before the larger
      change. This change must land in downstream projects before the refactor to
      enable a smooth transition to the new API.

      - Add arguments to MyImportantClass::MyFunction
      - Update MyImportantClass to handle precondition Y
      - Add stub functions to be used during the transition

.. admonition:: **No**: Long paragraph is hard to scan
   :class: error

   .. code-block:: none

      pw_foo: Many things in a giant BWOT

      This change does A, B, and C. The commit message is a Big Wall Of Text
      (BWOT), which we try to discourage in Pigweed. Also changes X and Y,
      because Z and Q. Furthermore, in some cases, adds a new Foo (with Bar,
      because we want to). Also refactors qux and quz.

------------
Subject line
------------
The subject line (first line of the commit) should take the form ``pw_<module>:
Short description``. The module should not be capitalized, but the description
should (unless the first word is an identifier). See below for the details.

.. admonition:: **No**: Uses a non-standard ``[]`` to indicate module:
   :class: error

   .. code-block:: none

      [pw_foo]: Do a thing

.. admonition:: **No**: Has a period at the end of the subject
   :class: error

   .. code-block:: none

      pw_bar: Do something great.

.. admonition:: **No**: Puts extra stuff after the module which isn't a module.
   :class: error

   .. code-block:: none

      pw_bar/byte_builder: Add more stuff to builder

Multiple modules
================
Commits often change code across multiple modules. Adapt the module prefix
accordingly.

#. **Changes primarily for one module** - If the commit is primarily concerned
   with one module, only list that module, even if there are changes in other
   modules. This is the most common case.
#. **2-3 modules**: If the commit is focused on two or three modules, list the
   the modules separated by commas, as shown below.
#. **>3 modules** - Use a general prefix (``GN: Fix deps``) or omit the
   prefix entirely.

.. admonition:: **Yes**: Small number of modules affected; use comma separated
   list
   :class: checkmark

   .. code-block:: none

      pw_foo,pw_bar,pw_baz: Change something in a few places

      When changes cross a few modules, include them with the syntax shown
      above.

.. admonition:: **Yes**: Many modules changed
   :class: checkmark

   .. code-block:: none

      Change convention for how errors are handled

      When changes cross many modules, skip the module name entirely.

.. admonition:: **No**: Too many modules changed for subject
   :class: error

   .. code-block:: none

      pw_a,pw_b,pw_c,pw_d,pw_e,pw_f,pw_g,pw_h,pw_i,pw_j: Change error handling convention

      When changes cross many modules, skip the module name entirely.

Non-standard modules
====================
Most Pigweed modules follow the format of ``pw_<foo>``; however, some do not,
such as targets. Targets are effectively modules, even though they're nested, so
they get a ``/`` character.

.. admonition:: **Yes**:
   :class: checkmark

   .. code-block:: none

      targets/xyz123: Tweak support for XYZ's PQR

      PQR is needed for reason ZXW; this adds a performant implementation.

Capitalization
==============
The text after the ``:`` should be capitalized, provided the first word is not a
case-sensitive symbol.

.. admonition:: **No**: Doesn't capitalize the subject
   :class: error

   .. code-block:: none

      pw_foo: do a thing

      Above subject is incorrect, since it is a sentence style subject.

.. admonition:: **Yes**: Doesn't capitalize the subject when subject's first
   word is a lowercase identifier.
   :class: checkmark

   .. code-block:: none

      pw_foo: std::unique_lock cleanup

      This commit message demonstrates the subject when the subject has an
      identifier for the first word. In that case, follow the identifier casing
      instead of capitalizing.

   However, imperative style subjects often have the identifier elsewhere in
   the subject; for example:

   .. code-block:: none

      pw_foo: Improve use of std::unique_lock

------
Footer
------
We support a number of `git footers`_ in the commit message, such as ``Bug:
123`` in the message below:

.. code-block:: none

   pw_something: Add foo and bar functions

   Bug: 123

The footer syntax is described in the `git documentation
<https://git-scm.com/docs/git-interpret-trailers>`_. Note in particular that
multi-line footers are supported:

.. code-block::none

   pw_something: Add foo and bar functions

   Test: Carried out manual tests of pw_console
     as described in the documentation.

You are encouraged to use the following footers when appropriate:

* ``Bug``: Associates this commit with a bug (issue in our `bug tracker`_). The
  bug will be automatically updated when the change is submitted. When a change
  is relevant to more than one bug, include multiple ``Bug`` lines, like so:

  .. code-block:: none

     pw_something: Add foo and bar functions

     Bug: 123
     Bug: 456

* ``Fixed`` or ``Fixes``: Like ``Bug``, but automatically closes the bug when
  submitted.

  .. code-block:: none

     pw_something: Fix incorrect use of foo

     Fixes: 123

* ``Test``: The author can use this field to tell the reviewer how the change
  was tested. Typically, this will be some combination of writing new automated
  tests, running automated tests, and manual testing.

  Note: descriptions of manual testing procedures belong in module
  documentation, not in the commit message. Use the ``Test`` field to attest
  that tests were carried out, not to describe the procedures in detail.

  .. code-block:: none

     pw_something: Fix incorrect use of foo

     Test: Added a regression unit test.

In addition, we support all of the `Chromium CQ footers`_, but those are
relatively rarely useful.

.. _bug tracker: https://bugs.chromium.org/p/pigweed/issues/list
.. _Chromium CQ footers: https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/infra/cq.md#options
.. _git footers: https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/git-footers.html
