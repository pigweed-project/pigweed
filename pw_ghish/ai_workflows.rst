.. _module-pw_ghish-ai-workflows:

============
AI Workflows
============
.. pigweed-module-subpage::
   :name: pw_ghish

Gerrit provides commit-based patchsets, fine-grained inline comment threading,
and server-side draft states. However, AI coding agents often struggle with
Gerrit's custom command-line tooling and REST APIs, having been primarily
trained on Git and GitHub CLI workflows.

``pw_ghish`` maps standard GitHub CLI (``gh pr``) commands directly to Gerrit
and LUCI infrastructure. This provides a familiar CLI interface while preserving
Gerrit's review semantics, supporting effective human-agent pair programming
workflows.

This document describes common user journeys for AI-assisted engineering in
Pigweed and Gerrit projects using ``pw_ghish``.

-----------------------------
CUJ 1: Private draft steering
-----------------------------
When guiding an AI agent through complex refactors or multi-file changes, typing
long prompts in a chat window is inefficient. Developers often want to inspect
the code diff visually in Gerrit and annotate exact lines that need changes—without
publishing those notes to other human reviewers or the public change history.

The Workflow
============
1. **Agent pushes initial implementation**:
   The agent writes the code and pushes the change:

   .. code-block:: console

      $ ./gh pr create -t "pw_ring_buffer: Add peek method" --draft

2. **Engineer leaves private draft comments in Gerrit**:
   The engineer opens the Gerrit web UI, reviews the diff, and clicks lines to
   leave inline comments and suggestions. The engineer **does not click Send**;
   the comments remain stored on the Gerrit server as unpublished drafts.

3. **Agent pulls down private drafts and addresses feedback**:
   The engineer simply tells the agent: *"Please address my draft comments."*
   The agent inspects the change with ``pr view --comments``:

   .. code-block:: console

      $ ./gh pr view --comments

   ``pw_ghish`` displays the draft comments clearly marked with file paths, line
   numbers, and draft indicators.

4. **Agent updates the code**:
   The agent implements the requested fixes, runs local unit tests, and uploads
   a new patchset:

   .. code-block:: console

      $ ./gh pr push

Key benefits
============

* **Visual steering**: Engineers can use Gerrit's side-by-side diff viewer
  to direct the agent to specific lines and code contexts.

* **Privacy**: Work-in-progress review notes remain private between the engineer
  and the agent until published.

------------------------------
CUJ 2: Staged review responses
------------------------------
When a reviewer leaves feedback on a change, an agent can address the comments
and prepare fixes. However, allowing an agent to publish public replies directly
risks inaccurate explanations or premature thread resolutions.

``pw_ghish`` addresses this through staged draft replies and resolutions.

The Workflow
============
1. **Agent inspects unresolved reviewer threads**:
   The engineer asks the agent: *"Address the reviewer comments on my change."*
   The agent inspects all active threads:

   .. code-block:: console

      $ ./gh pr view --comments

2. **Agent modifies code and verifies locally**:
   The agent updates the source files to address the reviewer's feedback and
   runs tests:

   .. code-block:: console

      $ bazelisk test //...

3. **Agent stages replies and resolutions as drafts**:
   For each addressed comment, the agent replies using ``--draft`` and
   ``--resolved``:

   .. code-block:: console

      $ ./gh pr comment --path pw_ring_buffer/ring_buffer.cc --line 84 \
          -m "Updated to return pw::Result<ConstByteSpan> instead of raw pointer." \
          --resolved --draft

   The reply is recorded in Gerrit as an unpublished draft reply, and the thread
   is marked resolved in the draft state.

4. **Agent uploads the new patchset**:
   The agent pushes the updated code:

   .. code-block:: console

      $ ./gh pr push

5. **Engineer reviews patch-to-patch diff and sends**:
   The engineer opens Gerrit to verify:

   * Compares the new patchset against the previous patchset in the Gerrit diff viewer.
   * Sees the agent's drafted inline replies positioned right beside each change.
   * If satisfied, the engineer clicks **Send** in Gerrit to publish both the code
     and the explanations. If adjustments are needed, the engineer edits the draft
     replies before publishing.

Key benefits
============

* **Human verification**: Public comments are not published without human
  review, allowing engineers to verify explanations before sending.

* **Direct thread replies**: The agent writes draft replies directly into
  Gerrit's review threads rather than printing them in a chat window.

-----------------------------------
CUJ 3: CL handoff via URL or branch
-----------------------------------
Handing off an existing change to an agent—such as a patch needing updates or
a failing build—often requires multi-step Git fetch refspecs.

With ``pw_ghish``, checking out a change requires only the change number or URL.

The Workflow
============
1. **Handoff prompt**:
   The engineer gives the agent a shortlink or URL:
   *"Can you pick up pwrev/472267, fix the compiler warnings, and get tryjobs green?"*

2. **Agent checks out the change**:
   The agent runs ``pr checkout`` with the shortlink:

   .. code-block:: console

      $ ./gh pr checkout pwrev/472267

   ``pw_ghish`` fetches the change ref from Gerrit, checks out the commit, and
   configures tracking branch metadata.

3. **Agent inspects state**:
   The agent runs ``pr status`` or ``pr checks`` to see live tryjob status and
   ``pr view --comments`` to check for outstanding review feedback:

   .. code-block:: console

      $ ./gh pr status
      $ ./gh pr checks

4. **Agent makes forward progress and pushes**:
   After fixing the code, the agent uploads an updated patchset:

   .. code-block:: console

      $ ./gh pr push

-------------------------------------------------
CUJ 4: Autonomous CI driving and failure repair
-------------------------------------------------
In traditional Gerrit workflows, verifying changes through presubmit imposes a
heavy context-switching penalty on engineers:

1. The engineer uploads a CL with Commit-Queue (``CQ+1``).
2. Presubmit tryjobs run across dozens of builders for 20 to 30 minutes.
3. The engineer is forced to context-switch away to another task.
4. Half an hour later, the engineer must break focus, check the Gerrit web UI,
   sift through Buildbucket builder lists, click through nested step hierarchies
   to inspect LogDog logs, diagnose what failed, and context-switch back to code.

``pw_ghish`` eliminates this round-trip tax by allowing AI agents to autonomously
poll, watch, diagnose, and drive changes to ground.

The Workflow
============
1. **Agent pushes change and begins autonomous CI watch**:
   The agent uploads the patchset with Commit-Queue enabled and monitors the run:

   .. code-block:: console

      $ ./gh pr push --cq
      $ ./gh pr checks --watch --fail-fast

   ``pw_ghish`` watches Buildbucket in the background, polling at a configurable
   interval (default: 15s) without requiring human supervision.

2. **Instant fail-fast and failure triage**:
   If any blocking builder fails, ``--fail-fast`` halts immediately. With
   automatic failure diagnostics enabled (the default), ``pw_ghish`` queries
   LogDog and dumps the failing step and error log snippet straight into stdout:

   .. code-block:: console

      $ ./gh pr checks --watch --fail-fast
      ...
      ✗  docs-builder                    2m36s     https://ci.chromium.org/b/8671020269272436273
      ...
      FAILURE: docs-builder (Build 8671020269272436273)
        Failing Step: "ninja"
        Summary: Sphinx documentation build failed: undefined label 'module-pw_foo'

        --- LogDog Output Snippet: "ninja" ---
        pw_foo/docs.rst:14: WARNING: undefined label: 'module-pw_foo'

   .. tip::

      Agents can also inspect failure reports anytime with
      ``./gh run view --log-failed``, drill down into step trees with
      ``./gh run view -j <builder>``, or retry failed builders with
      ``./gh run rerun --failed``.

3. **Autonomous repair loop**:
   The agent ingests the error snippet directly in context, edits the source
   file, tests the fix locally, and pushes the updated patchset:

   .. code-block:: console

      $ bazelisk test //pw_foo/...
      $ ./gh pr push --cq
      $ ./gh pr checks --watch --fail-fast

4. **Green completion and auto-submit**:
   Once all checks pass, the agent completes the loop or enables automated
   submission:

   .. code-block:: console

      $ ./gh pr merge --auto

Key benefits
============

* **Eliminates human context switching**: Developers stay in flow on primary
  design and coding tasks while the agent drives presubmits to completion.

* **Terminal-native failure triage**: No sifting through web browser consoles or
  nested step logs—error snippets are delivered directly to the terminal.

* **Autonomous convergence**: The agent iterates on failures independently
  until all required builders are green.

------------------------------
CUJ 5: Dependent change stacks
------------------------------
Large changes are often structured as a stack of dependent commits. Gerrit
tracks each commit as an independent change using its ``Change-Id``.

However, pushing branches with multiple commits can inadvertently create
unintended changes if target branches are misconfigured.

The Workflow
============
1. **Safety Stack Guard**:
   If an agent attempts to push a branch with multiple unpushed commits,
   ``pw_ghish`` halts immediately and requires ``--stack``:

   .. code-block:: console

      $ ./gh pr create --stack

2. **Branch Memory**:
   When updating an existing change within a stack, ``pw_ghish`` discovers the
   change's recorded target branch from Gerrit (via its ``Change-Id``). The agent
   never accidentally uploads updates targeting ``main`` when the change was
   created against a feature branch.

3. **Stack Traversal**:
   Agents can checkout any change in the stack by change number or shortlink,
   apply rebased updates, and push individual patchsets cleanly:

   .. code-block:: console

      $ ./gh pr checkout 472260
      $ git rebase origin/main
      $ ./gh pr push

-------------------------------
CUJ 6: Asynchronous auto-submit
-------------------------------
Rather than polling CI checks in a loop waiting for presubmits to complete,
engineers and agents can enable automated submission once reviews are complete.

The Workflow
============
1. **Approve and enable auto-submit**:
   Once code review is satisfied, the agent or engineer enables automated
   submission:

   .. code-block:: console

      $ ./gh pr merge --auto

2. **LUCI Commit-Queue takes over**:
   ``pw_ghish`` votes ``Pigweed-Auto-Submit+1`` on Gerrit. As soon as all
   required tryjob builders pass and approvals are registered, the LUCI CV bot
   automatically rebases the commit onto ``origin/main`` and submits it.

3. **Immediate offramp on failure**:
   If submit requirements cannot be met (for instance, missing a mandatory
   ``Code-Review+2`` approval), ``pr merge`` immediately reports the missing
   labels and hints at the required voting flags rather than silently hanging.
