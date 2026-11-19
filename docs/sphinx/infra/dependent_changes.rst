.. _dependent-changes:

=================
Dependent changes
=================
When a change needs other pending changes, for example a Pigweed change that
breaks a downstream project and the downstream change that fixes it, list them
with ``Depends-on:`` trailers. Tryjobs patch the dependencies in, and Gerrit
won't submit the change until its dependencies are satisfied.

.. TODO: b/230610752 - Remove this paragraph once dependencies on changes to
   the same repository on the same Gerrit host are supported.

Dependencies can't be used for changes to the same repository on the same
Gerrit host (`b/230610752 <https://issuetracker.google.com/230610752>`_). Stack
these changes instead.

.. note::

   ``patches.json`` files still work but are deprecated. See
   :ref:`patches-json`.

.. _creating:

--------
Creating
--------
Add a ``Depends-on:`` trailer for each change this change needs. Trailers
(which Gerrit calls footers) are the ``Key: value`` lines at the end of a
commit message, like ``Change-Id:``. ``Depends-on:`` trailers must be in the
last paragraph of the commit message, and every line in that paragraph must be
a trailer, so put them next to ``Change-Id:`` with no blank lines or other text
in between. Each trailer names the Change-Id of a change, not its number.
Prefix it with the Gerrit host name (the part before ``-review``) if the change
is on a different host.

.. code-block:: output

   pw_foo: Change the Bar API

   Update Bar to take a span instead of a pointer and size.

   Depends-on: I0123456789abcdef0123456789abcdef01234567
   Depends-on: other-host:Ifedcba9876543210fedcba9876543210fedcba98
   Change-Id: I13579bdf13579bdf13579bdf13579bdf13579bdf

Only ``Depends-on`` and ``Depends-On`` are accepted; other capitalizations,
like ``depends-on``, are ignored. Don't include ``-review`` or
``.googlesource.com`` in the host name.

Dependencies are transitive. If A depends on B and B depends on C, tryjobs for
A patch in A, B, and C. Changing ``Depends-on:`` trailers invalidates earlier
tryjob results, so CQ runs the tryjobs again rather than reusing results from
before the change.

Tryjobs handle dependencies as follows:

* Submitted changes are assumed to already be in the checkout. They aren't
  patched in, and their dependencies aren't followed.
* Depending on an abandoned change fails the build.
* Anybody can upload a change, so a dependency is only patched in if the
  uploader of its current patchset is a Googler (or a trusted service account)
  or a Googler has approved it. Any other dependency fails the build.
* Dependencies on hosts the builder can't read are skipped. For example, public
  builders skip dependencies on internal hosts, while internal builders patch
  them in. Change-Ids that aren't found are skipped too. In both cases, the
  change still can't be submitted until the dependency checker can find the
  dependency and it has been submitted and is visible to the change (see
  :ref:`submitting`).
* If a Change-Id matches more than one change (for example, after a
  cherry-pick), the change on the same branch as the depending change is used,
  or failing that, the only open change. If several changes are open and not
  exactly one is on the same branch, the build fails. List the dependency in a
  ``patches.json`` file instead.
* Malformed trailers fail the build.

.. _submitting:

----------
Submitting
----------
Changes with ``Depends-on:`` trailers can't be submitted until a dependency
checker votes ``Dependencies-Satisfied +1``. Set the auto-submit label
(``Pigweed-Auto-Submit`` for Pigweed) to +1 and the change will be submitted
once the vote and its other requirements are in place.

.. _dependent-changes-visibility:

Visibility
==========
Submitting a dependency isn't enough. The checker doesn't vote on a change
until each of its dependencies has been submitted *and* is visible to the
change, meaning the dependency has :ref:`rolled <docs-rollers>` to wherever the
change will see it.

For example, change A in repository ``a`` depends on change B in repository
``b``, and both ``a`` and ``b`` roll into repository ``c``:

#. B is submitted. A still isn't submittable, because the copy of ``b`` in
   ``c`` doesn't include B yet.
#. The roller for ``b`` rolls B into ``c``. Now B is visible to A.
#. The checker votes ``Dependencies-Satisfied +1`` on A, and A can be
   submitted.

The checker doesn't react to rolls immediately, so there's a brief delay
between B rolling into ``c`` and the vote on A.

Because the checker waits for dependencies to roll, it's safe to set the
auto-submit label on a change as soon as it's ready. It won't be submitted
before its dependencies are visible to it.

One-way dependencies
====================
If A depends on B but B doesn't depend on A, the checker votes on A once B has
been submitted and is visible to A (see
:ref:`dependent-changes-visibility`).

Codependent changes
===================
Changes that depend on each other are never submitted together. Instead, the
checker comments on them asking for the dependency to be made one-way.
Codependent changes in the same repository aren't supported at all. Combine
them into one change.

To submit codependent changes:

#. Get all the changes passing CQ.
#. Choose the change to submit first. If one of the changes is in a submodule
   of the other's repository, choose the change in the submodule.
#. Remove the ``Depends-on:`` trailer from that change, then submit it. If it
   can't pass CQ without the other change, directly submit it, bypassing CQ.
   This will break the roller, but not the source tree, so others on your team
   are unaffected.

   .. admonition:: Note

      For the main Pigweed repository, only core Pigweed team members can force
      submit, and they must first
      `request a temporary ACL <http://go/pw-cookbook#bypass-cq>`_ to do so.
      This process requires an associated bug, so have one on hand before
      reaching out with a force submission request. Some other projects also
      have limitations on submission outside of CQ.

#. If the other change's repository has the submitted change's repository as a
   submodule, update the submodule pin in the other change to the submitted
   commit hash (in most cases
   ``git submodule update --remote path/to/submodule`` should be sufficient,
   but see the
   `git submodule documentation <https://git-scm.com/book/en/v2/Git-Tools-Submodules>`_
   for full details), add it to the change (``git add path/to/submodule``),
   and push it to Gerrit.
#. Submit the other change. The checker votes on it once its dependency is
   visible to it (see :ref:`dependent-changes-visibility`).

If you bypassed CQ, the roller will start working again after the other change
is submitted.

.. _details:

-------
Details
-------
Sometimes codependent changes must be made in multiple repositories within an
Android Repo Tool workspace or across multiple submodules. Given a situation
where Pigweed change A would break the sample project and sample project change
B fixes it, each change lists the other:

Pigweed change A
  ``Depends-on: sample-project:<Change-Id of B>``

Sample project change B
  ``Depends-on: pigweed:<Change-Id of A>``

When running tryjobs for change A, builders will attempt to patch in change B
as well. For pure Pigweed tryjobs this fails but the build continues. For the
tryjobs that are there to ensure Pigweed doesn't break the sample project, both
change A and change B will be applied to the checkout.

.. _banned-codewords:

----------------
Banned codewords
----------------
Sometimes the name of an internal Gerrit instance is a codeword we don't allow
on the Pigweed Gerrit instance. For example, you may wish to do the following.

Pigweed change A
  ``Depends-on: secret-project:<Change-Id of B>``

Secret-Project change B
  ``Depends-on: pigweed:<Change-Id of A>``

This will be rejected by the Pigweed Gerrit instance because using
"secret-project" is banned on that Gerrit instance and you won't be able to
push. Instead, do the following, using the
`requires-helper <https://pigweed-internal.googlesource.com/requires-helper>`_
repository on the Pigweed-Internal Gerrit instance.

Pigweed change A
  ``Depends-on: pigweed-internal:<Change-Id of C>``

Secret-Project change B
  ``Depends-on: pigweed:<Change-Id of A>``

Pigweed-Internal change C
  ``Depends-on: secret-project:<Change-Id of B>``

The ``pw requires`` command creates change C and adds its trailer to change A.
In this case the command would be ``pw requires secret-project:B``. Run this
inside the Pigweed repository after committing change A. Multiple changes can
be handled by passing multiple arguments to ``pw requires``. Requirements can
also be given as ``<gerrit-name>:I<change-id>`` (for example
``pw requires secret-project:I0123...``). For now, ``pw requires`` also adds
equivalent ``patches.json`` files.

Public builders won't have access to the Pigweed-Internal Gerrit instance so
they won't even be able to see the ``secret-project`` reference. Internal
builders for other internal projects will see the ``secret-project`` reference
but won't be able to resolve it. Builders having access to ``secret-project``
will see all three changes and attempt to patch all three in. Pigweed-Internal
change C is not included in any workspaces so it will never be patched in, but
it transitively applies requirements to public changes. Change C is never
submitted, so the dependency checker treats a dependency on it as a dependency
on change B.

.. _patches-json:

-------------------------
patches.json (deprecated)
-------------------------
Before ``Depends-on:`` trailers, dependencies were listed in a ``patches.json``
file in the root of the repository. These files still work, but use
``Depends-on:`` trailers for new changes.

.. code-block:: json

   [
     {
       "gerrit_name": "pigweed",
       "number": 123456
     },
     {
       "gerrit_name": "pigweed",
       "number": 654321
     }
   ]

Each entry names a change by Gerrit instance name (the part before "-review")
and change number. The repository name is not included. ``patches.json`` files
in dependencies are followed too, along with their ``Depends-on:`` trailers.

Pigweed-related Gerrit hosts reject submission of changes containing
``patches.json`` files, so remove the file (``git rm patches.json``) before
submitting.
