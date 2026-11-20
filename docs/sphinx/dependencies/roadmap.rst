.. _docs-dependencies-roadmap:

================
Roadmap & status
================
This page captures Pigweed's dependency implementation status and roadmap
(current as of **November 19, 2026**) for its
:ref:`dependency architecture <docs-build-dependency-architecture>`.

.. note::

   Full hermetic dependency management targets Bazel, Pigweed's primary build
   system. GN and CMake builds continue to use :ref:`module-pw_env_setup`,
   :ref:`module-pw_package`, and project-managed source checkouts.

--------------
Current status
--------------
Several core components of the architecture are already in production in
Pigweed:

* **Rust (crates.io)**: ``@rust_crates`` indirection via
  :cs:`pw_build/pw_rust_crates_extension.bzl` with ``std`` / ``no_std`` platform
  constraint selection, ``override_repo`` support, SHA-256 ``Cargo.lock``
  checksums, and ``cargo deny`` license and advisory checks in CI (see
  :ref:`module-pw_third_party_crates_io`).
* **Python (PyPI)**: Platform-specific, SHA-256 hash-pinned lockfiles,
  configurable ``PIGWEED_PIP_INDEX_URL`` index redirection, and hermetic local
  ``sdist`` builds using ``--no-index`` and ``--no-build-isolation``.
* **Prebuilt binaries and toolchains (CIPD)**: ``cipd``, ``pw_cxx_toolchain``,
  and ``pw_rust`` extensions supporting root-module toolchain tag overrides,
  ``dev_dependency = True`` toolchain registrations, ``override_repo`` escape
  hatches, and **6** dev-only ``cipd_dev_ext`` repositories.
* **Bzlmod hygiene**: Initial ``dev_dependency = True`` module declarations
  (``rules_doxygen``, ``rules_mypy``) and non-root consumer CI testing
  (:cs:`pw_build/bazel_tests/non_root_test`).

-------------------
Current limitations
-------------------
* **Unconditional crate.from_cargo and pip.parse evaluation**: Pigweed's
  top-level :cs:`MODULE.bazel` still invokes ``crate`` (``crate.from_cargo``
  for ``crates_std`` and ``crates_no_std``) and ``pip`` (``pip.parse`` for
  ``pigweed_python_packages``) in its non-dev module configuration. If a
  downstream or offline project uses ``@rules_rust``'s ``crate`` or
  ``@rules_python``'s ``pip`` extension, Bazel evaluates Pigweed's default
  manifests even when the resulting hub repository is overridden.
* **Non-propagating module patches**: Required portability and security patches
  in :cs:`MODULE.bazel` currently rely on root-only ``single_version_override``
  declarations, which Bazel ignores when ``@pigweed`` is consumed as a
  dependency. Downstream roots must duplicate those patches manually.
* **Direct BCR resolution and remaining dev-dependency bloat**: Pigweed still
  resolves Bazel modules directly against ``bcr.bazel.build``, and a number of
  upstream-only build rules, Python packages, crates, and CIPD tools have not
  yet been isolated behind ``dev_dependency = True``.

-------
Roadmap
-------
Pigweed is completing the
:ref:`dependency architecture <docs-build-dependency-architecture>` in two
milestones:

#. **Split MODULE.bazel into Core, Resolution, and Dev Module Files and prune
   dev dependencies**: Expose ``@rust_crates`` (with stub ``crates_std`` and
   ``crates_no_std`` hubs on ``pw_rust_crates``) and
   ``@pigweed_python_packages`` (via a ``pw_py_packages`` wrapper extension) in
   the Core Module File. Isolate Pigweed's default ``crate`` and ``pip``
   extensions under ``use_extension(..., dev_dependency = True)`` in the
   Resolution Module File (``bazel/resolve.MODULE.bazel``) wired with
   ``override_repo`` so downstream projects skip upstream manifest evaluation,
   use ``@rules_rust`` ``crate`` or ``@rules_python`` ``pip`` independently, or
   bind their own hubs uniformly with ``override_repo``. Also export
   ``Cargo.toml`` and ``Cargo.lock`` from
   ``third_party/crates_io/crates_{std,no_std}/BUILD.bazel`` (matching the
   already-exported Python lockfiles in :cs:`pw_env_setup/py/BUILD.bazel`),
   migrate ``pip`` index configuration to ``pip.default(index_url = ...)`` with
   ``envsubst`` retained on ``pip.parse``, and move upstream-only dependencies
   into the Dev Module File (``bazel/dev.MODULE.bazel``):

   * **Bzlmod**: Move **14** upstream-only modules to the Dev Module File,
     reducing Core Module File ``bazel_dep`` entries from **38 to 24** (**41**
     total), and expand non-root CI leak checks.
   * **crates.io**: Split upstream-only test, CLI, and board-example crates out
     of ``crates_std`` (**32** crates) and ``crates_no_std`` (**13** crates)
     into ``crates_std_dev`` and ``crates_no_std_dev`` inside the Dev Module
     File.
   * **PyPI**: Split documentation, lint, and test packages out of the main
     lockfile, reducing locked consumer Python packages from **130 to 56**.
   * **CIPD**: Move **4** remaining upstream-only formatter, GN, and emulator
     repositories (``qemu``, ``buildifier``, ``gn``, ``go``) from ``cipd_ext``
     to ``cipd_dev_ext`` (keeping ``bloaty`` in ``cipd_ext`` because public
     :cs:`pw_bloat/py/BUILD.bazel` depends on it), standardize SHA-256 instance
     digests with SLSA provenance, and align multi-tier root-wins tag precedence
     across CIPD and toolchain extensions.

#. **Launch the Pigweed Bazel Registry and static archive mirror**: Stand up the
   Git-reviewed PBR repository and automated CI publishing pipeline to immutable
   static storage (stripping root-only ``include()`` lines when publishing
   ``@pigweed`` itself to PBR), migrate ``single_version_override`` patches
   and third-party ``BUILD.bazel`` overlays into PBR ``source.json`` metadata,
   and switch Pigweed's ``.bazelrc`` to resolve exclusively from PBR with
   ``bcr.bazel.build`` disabled.
