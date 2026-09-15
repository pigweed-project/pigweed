.. _module-pw_fortifier-design:

======
Design
======
``pw_fortifier`` is designed to provide libraries that can be used to build
agentic scanning tools. These libraries are built on a common framework that
implements the scanner as a pipeline of individual stages.

-----
Goals
-----
The overall goal of building an agentic scanning framework results in some
immediate sub-goals for the design:

- **Flexibility**: The space of agentic tooling is evolving rapidly. The best
  agents, skills, and models for a particular task may be supplanted by new ones
  in the coming days, weeks, and months. As a result, it is strongly undesirable
  to tie the scanner framework to any specific approach. Instead, the pipeline
  design seeks to identify the sequence of steps required to perform a scan
  regardless of implementation, and provide clear injection points where tool
  owners can add or swap agents, skills, and models as needed.
- **Performance**: Scanning an entire codebase can be arduous. Strictly
  sequencing calls to various agents and subprocesses can lead to bottlenecks
  that make frequent scanning infeasible. The pipeline design heavily relies on
  an asynchronous execution model to ensure stages that can make progress are
  not blocked by those that are waiting on agent or subprocess responses.
- **Extensibility**: While these tools were originally envisioned for upstream
  Pigweed, it is clear they may have value to downstream consumers as well. To
  maximize this value, the pipeline design makes as few assumptions about
  project details as possible, and provides specific interfaces to add
  functionality to stages like the :ref:`module-pw_fortifier-design-analyzer`
  or :ref:`code editors <module-pw_fortifier-design-code_editor>` to handle
  cases beyond those encountered in upstream.
- **Reusability**: Finally, it is desirable to have a common mechanism for
  multiple use cases. Upstream Pigweed in particular has two identified types of
  issues they wish to scan for: security defects and out-of-date third-party
  packages. The pipeline design allows the underlying framework to be reused for
  each of these workflows.

-----------------
High-level design
-----------------
The :py:class:`~pw_fortifier.scanner.Scanner` class orchestrates an
asynchronous, multi-stage data processing pipeline.
:py:class:`~pw_fortifier.pipeline_stage.PipelineStage`\ s are asynchronous tasks
that run in a loop, and may be producers, consumers, or both.

Each :py:class:`consumer <pw_fortifier.pipeline_stage.PipelineConsumerMixin>`
reads from an ``asyncio.Queue`` and processes the input. Consumers are resilient
against transient errors like network timeouts, and will automatically retry
processing (up to a configured
:ref:`maximum <module-pw_fortifier-scanner_cli-max_retries>`). They routinely
save their intermediate state within the
:ref:`working directory <module-pw_fortifier-scanner_cli-working_dir>`, allowing
interrupted runs to be
:ref:`resumed <module-pw_fortifier-scanner_cli-resume>`.

Each :py:class:`producer <pw_fortifier.pipeline_stage.PipelineProducerMixin>`
writes to the input queue of the next stage. When there are no more outputs to
be produced, producers send a sentinel value that consumers recognize as
indicating the queue is closed.

Many stages are both consumers and producers. They read inputs, process and
transform them, and then send them on to the next stage. On receiving a closure
sentinel, they send the sentinel to the next stage and stop running.

Finally, the module also includes the
:py:class:`~pw_fortifier.pipeline_stage.PipelineMux` and
:py:class:`~pw_fortifier.pipeline_stage.PipelineDemux` classes. These allow
"fanning out" based on input name, and then "fanning in" again back to a single
next stage.

.. _module-pw_fortifier-design-architecture:

Pipeline architecture
=====================
The pipeline coordinates discovering, analyzing, triaging, filing, and patching
issues. The diagram below illustrates the flow of data through the standard
pipeline stages:

.. mermaid::

   flowchart TD
      subgraph "Discovery & Generation"
         A[Emitter] --> B[Analyzer Stages]
         B --> C[Deduplicator]
      end
      subgraph "Triage & Tracking"
         C --> D[Triager]
         D --> E[IssueWriter]
         E --> F[Issue Demux]
         G[IssueReader] --> F
      end
      subgraph "Action & Output"
         F --> H[Editor]
         H --> I[Collector]
      end

---------------
Pipeline stages
---------------
The :py:class:`~pw_fortifier.scanner.Scanner` pipeline consists of the following
common stages:

.. _module-pw_fortifier-design-emitter:

Emitter
=======
The :py:class:`~pw_fortifier.emitter.Emitter` stage is the entry point when
scanning files. It uses a :py:class:`PathEnumerator` to walk the
:ref:`read-only repository <module-pw_fortifier-scanner_cli-src_repo>` and
discover files matching registered glob patterns, emitting file paths to
downstream stages.

Alternatively, when specific
:ref:`files <module-pw_fortifier-scanner_cli-files>` are passed via the command
line, only those matching files are emitted.

Finally, if specific :ref:`issues <module-pw_fortifier-scanner_cli-issues>` or
:ref:`hotlists <module-pw_fortifier-scanner_cli-hotlists>` are provided via the
command line, the ``Emitter`` is disabled entirely. The provided issues will be
passed by the :ref:`module-pw_fortifier-design-issue_reader` stage instead.

.. _module-pw_fortifier-design-analyzer:

Analyzer
========
This is a collection of stages that transforms paths to files into specific
findings. The number of stages and how they are related to one another are
specific to the tool implementation.

Code analysis for DefectScanner
-------------------------------
For example, the :py:class:`~pw_fortifier.defect_scanner.DefectScanner` class
:ref:`requires <module-pw_fortifier-defect_impl>` two types be
provided for code analysis:

- A :py:class:`~pw_fortifier.code_analyzer.CodeAnalyzer` implements an agentic
  analysis of security defects in the source referenced by a given path.
- A :py:class:`~pw_fortifier.critic.Critic` invokes an agent to challenge the
  previous step's findings and validate they are legitimate defects.

Package analysis for FreshnessScanner
-------------------------------------
As another example, the
:py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner` class uses a
:py:class:`~pw_fortifier.pipeline_stage.PipelineMux` and
:py:class:`~pw_fortifier.pipeline_stage.PipelineDemux` to send paths to
:py:class:`~pw_fortifier.package_analyzer.PackageAnalyzer`\ s that have been
:ref:`registered <module-pw_fortifier-freshness_impl>` with the
scanner.

.. _module-pw_fortifier-design-deduplicator:

Deduplicator
============
The :py:class:`pw_fortifier.deduplicator.Deduplicator` stage filters out
duplicate findings by cross-referencing candidate items against historical
issues and existing bug tracker state.

The implementation of this type is correlated with the
:ref:`module-pw_fortifier-design-issue_writer` stage. Depending on how
predictable the output of the latter is, the implementation of this stage may
range from completely deterministic to fully agentic.

.. _module-pw_fortifier-design-triager:

Triager
=======
The :py:class:`~pw_fortifier.triager.Triager` class determines the severity of
an issue, and uses
:ref:`CoreOwnerFinder <module-pw_fortifier-other_utils-find_core_owners-lib>` to
select an assignee.

Scanner-specific implementations will produce scanner-specific issues. For
example, :py:class:`~pw_fortifier.defect_scanner.DefectScanner` produces
:py:class:`~pw_fortifier.defect.Defect`\ s, while
:py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner` produces
:py:class:`~pw_fortifier.freshness_result.FreshnessResult`\ s.

.. _module-pw_fortifier-design-issue_writer:

IssueWriter
===========
The :py:class:`pw_fortifier.issue_tracker.IssueWriter` class is used to create
Buganizer issues. When
:ref:`-b / --create-bugs <module-pw_fortifier-scanner_cli-create_bugs>` is
enabled, it creates new issues via an
:py:class:`pw_fortifier.issue_tracker.IssueTracker` implementation that handles
details such as component IDs, default hotlists, and specific Buganizer APIs.

When :ref:`-v / --verbose <module-pw_fortifier-scanner_cli-verbose>` is enabled,
it prints issue titles and descriptions to stdout.

.. _module-pw_fortifier-design-issue_reader:

IssueReader
===========
In normal bulk-scanning mode, the :ref:`module-pw_fortifier-design-emitter` puts
paths into the pipeline to be processed by the preceding stages. However, the
user can instead specify :ref:`issues <module-pw_fortifier-scanner_cli-issues>`
or :ref:`hotlists <module-pw_fortifier-scanner_cli-hotlists>` to bypass those
stages. In this case, the :py:class:`~pw_fortifier.issue_tracker.IssueReader`
fetches existing issues directly from the issue tracker and injects them into
the pipeline directly upstream of the code editor. This facilitates creating CLs
for existing issues.

.. _module-pw_fortifier-design-code_editor:

Code Editor
===========
Much like the :ref:`module-pw_fortifier-design-analyzer` stages, the code editor
stage implementation varies between tools while sharing a single purpose: to
create Gerrit CLs that resolve the discovered issues.

Unlike the ``Analyzer`` stages, all code changes are handled by a single stage.
This stage is also the only one that interacts with the
:ref:`writable repository <module-pw_fortifier-scanner_cli-dst_repo>`. This
avoids any concurrent reads or writes of files when the code editor is making
changes.

In the absence of
:ref:`--allow-uploads <module-pw_fortifier-scanner_cli-allow_uploads>`, the code
editors will not publish their CLs to Gerrit and preserve them locally.

PoC and fix generator for DefectScanner
---------------------------------------
The code editor for the :py:class:`~pw_fortifier.defect_scanner.DefectScanner`
is an implementation of the
:py:class:`~pw_fortifier.poc_and_fix_generator.PocAndFixGenerator`. As the name
implies, this code editor attempts to make two sets of changes:

- First, it attempts to add a unit test that acts as a "proof of concept" (PoC)
  for the security defect. A valid PoC fails the test if the defect could be
  exploited.
- Next, it attempts to fix the vulnerability. If a valid PoC was created, that
  test should pass with the fix applied.

This code editor will create CLs containing the PoCs and fixes for _each_ issue
and publish them to the Gerrit instance associated with the repository
(when :ref:`--allow-uploads <module-pw_fortifier-scanner_cli-allow_uploads>` is
enabled).

Roll generator for FreshnessScanner
-----------------------------------
The code editor for the
:py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner` is the
:py:class:`~pw_fortifier.roll_generator.RollGenerator` class. This class
has a default behavior of simply trying to revise the version of a third-party
package and verifying that upstream Pigweed still builds, passes its unit tests,
and passes its presubmit checks. It also allows for registering additional
:py:class:`~pw_fortifier.package_updater.PackageUpdater` instances that handle
editing code and verifying builds for specific packages or files.

This code editor will create a _single_ CL containing all the rolls that were
successfully applied and publish it to the Gerrit instance associated with the
repository (when
:ref:`--allow-uploads <module-pw_fortifier-scanner_cli-allow_uploads>` is
enabled).

.. _module-pw_fortifier-design-collector:

Collector
=========
The final :py:class:`~pw_fortifier.collector.Collector` stage is a "sink" that
collects processed items, prints formatted console summaries, and optionally
writes structured CSV reports to an
:ref:`output <module-pw_fortifier-scanner_cli-output>` file.
