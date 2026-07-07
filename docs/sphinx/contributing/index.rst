.. _docs-contributing:

==================
Contributing guide
==================

.. Note to authors: This guide should be optimized to help people quickly
.. contribute to Pigweed. The guide should be short and concise. We don't want
.. to overwhelm new contributors and make the contribution process feel daunting.
.. Detailed context and edge cases should be put elsewhere.

.. _main repository: https://cs.opensource.google/pigweed/pigweed

Thank you for your interest in improving :ref:`docs-glossary-upstream`! This
guide quickly walks you through the end-to-end contribution process.

.. _good first issues: https://issues.pigweed.dev/issues?q=id:256050233%20OR%20parentid:256050233%2B
.. _bring it home changes: https://pigweed-review.googlesource.com/q/hashtag:%22BringItHome%22

.. tip::

   If you don't have a particular contribution in mind, check out our
   `good first issues`_ and `bring it home changes`_. The bring it home changes
   are incomplete changes that we would be happy to merge but need more work
   before we can accept them.

.. _docs-contributing-help:

----------
Contact us
----------
.. _issue tracker: https://pwbug.dev
.. _public chat room: https://discord.gg/M9NSeTA
.. _corp chat rooms: http://go/pigweed-chat-rooms

If you ever need help, please don't hesitate to contact us:

* Create an issue in our `issue tracker`_ or leave a new comment on an
  existing issue
* Message us in our `public chat room`_
* (Googlers) Message us in our `corp chat rooms`_

.. _docs-contributing-expectations:

------------
Expectations
------------
This is what we expect from all contributors. Please don't send
us code if you can't commit to all of these expectations. You're
welcome to `create an issue <https://pwbug.dev>`_ instead.

.. _docs-contributing-expectations-community:

Community guidelines
====================
.. _Google's Open Source Community Guidelines: https://opensource.google/conduct/

We expect everyone who interacts with the project to respect our
:ref:`docs-code-of-conduct` and `Google's Open Source Community Guidelines`_.

.. _docs-contributing-expectations-complete:

Complete solutions
==================
Contributions are expected to be complete solutions accompanied by
documentation, tests, and :ref:`support for our primary build systems
<docs-contributing-expectations-build-systems>`.

.. _docs-contributing-expectations-build-systems:

Support for primary build systems
---------------------------------
Patches must include support for our three primary build systems: Bazel, GN,
and CMake. Soong is optional but appreciated. We understand that not everyone has
experience with all three builds; but for most patches following the pattern of
existing code is enough. If not, we can help on :ref:`chat <docs-contributing-help>`.

:ref:`docs-contributing-build` provides the common commands that you need when
working with each build system.

.. _docs-contributing-expectations-major-changes:

Communications around major changes
===================================
.. _public chat room: https://discord.gg/M9NSeTA
.. _create an issue: https://pwbug.dev

Before making major changes or :ref:`SEEDs <seed-0001>`, please
:ref:`contact us <docs-contributing-help>`. We always have design
discussions before making large changes.

.. _docs-contributing-requirements:

------------
Requirements
------------
Your :ref:`docs-glossary-development-host` must meet these requirements:

.. _hermetically: https://bazel.build/basics/hermeticity

* Operating system: Linux has the best support, then macOS, then Windows. See
  :ref:`docs-first-time-setup-guide-support-notes`.

* Disk space: After a full build, the Pigweed repository takes up about 10
  gigabytes of disk space. The repo is large because we `hermetically`_
  download many different cross-platform toolchains and compile the code
  for many hardware architectures.

.. _docs-contributing-setup:

-----
Setup
-----

.. _docs-contributing-setup-cla:

Contributor license agreement
=============================
.. _Google Open Source Contributor License Agreement: https://cla.developers.google.com/

Sign the `Google Open Source Contributor License Agreement`_ (CLA). If you've already
signed it for another Google project, you don't need to sign it again.

All Pigweed contributors must sign the CLA. You (or your employer) retain the
copyright to your contribution. The CLA simply gives us permission to use and
redistribute your contributions as part of the project.

.. _docs-contributing-setup-devtools:

Install developer tools
=======================
A few tools must be installed globally on your development host:

#. Complete the :ref:`first-time setup <docs-first-time-setup-guide>` process.

#. :ref:`Install Bazelisk <docs-install-bazel>`.

.. _Pigweed onboarding for Googlers: http://go/pigweed-onboarding#getting-started

If you're a Googler working on a Google-owned development host,
there is some extra required setup. See `Pigweed onboarding for Googlers`_.

.. _docs-contributing-clone:

Clone the repo
==============
#. Clone the upstream Pigweed repository:

   .. code-block:: console

      $ git clone https://pigweed.googlesource.com/pigweed/pigweed

#. ``cd`` into the repo:

   .. code-block:: console

      $ cd pigweed

Gerrit setup
============
.. _Gerrit: https://www.gerritcodereview.com/
.. _new password: https://pigweed.googlesource.com/new-password

Set up your development host to be able to push code to our `Gerrit`_ instance:

#. Obtain a login cookie from Gerrit's `new password`_ page. Make sure
   to sign in with the email address that you used for the
   :ref:`docs-contributing-setup-cla`.

#. Install the Gerrit commit hook so that a ``Change-Id`` is automatically
   added to the bottom of all of your commits messages.

   .. tab-set::

      .. tab-item:: Linux

         .. code-block:: console

            $ f=`git rev-parse --git-dir`/hooks/commit-msg ; mkdir -p $(dirname $f) ; curl -Lo $f https://gerrit-review.googlesource.com/tools/hooks/commit-msg ; chmod +x $f

      .. tab-item:: macOS

         .. code-block:: console

            $ f=`git rev-parse --git-dir`/hooks/commit-msg ; mkdir -p $(dirname $f) ; curl -Lo $f https://gerrit-review.googlesource.com/tools/hooks/commit-msg ; chmod +x $f

      .. tab-item:: Windows

         .. code-block:: console

            $ git rev-parse --git-dir > gitrepopath.txt & set /p "g="< gitrepopath.txt & del gitrepopath.txt & call set "f=%g%/hooks" & call mkdir "%f%" & call curl -Lo "%f%/commit-msg" https://gerrit-review.googlesource.com/tools/hooks/commit-msg

.. _mirror: https://github.com/pigweed-project/pigweed


.. _docs-contributing-change:

------------------
Create your change
------------------
.. _Basic Gerrit walkthrough for GitHub users: https://gerrit-review.googlesource.com/Documentation/intro-gerrit-walkthrough-github.html
.. _Changes: https://gerrit-review.googlesource.com/Documentation/concept-changes.html

.. _docs-contributing-change-gerrit:

Differences between Gerrit and GitHub
=====================================
We do all of our code reviews on `Gerrit`_. Our GitHub `mirror`_ is read-only.
Gerrit's code review workflow is different than GitHub. See `Basic Gerrit
walkthrough for GitHub users`_ and `Changes`_.

.. _docs-contributing-change-git-push:

Pushing changes
===============
Always use this command to push up your code to Gerrit:

.. code-block:: console

   $ git push origin HEAD:refs/for/main

.. _docs-contributing-change-git-amend:

Amending changes
================
When you need to update the code in your change, use ``--amend``:

.. code-block:: console

   $ git commit --amend

This workflow is required because each commit past ``main`` is treated
as a separate change.

If you use incremental commits to help keep track of your work, you will
eventually need to use a ``rebase`` workflow to merge all of the commits
into a single commit:

.. code-block:: console

   $ git rebase -i HEAD~10 # last 10 commits
   # An interactive editor will open. Mark each of your incremental
   # commits with `f` or `fixup` to merge them into the first commit.

.. _docs-contributing-change-message:

Commit message conventions
==========================
Follow our :ref:`docs-pw-style-commit-message`.

We use a specific structured format for commit titles. We have a
:ref:`presubmit check <docs-contributing-review-presubmit>` that
enforces our commit title style.

.. _docs-contributing-review:

-----------
Code review
-----------

.. _docs-contributing-review-presubmit:

Dry run the presubmit checks
============================
If you want to dry run Pigweed's :ref:`presubmit checks
<docs-ci-cq-intro-presubmit>`, ask a :ref:`committer <docs-glossary-committers>`
to kick them off for you. Only committers can start a dry run.

The presubmit checks are our suite of automated builds, tests, linters, and
formatters. When you attempt to :ref:`merge your change
<docs-contributing-review-merge>`, all of the presubmit checks must pass. Dry
running the presubmit checks lets you find and fix issues early.

.. _docs-contributing-review-reviewers:

Selecting reviewers
===================
Look for ``OWNERS`` files near the code that you've contributed. Those people
should be your reviewers. If you can't find owners, add
``gwsq-pigweed@pigweed.google.com.iam.gserviceaccount.com`` as a reviewer. This
will automatically choose an appropriate person to review the change.

.. _docs-contributing-review-merge:

Merging your change
===================
You need 2 :ref:`docs-glossary-committers` to approve your change. Only
committers can merge changes.

.. _docs-contributing-feedback:

--------
Feedback
--------
Thank you very much for contributing to Pigweed! And welcome to the community :)

If you have suggestions on how we can improve our contribution
process you are welcome to send us feedback using the
:ref:`usual channels <docs-contributing-help>`.

.. toctree::
   :maxdepth: 1
   :hidden:

   self
   build
   Code reviews (Gerrit) <https://pigweed-review.googlesource.com>
   ../code_reviews
   Issue tracker <https://issues.pigweed.dev/issues?q=status:open>
   SEEDs <../../seed/0000>
   ../infra/index
   ../embedded_cpp_guide
   ../style_guide
   Code of conduct <pw://code_of_conduct.html>
   docs/index
   ../testing/index
