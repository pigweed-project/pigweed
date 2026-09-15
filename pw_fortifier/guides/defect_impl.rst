.. _module-pw_fortifier-defect_impl:

========================================
Implementation guide for defect scanners
========================================
This guide walks step-by-step through implementing a custom security defect
scanner using :py:mod:`pw_fortifier`.

--------
Overview
--------
Building a defect scanner consists of the following steps:

1. Create an :ref:`IssueTracker
   <module-pw_fortifier-defect_impl-issue_tracker>` to access Buganizer.
2. Implement a :ref:`CodeAnalyzer
   <module-pw_fortifier-defect_impl-code_analyzer>` to scan files for defects.
3. Implement a :ref:`Critic <module-pw_fortifier-defect_impl-critic>` to
   validate findings and filter false positives.
4. Implement a :ref:`Deduplicator
   <module-pw_fortifier-defect_impl-deduplicator>` to prevent duplicate bugs.
5. Implement a :ref:`Triager <module-pw_fortifier-defect_impl-triager>` to
   assess defect severity and assign owners.
6. Implement a :ref:`DefectSummarizer
   <module-pw_fortifier-defect_impl-summarizer>` to format issue titles.
7. Implement a :ref:`PocAndFixGenerator
   <module-pw_fortifier-defect_impl-poc_and_fix_generator>` to produce fixes.
8. Wire all stages into a :ref:`DefectScanner
   <module-pw_fortifier-defect_impl-defect_scanner>` executable.
9. Define :ref:`Build targets
   <module-pw_fortifier-defect_impl-build_targets>` to run the scanner.

.. note::

   The Pigweed team maintains implementations for the pipeline stages
   described below that you can quickly adapt for your project. If you are
   trying to build scanning tools for a Google project,
   `reach out to us <http://go/pigweed-communication>`_ and we can help you get
   set up quickly!

Pipeline architecture
=====================
The diagram below shows how defect scanning pipeline stages connect. Orange
nodes represent implementer-provided components; light blue nodes represent
library-provided components.

.. mermaid::

   flowchart TD
      subgraph Discovery ["Discovery & Generation"]
         Emitter --> CodeAnalyzer
         CodeAnalyzer --> Critic
         Critic --> Deduplicator
      end

      subgraph Tracking ["Triage & Tracking"]
         IssueTracker[("IssueTracker")]
         Deduplicator --> Triager
         Triager --> IssueWriter
         IssueWriter --> IssueDemux["Issue Demux"]
         IssueReader --> IssueDemux
         Deduplicator -.- IssueTracker
         IssueWriter -.- IssueTracker
         IssueReader -.- IssueTracker
      end

      subgraph Action ["Action & Output"]
         IssueDemux --> PocAndFixGenerator
         PocAndFixGenerator --> Collector
      end

      classDef implementer fill:#ffe0b2,stroke:#f57c00,stroke-width:2px;
      classDef library fill:#e1f5fe,stroke:#0288d1,stroke-width:1px;

      class CodeAnalyzer,Critic,Deduplicator implementer;
      class Triager,IssueTracker,PocAndFixGenerator implementer;
      class Emitter,IssueWriter,IssueDemux,IssueReader,Collector library;

.. _module-pw_fortifier-defect_impl-issue_tracker:

.. include:: ../doc_resources/issue_tracker.rst

.. _module-pw_fortifier-defect_impl-code_analyzer:
.. _module-pw_fortifier-defect_impl-analyzer:

---------------
2. CodeAnalyzer
---------------
The code analyzer stage inspects individual source files for vulnerabilities and
writes findings to report files.

1. Subclass :py:class:`~pw_fortifier.code_analyzer.CodeAnalyzer`:

   .. code-block:: py

      from typing import AsyncIterator

      from pw_fortifier.async_path import AsyncPath
      from pw_fortifier.code_analyzer import CodeAnalyzer


      class MyCodeAnalyzer(CodeAnalyzer):

          def _analyze(
              self, input_file: AsyncPath, out_path: AsyncPath
          ) -> AsyncIterator[AsyncPath]:

2. In the analysis logic, invoke your agent or scanner to inspect the input
   file:

   .. code-block:: py

      async def _impl():
          findings = await my_analyzer_agent.scan(input_file)

3. Write the markdown findings to files under ``out_path`` and yield their
   paths:

   .. code-block:: py

      for idx, finding in enumerate(findings):
          report_path = out_path / f'finding-{idx}.md'
          await report_path.write_text(finding.markdown)
          yield report_path

      return _impl()

The base class automatically extracts referenced source files from the report
and constructs :py:class:`~pw_fortifier.defect.Defect` instances.

.. _module-pw_fortifier-defect_impl-critic:

---------
3. Critic
---------
The critic stage validates findings from the code analyzer to filter out false
positives before filing.

1. Subclass :py:class:`~pw_fortifier.critic.Critic`:

   .. code-block:: py

      from pw_fortifier.critic import Critic
      from pw_fortifier.defect import Defect


      class MyCritic(Critic):

2. Implement :py:meth:`~pw_fortifier.critic.Critic._criticize` to validate the
   defect description using an agent or tool:

   .. code-block:: py

      async def _criticize(self, issue: Defect) -> Defect | None:
          is_valid = await my_critic_agent.validate(issue.description)
          if not is_valid:
              return None
          return issue

Returning ``None`` drops the defect from the pipeline as a false positive.

.. _module-pw_fortifier-defect_impl-deduplicator:

---------------
4. Deduplicator
---------------
The deduplicator stage checks incoming defect reports against existing
historical issues to prevent duplicate filings.

1. Subclass :py:class:`~pw_fortifier.deduplicator.Deduplicator` and set
   ``ISSUE_TYPE = Defect``:

   .. code-block:: py

      from pw_fortifier.deduplicator import Deduplicator
      from pw_fortifier.defect import Defect
      from pw_fortifier.issue import Issue


      class MyDeduplicator(Deduplicator):
          ISSUE_TYPE = Defect

2. Implement :py:meth:`~pw_fortifier.deduplicator.Deduplicator._is_duplicate`
   using ``self.issue_tracker`` to check existing Buganizer issues:

   .. code-block:: py

      async def _is_duplicate(self, issue: Issue) -> int | None:
          assert isinstance(issue, Defect)
          duplicate_id = await my_dedup_agent.find_duplicate(
              issue, self.issue_tracker
          )
          return duplicate_id

Return the duplicate Buganizer issue ID if one exists, or ``None`` if the
finding represents a new vulnerability.

.. _module-pw_fortifier-defect_impl-triager:

----------
5. Triager
----------
The triager stage assesses defect severity.

1. Subclass :py:class:`~pw_fortifier.triager.Triager` and set
   ``ISSUE_TYPE = Defect``:

   .. code-block:: py

      from pw_fortifier.defect import Defect
      from pw_fortifier.issue import Issue
      from pw_fortifier.triager import Triager


      class MyTriager(Triager):
          ISSUE_TYPE = Defect

2. Implement :py:meth:`~pw_fortifier.triager.Triager._triage` to set the defect
   severity in place:

   .. code-block:: py

      async def _triage(self, issue: Issue) -> None:
          assert isinstance(issue, Defect)
          issue.severity = await my_triage_agent.assess_severity(issue)

Owner assignment is handled automatically by the base stage using
:py:class:`~pw_fortifier.find_core_owners.CoreOwnerFinder` if ``issue.assignee``
is not already populated.

.. _module-pw_fortifier-defect_impl-summarizer:

-------------------
6. DefectSummarizer
-------------------
The defect summarizer formats defect titles and vulnerability codes for
Buganizer issues.

1. Subclass :py:class:`~pw_fortifier.defect_tracker.DefectSummarizer`:

   .. code-block:: py

      from pw_fortifier.defect import Defect
      from pw_fortifier.defect_tracker import DefectSummarizer


      class MyDefectSummarizer(DefectSummarizer):

2. Implement :py:meth:`~pw_fortifier.defect_tracker.DefectSummarizer.summarize`
   to return a tuple of ``(vuln_code, summary)``:

   .. code-block:: py

      def summarize(self, defect: Defect) -> tuple[str, str]:
          vuln_code = 'UAF'
          summary = 'Use-after-free in buffer handling'
          return (vuln_code, summary)

.. _module-pw_fortifier-defect_impl-poc_and_fix_generator:

---------------------
7. PocAndFixGenerator
---------------------
The code editor stage generates a proof-of-concept unit test and a code fix for
the defect.

1. Subclass :py:class:`~pw_fortifier.poc_and_fix_generator.PocAndFixGenerator`:

   .. code-block:: py

      from pw_fortifier.defect import Defect
      from pw_fortifier.poc_and_fix_generator import PocAndFixGenerator


      class MyPocAndFixGenerator(PocAndFixGenerator):

          def __init__(self) -> None:
              super().__init__('my_poc_and_fix_generator')

2. Implement
   :py:meth:`~pw_fortifier.poc_and_fix_generator.PocAndFixGenerator._generate_poc`
   to create a failing test:

   .. code-block:: py

      async def _generate_poc(self, defect: Defect) -> str | None:
          test_target = await my_fix_agent.create_poc(defect)
          return test_target

3. Implement
   :py:meth:`~pw_fortifier.poc_and_fix_generator.PocAndFixGenerator._generate_fix`
   to apply the code fix:

   .. code-block:: py

      async def _generate_fix(self, defect: Defect) -> bool:
          fix_ok = await my_fix_agent.create_fix(defect)
          return fix_ok

4. Implement
   :py:meth:`~pw_fortifier.poc_and_fix_generator.PocAndFixGenerator._summarize`
   to construct the Git commit message:

   .. code-block:: py

      async def _summarize(
          self, defect: Defect, diffs: list[str]
      ) -> tuple[str, str]:
          subject = f'Fix security defect in {defect.location.file}'
          body = 'Applies fix and adds regression unit test.'
          return (subject, body)

.. _module-pw_fortifier-defect_impl-defect_scanner:

----------------
8. DefectScanner
----------------
Assemble all stages into an executable scanner tool by subclassing
:py:class:`~pw_fortifier.defect_scanner.DefectScanner`.

1. Create a script (e.g. ``my_defect_scanner.py``) and define the scanner
   skeleton:

   .. code-block:: py

      import asyncio
      import sys

      from pw_fortifier.defect_scanner import DefectScanner


      class MyDefectScanner(DefectScanner):

          def __init__(self) -> None:
              super().__init__('my_defect_scanner')


      async def main() -> None:
          scanner = MyDefectScanner()
          await scanner.run(*sys.argv[1:])


      if __name__ == '__main__':
          asyncio.run(main())

2. Set the repository URL to scan:

   .. code-block:: py

      self.repo_url = 'sso://repo-host/my-project'

3. Wire up the code analyzer and critic:

   .. code-block:: py

      self.code_analyzer = MyCodeAnalyzer()
      self.critic = MyCritic()

4. Wire up the summarizer and issue tracker:

   .. code-block:: py

      self.summarizer = MyDefectSummarizer()
      self.issue_tracker = MyIssueTracker()

5. Wire up the deduplicator, triager, and code generator:

   .. code-block:: py

      self.deduplicator = MyDeduplicator()
      self.triager = MyTriager()
      self.code_generator = MyPocAndFixGenerator()

.. _module-pw_fortifier-defect_impl-build_targets:

----------------
9. Build targets
----------------
Define a :cs:`pw_py_binary <pw_build/python.bzl>` target in ``BUILD.bazel`` to
run your scanner:

1. Load ``pw_py_binary``:

   .. code-block:: bazel

      load("//pw_build:python.bzl", "pw_py_binary")

2. Define the binary target and include ``//pw_fortifier/py:pw_fortifier`` in
   ``deps``:

   .. code-block:: bazel

      pw_py_binary(
          name = "my_defect_scanner",
          srcs = [
              "my_defect_scanner.py",
          ],
          deps = [
              "//pw_fortifier/py:pw_fortifier",
              ":my_security_stages_lib",
              ":my_issue_tracker_lib",
          ],
      )

-------
Summary
-------
Implementation checklist:

1. **IssueTracker**

   - [ ] Subclass ``IssueTracker``.
   - [ ] Set ``component_id``, ``primary_hotlist_id``, and ``default_assignee``.
   - [ ] (Optional) Set ``extra_hotlist_ids`` and ``ccs``.
   - [ ] Implement ``create()`` to file bugs in Buganizer.
   - [ ] Implement ``read()`` to query bugs by ID.
   - [ ] Implement ``read_hotlist()`` to stream bugs by hotlist.

2. **CodeAnalyzer**

   - [ ] Subclass ``CodeAnalyzer``.
   - [ ] Implement ``_analyze()`` to inspect files and yield markdown reports.

3. **Critic**

   - [ ] Subclass ``Critic``.
   - [ ] Implement ``_criticize()`` to filter out false positives.

4. **Deduplicator**

   - [ ] Subclass ``Deduplicator`` with ``ISSUE_TYPE = Defect``.
   - [ ] Implement ``_is_duplicate()`` using ``self.issue_tracker``.

5. **Triager**

   - [ ] Subclass ``Triager`` with ``ISSUE_TYPE = Defect``.
   - [ ] Implement ``_triage()`` to set defect severity.

6. **DefectSummarizer**

   - [ ] Subclass ``DefectSummarizer``.
   - [ ] Implement ``summarize()`` returning vulnerability code and summary.

7. **PocAndFixGenerator**

   - [ ] Subclass ``PocAndFixGenerator``.
   - [ ] Implement ``_generate_poc()`` to create a failing unit test.
   - [ ] Implement ``_generate_fix()`` to resolve the defect.
   - [ ] Implement ``_summarize()`` to generate commit messages.

8. **DefectScanner**

   - [ ] Subclass ``DefectScanner`` and call ``super().__init__()``.
   - [ ] Set ``repo_url``.
   - [ ] Wire up all pipeline stages in ``__init__``.

9. **Build targets**

   - [ ] Define ``pw_py_binary`` in ``BUILD.bazel``.
   - [ ] Add ``//pw_fortifier/py:pw_fortifier`` to ``deps``.

---------------------
Testing and debugging
---------------------
Run the scanner with:

.. code-block:: console

   # Local dry run across the repository (no bugs filed, no CLs uploaded)
   $ bazelisk run //path/to:my_defect_scanner -- -s /path/to/my/project

   # Scan specific files and output a CSV report
   $ bazelisk run //path/to:my_defect_scanner -- \
       -s /path/to/my/project -f "pw_sync/**" -o defects.csv

   # Run a full scan, filing bugs and uploading candidate fix CLs
   $ bazelisk run //path/to:my_defect_scanner -- \
       -s /path/to/my/project -b -u

.. include:: ../doc_resources/testing_tips.rst
