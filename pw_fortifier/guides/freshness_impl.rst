.. _module-pw_fortifier-freshness_impl:

===========================================
Implementation guide for freshness scanners
===========================================
This guide walks step-by-step through implementing a custom freshness scanner
using :py:mod:`pw_fortifier`.

--------
Overview
--------
Building a freshness scanner consists of the following steps:

1. Create an :ref:`IssueTracker
   <module-pw_fortifier-freshness_impl-issue_tracker>` to access Buganizer.
2. Set up the :ref:`FreshnessScanner
   <module-pw_fortifier-freshness_impl-scanner>` and register package
   analyzers.
3. (Optional) Implement custom :ref:`PackageAnalyzers
   <module-pw_fortifier-freshness_impl-package_analyzers>` for unsupported
   package formats.
4. (Optional) Implement custom :ref:`PackageUpdaters
   <module-pw_fortifier-freshness_impl-package_updaters>` for custom roll
   tooling.
5. Define :ref:`Build targets
   <module-pw_fortifier-freshness_impl-build_targets>` to run the scanner.

Pipeline architecture
=====================
The diagram below shows how pipeline stages connect. Orange nodes represent
implementer-provided components; light blue nodes represent library-provided
components.

.. mermaid::

   flowchart TD
      subgraph Discovery ["Discovery & Generation"]
         Emitter --> AnalyzerMux["AnalyzerMux"]
         AnalyzerMux --> BazelCipd["BazelCipd"]
         AnalyzerMux --> BazelDep["BazelDep"]
         AnalyzerMux --> BazelMaven["BazelMaven"]
         AnalyzerMux --> CargoAnalyzer["Cargo"]
         AnalyzerMux --> CipdSetup["CipdSetup"]
         AnalyzerMux --> Copybara["Copybara"]
         AnalyzerMux --> GoModAnalyzer["GoMod"]
         AnalyzerMux --> NpmAnalyzer["Npm"]
         AnalyzerMux --> PipAnalyzer["Pip"]
         AnalyzerMux --> CustomAnalyzer["Custom"]
         BazelCipd --> AnalyzerDemux["AnalyzerDemux"]
         BazelDep --> AnalyzerDemux
         BazelMaven --> AnalyzerDemux
         CargoAnalyzer --> AnalyzerDemux
         CipdSetup --> AnalyzerDemux
         Copybara --> AnalyzerDemux
         GoModAnalyzer --> AnalyzerDemux
         NpmAnalyzer --> AnalyzerDemux
         PipAnalyzer --> AnalyzerDemux
         CustomAnalyzer --> AnalyzerDemux
         AnalyzerDemux --> Deduplicator
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
         IssueDemux --> RollGenerator
         subgraph Updaters ["PackageUpdaters"]
            CargoUpdater["Cargo"]
            GoModUpdater["GoMod"]
            NpmUpdater["Npm"]
            PipUpdater["Pip"]
            CustomUpdater["Custom"]
         end
         RollGenerator -.- Updaters
         RollGenerator --> Collector
      end

      classDef implementer fill:#ffe0b2,stroke:#f57c00,stroke-width:2px;
      classDef library fill:#e1f5fe,stroke:#0288d1,stroke-width:1px;

      class IssueTracker,CustomAnalyzer,CustomUpdater implementer;
      class Emitter,AnalyzerMux,BazelCipd,BazelDep,BazelMaven library;
      class CargoAnalyzer,CipdSetup,Copybara,GoModAnalyzer library;
      class NpmAnalyzer,PipAnalyzer,AnalyzerDemux,Deduplicator library;
      class Triager,IssueWriter,IssueDemux,IssueReader library;
      class RollGenerator,Collector,CargoUpdater library;
      class GoModUpdater,NpmUpdater,PipUpdater library;

.. _module-pw_fortifier-freshness_impl-issue_tracker:

.. include:: ../doc_resources/issue_tracker.rst

.. note::

   The Pigweed team maintains an internal ``IssueTracker`` implementation using
   Google Buganizer APIs. Googlers can contact Pigweed at
   `go/pigweed-communication <http://go/pigweed-communication>`_ for help
   adapting it to their projects.

.. _module-pw_fortifier-freshness_impl-scanner:
.. _module-pw_fortifier-freshness_impl-freshness_scanner:

-------------------
2. FreshnessScanner
-------------------
Next, set up the scanner executable by subclassing
:py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner`.

1. Create a script (e.g. ``my_freshness_scanner.py``) and define the scanner
   skeleton:

   .. code-block:: py

      import asyncio
      import sys

      from pw_fortifier.freshness_scanner import FreshnessScanner


      class MyFreshnessScanner(FreshnessScanner):

          def __init__(self) -> None:
              super().__init__('my_freshness_scanner')


      async def main() -> None:
          scanner = MyFreshnessScanner()
          await scanner.run(*sys.argv[1:])


      if __name__ == '__main__':
          asyncio.run(main())

2. Set the repository URL to scan:

   .. code-block:: py

      self.repo_url = 'sso://repo-host/my-project'

3. Instantiate the ``IssueTracker`` defined in Step 1:

   .. code-block:: py

      from my_issue_tracker import MyIssueTracker

      # In MyFreshnessScanner.__init__:
      self.issue_tracker = MyIssueTracker()

4. Import and register a provided package analyzer via
   :py:meth:`~pw_fortifier.freshness_scanner.FreshnessScanner.register`:

   .. code-block:: py

      from pw_fortifier.bazel_dep import BazelDepAnalyzer

      # In MyFreshnessScanner.__init__:
      self.register(BazelDepAnalyzer())

5. Repeat for each package type in your repository. For analyzers with companion
   updaters, pass the updater as well:

   .. code-block:: py

      from pw_fortifier.cargo import CargoAnalyzer, CargoUpdater
      from pw_fortifier.npm import NpmAnalyzer, NpmUpdater

      # In MyFreshnessScanner.__init__:
      self.register(CargoAnalyzer(), CargoUpdater())
      self.register(NpmAnalyzer(), NpmUpdater())

For a complete working example, see
:cs:`pw_fortifier/py/demo_freshness_scanner.py`.

Available package analyzers
===========================
The following package analyzers are provided by ``pw_fortifier``:

* :py:class:`~pw_fortifier.bazel_cipd.BazelCipdAnalyzer`: CIPD packages in
  ``MODULE.bazel``.
* :py:class:`~pw_fortifier.bazel_dep.BazelDepAnalyzer`: BCR dependencies in
  ``MODULE.bazel``.
* :py:class:`~pw_fortifier.bazel_maven.BazelMavenAnalyzer`: Maven artifacts in
  ``MODULE.bazel``.
* :py:class:`~pw_fortifier.cargo.CargoAnalyzer`: Rust crates in ``Cargo.toml``
  and ``Cargo.lock``. (Pairs with
  :py:class:`~pw_fortifier.cargo.CargoUpdater`).
* :py:class:`~pw_fortifier.cipd_setup.CipdSetupAnalyzer`: CIPD JSON manifests in
  ``pw_env_setup``.
* :py:class:`~pw_fortifier.copybara.CopybaraAnalyzer`: Third-party repositories
  in ``copy.bara.sky``.
* :py:class:`~pw_fortifier.go_mod.GoModAnalyzer`: Go modules in ``go.mod``.
  (Pairs with :py:class:`~pw_fortifier.go_mod.GoModUpdater`).
* :py:class:`~pw_fortifier.npm.NpmAnalyzer`: Node packages in ``package.json``
  and ``package-lock.json``. (Pairs with
  :py:class:`~pw_fortifier.npm.NpmUpdater`).
* :py:class:`~pw_fortifier.pip.PipAnalyzer`: Python requirements in
  ``setup.cfg``. (Pairs with :py:class:`~pw_fortifier.pip.PipUpdater`).

.. _module-pw_fortifier-freshness_impl-package_analyzers:

--------------------------
3. Custom PackageAnalyzers
--------------------------
To scan dependency types not covered by provided analyzers, implement a custom
:py:class:`~pw_fortifier.package_analyzer.PackageAnalyzer`.

1. Subclass :py:class:`~pw_fortifier.package_analyzer.PackageAnalyzer` and set
   :py:attr:`~pw_fortifier.package_analyzer.PackageAnalyzer.PKG_TYPE`:

   .. code-block:: py

      from pw_fortifier.package_analyzer import PackageAnalyzer


      class MyCustomAnalyzer(PackageAnalyzer):
          PKG_TYPE = 'custom_pkg'

2. Configure target file discovery:

   * For distributed files matching a specific name across the repository, set
     :py:attr:`~pw_fortifier.package_analyzer.PackageAnalyzer.TARGET` and
     implement
     :py:meth:`~pw_fortifier.pipeline_stage.PipelineStage._process_one`:

     .. code-block:: py

        TARGET = 'dependencies.json'

        async def _process_one(self, path: AsyncPath) -> None:
            # Parse file at path...

   * For a single fixed-location file (e.g. root workspace configuration),
     override :py:meth:`~pw_fortifier.pipeline_stage.PipelineStage._set_up`
     to read the file directly and enqueue results.

3. In the parsing logic, query available versions from your registry and call
   :py:meth:`~pw_fortifier.package_analyzer.PackageAnalyzer.find_lowest` to find
   the earliest valid version:

   .. code-block:: py

      earliest = self.find_lowest(
          TIER2_DEVHOST, current_version, available_versions
      )

4. Construct and emit a
   :py:class:`~pw_fortifier.freshness_result.FreshnessResult` using
   :py:meth:`~pw_fortifier.package_analyzer.PackageAnalyzer._send_result`:

   .. code-block:: py

      if earliest and earliest.version != current_ver:
          result = FreshnessResult(
              package=pkg_name,
              location=f'{rel_path}:{line}',
              pkg_type=self.PKG_TYPE,
              current=current_version,
              earliest=earliest,
              tier=TIER2_DEVHOST,
          )
          await self._send_result(result)

5. Register your custom analyzer in your scanner via
   :py:meth:`~pw_fortifier.freshness_scanner.FreshnessScanner.register`:

   .. code-block:: py

      from my_custom_analyzer import MyCustomAnalyzer

      # In MyFreshnessScanner.__init__:
      self.register(MyCustomAnalyzer())

.. _module-pw_fortifier-freshness_impl-package_updaters:

-------------------------
4. Custom PackageUpdaters
-------------------------
By default,
:py:class:`~pw_fortifier.roll_generator.RollGenerator` attempts rolls by text
replacement of version strings. For package types that require tool
invocations (e.g. ``cargo update`` or lockfile generation), implement a custom
:py:class:`~pw_fortifier.package_updater.PackageUpdater`.

1. Subclass :py:class:`~pw_fortifier.package_updater.PackageUpdater` and set
   :py:attr:`~pw_fortifier.package_updater.PackageUpdater.PKG_TYPE` or
   :py:attr:`~pw_fortifier.package_updater.PackageUpdater.TARGET`:

   .. code-block:: py

      from pw_fortifier.package_updater import PackageUpdater


      class MyCustomUpdater(PackageUpdater):
          PKG_TYPE = 'custom_pkg'
          TARGET = 'dependencies.json'

2. Implement :py:meth:`~pw_fortifier.package_updater.PackageUpdater.update` to
   apply changes in the workspace and verify the build:

   .. code-block:: py

      async def update(
          self,
          dst_repo: WritableGitWorkspace,
          result: FreshnessResult,
      ) -> bool:
          # Modify files in dst_repo using relevant tools
          # Run build and presubmit validation
          return True

3. Register the updater alongside your analyzer via
   :py:meth:`~pw_fortifier.freshness_scanner.FreshnessScanner.register`:

   .. code-block:: py

      from my_custom_analyzer import MyCustomAnalyzer
      from my_custom_updater import MyCustomUpdater

      # In MyFreshnessScanner.__init__:
      self.register(MyCustomAnalyzer(), MyCustomUpdater())

.. _module-pw_fortifier-freshness_impl-build_targets:

----------------
5. Build targets
----------------
Define a :cs:`pw_py_binary <pw_build/python.bzl>` target in ``BUILD.bazel`` to
run your scanner:

1. Load ``pw_py_binary``:

   .. code-block:: bazel

      load("//pw_build:python.bzl", "pw_py_binary")

2. Define the binary target and include ``pw_fortifier`` in ``deps``:

   .. code-block:: bazel

      pw_py_binary(
          name = "my_freshness_scanner",
          srcs = [
              "my_freshness_scanner.py",
          ],
          deps = [
              "@pigweed//pw_fortifier/py:pw_fortifier",
              ":my_custom_analyzer_lib",
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

2. **FreshnessScanner**

   - [ ] Subclass ``FreshnessScanner`` and call ``super().__init__()``.
   - [ ] Set ``repo_url``.
   - [ ] Instantiate and set ``self.issue_tracker``.
   - [ ] Register desired package analyzers (and optional updaters).

3. **Custom PackageAnalyzers (optional)**

   - [ ] Subclass ``PackageAnalyzer`` and set ``PKG_TYPE``.
   - [ ] Set ``TARGET`` and implement ``_process_one()`` (or ``_set_up()``).
   - [ ] Query available versions and call ``find_lowest()``.
   - [ ] Emit findings via ``_send_result()``.
   - [ ] Register with ``self.register()``.

4. **Custom PackageUpdaters (optional)**

   - [ ] Subclass ``PackageUpdater`` and set ``PKG_TYPE`` or ``TARGET``.
   - [ ] Implement ``update()`` and return success boolean.
   - [ ] Register via ``self.register(analyzer, updater)``.

5. **Build targets**

   - [ ] Define ``pw_py_binary`` in ``BUILD.bazel``.
   - [ ] Add ``//pw_fortifier/py:pw_fortifier`` to ``deps``.

---------------------
Testing and debugging
---------------------
Run the scanner with:

.. code-block:: console

   # Local dry run across the repository (no bugs filed, no CLs uploaded)
   $ bazelisk run //path/to:my_freshness_scanner -- -s /path/to/my/project

   # Scan specific files and output a CSV report
   $ bazelisk run //path/to:my_freshness_scanner -- \
       -s /path/to/my/project \
       -f "**/MODULE.bazel" "**/Cargo.toml" -o freshness.csv

   # Run a full scan, filing bugs and uploading roll CLs
   $ bazelisk run //path/to:my_freshness_scanner -- \
       -s /path/to/my/project -b -u

The Scanner framework includes a few features to assist implementers in testing
and debugging pipeline stages.

.. include:: ../doc_resources/testing_tips.rst
