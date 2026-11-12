.. _docs-ci-cq-intro:

===========
CI/CQ Intro
===========

.. _essentials:

----------
Essentials
----------

Submitting Changes
==================
The Gerrit ``Submit`` button is hidden, replaced by the ``Submit to CQ``
button. The ``Submit`` button is still accessible behind the ``...`` button
in the top right corner of the Gerrit UI, but in some cases requires elevated
permissions.

.. _docs-ci-cq-intro-presubmit:

Presubmit checks and continuous integration
===========================================
.. _Pigweed's build console: https://ci.chromium.org/p/pigweed/g/pigweed/console

All Pigweed changelists (CLs) must adhere to Pigweed's style guide and pass a
suite of automated builds, tests, and style checks to be merged upstream. Much
of this checking is done by running :ref:`module-pw_presubmit` in automated builders.
These builders run before each Pigweed CL is submitted and in our
continuous integration infrastructure. See `Pigweed's build console`_.

In addition to the publicly visible presubmit checks, Pigweed runs internal
presubmit checks that are only visible within Google. If any these checks fail,
external developers will see a ``Dry run: Failed builds:`` comment on the CL,
even if all visible checks passed. Reach out to the Pigweed team for help
addressing these issues.

.. _docs-ci-cq-intro-presubmit-projects:

Project presubmit checks
------------------------
.. _examples repo: https://pigweed.googlesource.com/pigweed/examples/

In addition to Pigweed's presubmit checks, some projects that use Pigweed run
their presubmit checks in Pigweed's infrastructure. This supports a development
flow where projects automatically update their Pigweed submodule if their tests
pass. If a project cannot build against Pigweed's tip-of-tree, it will stay on
a fixed Pigweed revision until the issues are fixed. See the `examples repo`_
for a demonstration of this.

Pigweed does its best to keep builds passing for dependent projects. In some
circumstances, the Pigweed maintainers may choose to merge changes that break
dependent projects. This will only be done if:

* A feature or fix is needed urgently in Pigweed or for a different project.
* The project broken by the change does not imminently need Pigweed updates.

The downstream project will continue to build against their last working
revision of Pigweed until the incompatibilities are fixed.

In these situations, Pigweed's commit queue submission process will fail for all
changes. If a change passes all presubmit checks except for known failures, the
Pigweed team may permit manual submission of the CL. Contact the Pigweed team
for submission approval.

Triggering presubmits
=====================
Presubmits are not automatically run when a patch set is uploaded. Click
``CQ Dry Run`` to trigger them. (You can also use
``git push origin +HEAD:refs/for/main%l=Commit-Queue``).

Presubmit validity duration
===========================
If you don't have recent passing results from a ``CQ Dry Run`` (within 24
hours) then ``Submit to CQ`` will run presubmits. After any newly run
presubmits pass this will submit the change.

User interface
==============
If a presubmit fails you'll get a Gerrit comment with a link to the failing
build. The status of tryjobs (pending, running, failed, passed, etc.) is
shown directly in the Gerrit UI (see :ref:`tryjobs`).

.. _docs-ci-cq-intro-coverage:

Code coverage
-------------
Unit test coverage data for C++ is computed by the ``coverage`` builder and
displayed in Gerrit.

.. image:: https://www.gstatic.com/pigweed/gerrit_code_coverage.png
   :width: 800
   :alt: Code coverage display in Gerrit

* **When will coverage data be visible on my CL?** The coverage builder needs
  to finish running (about 6 minutes), and then the data needs to be ingested
  by the coverage pipeline (ran every 30 minutes).

* **What tests is this based on?** Only the C++ unit tests of the modules ran
  as part of the GN build. (There's no coverage data for Python or Rust yet.)

* **Can I generate a coverage report locally?** Yes. Running ``pw
  presubmit --step coverage`` will generate a HTML report at
  ``out/presubmit/coverage/host_clang_coverage/obj/coverage_report/html/index.html``.

* **I'd love to have this in my Pigweed-based project!** See
  :ref:`module-pw_build-gn-pw_coverage_report` for GN and
  :ref:`docs-build_system-bazel_coverage` for Bazel.

Auto-Submit
===========
If you want your change to be automatically submitted when all requirements
are met (``Code-Review +2``, ``OWNERS``-approval, all comments resolved,
etc.) set the ``Pigweed-Auto-Submit`` label to +1. If submission fails it will
be retried a couple times with backoff and then the auto submit job will give
up.

.. _tests-not-needed:

Tests not needed
================
The ``Tests-Not-Needed`` label can be used by a change owner or reviewer to
indicate that while no tests were added with a change to source code there's no
need for tests with this change. This label applies to both the ``C++-Tests``
and the ``Python-Tests`` labels.

If the change owner votes ``Tests-Not-Needed +1`` but the change includes a C++
test file, then the Gerrit submit requirement will fail. This is to keep people
from automatically setting ``Tests-Not-Needed +1`` when uploading changes.

Occasionally, a change will modify both C++ and Python files, but only include
tests for, say, Python. In that case, the ``C++-Tests`` submit requirement will
require ``Tests-Not-Needed +1`` and the ``Python-Tests`` submit requirement will
seem to forbid ``Tests-Not-Needed +1``. However, it only forbids it from the
change owner. To make a change like this submittable, get a reviewer to vote
``Tests-Not-Needed +1`` and make sure the change owner removes that vote.

.. _further-details:

---------------
Further Details
---------------

Applying changes in testing
===========================
Changes are always rebased on the most recent commit when tested. If they
fail to rebase the build fails.
``CQ Dry Run`` is the same as voting ``Commit-Queue +1`` label and
``Submit to CQ`` is the same as voting ``Commit-Queue +2``. If you vote both
``Code-Review +2`` and ``Commit-Queue +2`` on somebody's change you are
submitting it for them.

Post-Submit builders
====================
Jobs are run post-submission too and can be seen at
https://ci.chromium.org/p/pigweed (for public projects) and
https://ci.chromium.org/p/pigweed-internal (for internal projects). Builders can
also be viewed from Pigweed's :ref:`builder visualization <docs-builder-viz>`.

.. _automatic-bisection:

Automatic bisection
-------------------
Post-submit builds often aren't triggered for every commit, but instead trigger
on the most recent commit, effectively grouping multiple commits together. If
one of these builds fails, it can be hard to tell which commit caused the
failure. Pigweed infrastructure implements a `bisection algorithm`_ to detect
these failures and trigger builds to narrow down the possibilities for the
responsible commit. See also Pigweed's `bisector view`_.

.. _bisection algorithm: https://en.wikipedia.org/wiki/Bisection_(software_engineering)
.. _bisector view: https://ci.chromium.org/ui/p/pigweed/g/bisector/builders

Bisection results are clearest on views like the `Pigweed Console`_ or the
`Pigweed Roll Console`_.

.. _Pigweed Console: https://ci.chromium.org/p/pigweed/g/pigweed.pigweed/console
.. _Pigweed Roll Console: https://ci.chromium.org/p/pigweed/g/pigweed.pigweed.roll/console

In many cases automatic bisection will find the responsible commit before users
notice the failure.

The bisector mostly ignores infra failures. That is, it doesn't treat infra
failures as either "good" or "bad"—instead it pretends they don't exist.
However, if there are too many recent infra failures on a builder, it will
refuse to launch new builds of that builder.

The bisector always launches builds using buildbucket never luci-scheduler.
For most builders, this means there may be multiple builds running at once—one
triggered by luci-scheduler (perhaps from a new commit or a cron entry) and one
triggered by the bisector. Rollers are limited to one build at a time.

The bisector assumes there are no flakes, so even if there's a build running it
will still launch a new build because it assumes the running build will
eventually fail and it'll still be useful to know when that builder started
failing.

Once a build is passing at the top of the branch again, the bisector will stop
triggering new builds, even if the commit that caused the (now short-term)
breakage is not yet known.

Rollers launched by the bisector create dry-run changes. Once the bisector has
determined the commit that caused the current failure, the last passing dry-run
change is submitted. This ensures no downstream repository gets a bunch of
consecutive roll changes in its history.

.. _automatic-rerunning:

Automatic rerunning
-------------------
:ref:`automatic-bisection` can determine which commit caused a builder to start
failing, but doesn't help much for flakes. `Pigweed's automatic rerunner`_
finds failing builds and reruns them, without triggering builds on a specific
commit. Builds triggered this way will always take the branch head and run. This
is useful when a roller failed because of a flake—it ensures the downstream
project is updated despite the flake.

.. _Pigweed's automatic rerunner: https://ci.chromium.org/ui/p/pigweed/g/rerunner/builders

If possible, the rerunner launches builds with luci-scheduler instead of
buildbucket. This ensures the rules in luci-scheduler about the maximum number
of concurrent builds are honored, and it allows luci-scheduler to combine
rerunner triggers with triggers resulting from cron or new commits. In some
cases this means the rerunner won't result in any additional builds.

Interactions between bisection and rerunning
--------------------------------------------
The bisector ignores builds triggered solely by the rerunner since they won't be
attributed to specific commits. Builds where a rerunner trigger and one or more
commit triggers were combined will be treated exactly like they didn't have the
rerunner trigger by the bisector.

The rerunner only looks at builds triggered by luci-scheduler. Builds triggered
by the bisector are ignored. This is important because in attributing a failure
to a commit the last build might be a failure, but the builder can still be
passing and does not need any rerunner-triggered builds.

Non-``main`` branches
=====================
CQ is enabled for all branches. If you upload to an individual repository
branch X and the manifest or superproject also has a branch X, that branch of
the manifest will be used.

Rollers
=======
Just because a change has been submitted doesn't mean it's live in the
project. Submodules and Android Repo Tool projects often need to be
:ref:`rolled <docs-rollers>` before they're in the most recent checkout of the
project.

Presubmit result
================
The ``Presubmit-Verified`` label is set at the completion of CQ runs. It does
not block submission, but can be used by Copybara to see if CQ has passed.
If it looks incorrect, do another CQ run and it will be updated.

.. _docs-builder-viz:

Builder Visualization
=====================

Pigweed's builder visualization simplifies the process of browsing, viewing, and
triggering builders. The source-of-truth for it is Google-internal, but there's
a public version without the Google-internal bits.

 *  `Builder Viz link for external contributors <https://pigweed.googlesource.com/infra/config/+/main/generated/pigweed/for_review_only/viz/index.md>`_
 *  `Builder Viz link for Googlers <https://pigweed-internal.googlesource.com/infra/config/+/main/generated/pigweed/for_review_only/viz/index.md>`_

.. _tryjobs:

-------
Tryjobs
-------
The colors of tryjobs in the Gerrit UI indicate the status of the tryjob: gray
is pending/running, green is passed, red is failed or cancelled, and purple is
an infra failure.

Some tryjobs are not yet stable and are run as "experimental". These can be
viewed with ``Show Experimental Tryjobs``. Experimental tryjobs run with CQ but
do not block it.

Individual tryjobs can be run additional times using the ``Choose Tryjobs``
dialog. This can also be used to run tryjobs that would not normally run on the
change. Tryjobs ran this way can be used to satisfy CQ requirements, but don't
block CQ.

.. _prod-vs-dev:

Prod vs Dev
===========
Most builders have "prod" and "dev" versions. The "prod" versions block changes
in CQ and may cause emails to be sent out if they fail in CI. The "dev" builders
test new VM images before they go to "prod", so if a "dev" builder is failing
when a "prod" builder is not failing, then the "dev" builder is failing because
of an upcoming VM change, and teams should take time to get the "dev" builder to
pass. For most projects, "dev" builders show up on the far right of console
views in the LUCI UI.

.. _tryjobs-cli:

Tryjobs CLI
===========

``bb`` command
--------------
The ``bb`` command is available in a bootstrapped Pigweed environment and the
environments of many downstream projects. It is also available from
`Chromium's depot tools <https://chromium.googlesource.com/chromium/tools/depot_tools.git>_`.


Querying tryjobs
----------------
In addition to viewing tryjobs in the Gerrit UI, you can use the ``bb`` command
to query the tryjobs that ran on a change. The command to use is
``bb ls -cl $URL``, but ``$URL`` has two non-obvious requirements:

*  It needs to be a ".googlesource.com" URL and not a Google-internal version of
   that URL.
*  It needs to include the patchset number.

.. code-block:: bash

   $ bb ls -cl https://pigweed-review.googlesource.com/c/pigweed/sample_project/+/53684/1 | egrep -v '^(Tag|By):'
   http://ci.chromium.org/b/8841234941714219488 SUCCESS   'pigweed/try/sample-project-xref-generator'
   Created on 2021-07-19 at 16:45:32, waited 14.8s, started at 16:45:47, ran for 2m43s, ended at 16:48:30
   CL: https://pigweed-review.googlesource.com/c/pigweed/sample_project/+/53684/1

   http://ci.chromium.org/b/8841234941714219504 INFRA_FAILURE 'pigweed/try/sample-project-renode-test'
   Summary: Infra Failure: Step('run pigweed/pw_unit_test/renode/test.sh') (retcode: None)
   Created on 2021-07-19 at 16:45:32, waited 51.6s, started at 16:46:24, ran for 2m40s, ended at 16:49:04
   CL: https://pigweed-review.googlesource.com/c/pigweed/sample_project/+/53684/1

To exclude non-experimental tryjobs, add ``-t cq_experimental:false`` to the
command.

Manually launching tryjobs
--------------------------
In most cases, individual tryjobs can be launched using
``Choose Additional Tryjobs``. If any relevant tryjobs are not listed here
please file a bug.

The ``bb`` command can also be used to launch tryjobs, which can useful for
tracking down race conditions by launching many copies of a tryjob. Please be
careful using this, especially during working hours in California.

.. code-block:: shell

   URL="https://pigweed-review.googlesource.com/c/pigweed/sample_project/+/53684/1"
   TRYJOB="pigweed/sample_project.try/sample-project-xref-generator"
   for i in $(seq 1 25); do
     bb add -cl "$URL" "$TRYJOB"
   done

.. _why-didnt-lintformat-catch:

Why didn't lintformat catch this formatting change?
===================================================
Rolls of tools like clang can update the preferred format of clang-format. There
are two possibilities for addressing this. First, the tool roll could be blocked
until formatting passes. This could require coordinating several changes across
many repositories. This is further complicated if the new formatting preferred
by clang-format is not accepted by the previous version. Second, lintformat can
be configured to only run on changed files. This means downstream project
lintformat tryjobs would not be run on Pigweed changes, nor on rolls of Pigweed
into these projects.

The second choice was selected. This means when tools roll lintformat jobs may
start failing in CI, but they only fail in CQ on changes that touch files
currently failing in CI. Teams should watch their build alert email list and
proactively fix lintformat failures when they come.

.. _dependent-changes:

-----------------
Dependent changes
-----------------
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

Creating
========
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
  dependency and it has been submitted (see :ref:`submitting`).
* If a Change-Id matches more than one change (for example, after a
  cherry-pick), the change on the same branch as the depending change is used,
  or failing that, the only open change. If several changes are open and not
  exactly one is on the same branch, the build fails. List the dependency in a
  ``patches.json`` file instead.
* Malformed trailers fail the build.

.. _submitting:

Submitting
==========
Changes with ``Depends-on:`` trailers can't be submitted until a dependency
checker votes ``Dependencies-Satisfied +1``. Set the auto-submit label
(``Pigweed-Auto-Submit`` for Pigweed) to +1 and the change will be submitted
once the vote and its other requirements are in place.

One-way dependencies
--------------------
If A depends on B but B doesn't depend on A, the checker votes on A once B has
been submitted. It doesn't wait for B to roll anywhere. If A needs B to be
rolled first, for example because B is in a submodule of A's repository, don't
set the auto-submit label on A until B has rolled into A's repository.

Codependent changes
-------------------
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
#. Submit the other change. Its dependency has been submitted, so the checker
   votes on it.

If you bypassed CQ, the roller will start working again after the other change
is submitted.

.. _details:

Details
=======
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

Banned codewords
================
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

patches.json (deprecated)
=========================
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

.. _docs-contributing-appendix-python:

--------------------------------------------------------------
Updating Python dependencies in the virtualenv_setup directory
--------------------------------------------------------------
If you update any of the requirements or constraints files in
``//pw_env_setup/py/pw_env_setup/virtualenv_setup``, you must run this command
to ensure that all of the hashes are updated:

.. code-block:: console

   $ pw presubmit --step update_upstream_python_constraints --full

For Python packages that have native extensions, the command needs to be run 3
times: once on Linux, once on macOS, and once on Windows. See the warning
about caching Python packages for multiple platforms in
:ref:`docs-python-build-downloading-packages`.


Automated Tryjobs
=================
Fortunately, we have tryjob builders to help with the platform-specific
updates, though they are not run automatically. You must manually trigger them
through the Gerrit UI.

#. Upload your change to Gerrit.
#. Use the **Choose Tryjobs** link to add the constraint tryjobs:

   * ``pigweed-linux-python-constraints``
   * ``pigweed-mac-arm64-python-constraints``
   * ``pigweed-windows-python-constraints``

   .. image:: https://www.gstatic.com/pigweed/gerrit_choose_tryjobs.png
      :width: 800
      :alt: Choose Tryjobs link in Gerrit

#. If any jobs fail, the post-failure **logs** step will contain a
   **git_diff.txt** file with the patch that is needed.

   You can easily apply the patch locally by running the command just below,
   updating the ``$BBID`` value to the BuildBucket ID for your failure:

   .. code-block:: console

      $ curl https://logs.chromium.org/logs/pigweed/buildbucket/cr-buildbucket/$BBID/+/u/diff_upstream_python_constraints/logs/git_diff.txt/git_diff.txt?format=raw | git apply

   .. tip::
      Here is an example of constraints tryjob failure:

      * https://ci.chromium.org/b/8690768987505461249

      The ``8690768987505461249`` in the URL is the BuildBucket ID.

   You can then upload a new patchset to Gerrit with the updates from the
   failing builds. Note that failures from each of the tryjobs should result in
   changes to different files.

#. If the job passes, the files are already up-to-date on this host platform
   and no patching is necessary!
