.. _docs-build-dependency-architecture:

=======================
Dependency architecture
=======================
This document describes Pigweed's architecture for managing external
dependencies in Bazel across **Bzlmod**, **crates.io**, **PyPI**, and **CIPD**,
covering the build mechanisms and operational workflows for upstream Pigweed
developers, downstream product developers, and downstream platform builders.

.. note::

   This architecture is actively rolling out: components are written in the
   present tense to describe Pigweed's end state rather than what is
   implemented at ``HEAD`` today. See :ref:`docs-dependencies-roadmap` for
   current status. For the underlying motivation and design principles, see
   :ref:`docs-dependencies`. For introductory Bzlmod concepts, see
   :ref:`docs-bazel-dependencies`.

---------------------
Architecture overview
---------------------
Pigweed's Bazel configuration has to support two distinct workflows from one
repository. Contributors working inside Pigweed itself expect tests, linters,
documentation builders, and working lockfiles out of the box. Downstream product
developers and platform builders, by contrast, consume Pigweed as an external
dependency and need full authority over resolved versions, internal mirrors, and
toolchains without forking Pigweed's build files.

To serve both groups together, Pigweed builds its Bazel dependency architecture
around three ideas:

* **A curated Bazel registry (Pigweed Bazel Registry, PBR)**: Instead of
  pulling Bazel modules directly from the public Bazel Central Registry
  (``bcr.bazel.build``), builds resolve modules through a curated Pigweed
  registry hosting vetted versions, SHA-256-verified source archives, and shared
  patches.
* **Three role-based module files**: Rather than combining every dependency into
  one monolithic ``MODULE.bazel`` file, Pigweed separates its definitions into
  three named files joined unconditionally at Pigweed's root via Bazel's
  ``include()`` directive (which release packaging strips when publishing to
  PBR): the **Core Module File** (downstream consumer requirements), the
  **Resolution Module File** (Pigweed's root-only Cargo and Python lockfile
  bindings), and the **Dev Module File** (upstream-only lint, docs, and test
  tools). Downstream builds using PBR evaluate only the Core Module File (and
  non-registry overrides ignore the ``dev_dependency = True`` tags and
  overrides in the other two files), while Pigweed contributors evaluate all
  three.
* **Overridable language and toolchain repositories**: Pigweed build definitions
  reference shared logical repository names rather than hardcoding upstream
  lockfiles or URLs. Downstream roots can supply Pigweed's exported lockfiles,
  redirect Python fetches in ``.bazelrc``, or swap in their own repositories
  with ``override_repo`` (or register custom toolchains).

.. mermaid::

   flowchart TD
       Up["Upstream Pigweed Root"]
       Down["Downstream Root<br/>(Platform or Product)"]

       subgraph PW["@pigweed"]
           direction LR
           Dev["Dev Module File<br/>bazel/dev.MODULE.bazel<br/>Upstream-only tools"]
           Res["Resolution Module File<br/>bazel/resolve.MODULE.bazel<br/>Default Cargo & pip bindings"]
           Mod["Core Module File<br/>MODULE.bazel<br/>Consumer bazel_dep & extensions"]
       end

       subgraph Eco["External Package Sources"]
           direction LR
           Bzl["Bzlmod<br/>Pigweed Bazel Registry (PBR)<br/>or local --registry"]
           Crates["crates.io (@rust_crates)<br/>pw_rust_crates +<br/>override_repo"]
           PyPI["PyPI (@pigweed_python_packages)<br/>PIGWEED_PIP_INDEX_URL or<br/>pw_py_packages + override_repo"]
           CIPD["CIPD (pw_cxx_toolchain, pw_rust, cipd)<br/>SHA-256 packages, custom<br/>cc_toolchain, or override_repo"]
       end

       Up -->|"activates"| Dev
       Up -->|"activates"| Res
       Up -->|"root module"| Mod
       Down -->|"bazel_dep(name = 'pigweed')<br/>ignores Dev & Resolution"| Mod
       Mod --> Bzl
       Mod --> Crates
       Mod --> PyPI
       Mod --> CIPD

----------------------
Ecosystem architecture
----------------------
Bazel modules (Bzlmod) and Pigweed Bazel Registry
=================================================
Pigweed Bazel Registry (PBR) and archive mirror
-----------------------------------------------
Pigweed continues to upstream module definitions and fixes to the public Bazel
Central Registry (BCR) where feasible. Even with active BCR upstreaming,
resolving directly from ``bcr.bazel.build`` leaves gaps for embedded firmware
projects, so Pigweed builds resolve exclusively from the **Pigweed Bazel
Registry (PBR)**:

* **Propagated patches and embedded-only packages**: Bazel ignores
  ``single_version_override`` in non-root modules. Hosting in-flight fixes,
  embedded workarounds, and ``BUILD.bazel`` / ``MODULE.bazel`` overlays for
  non-BCR vendor SDKs and C/C++ libraries directly in PBR ``source.json``
  metadata lets every downstream consumer inherit them automatically while
  upstream changes are reviewed—or where upstream BCR lacks a maintainer.
* **Single static mirror instead of scattered release URLs**: Public BCR holds
  metadata while pointing across dozens of external download hosts. PBR pairs
  Git-reviewed metadata with deterministically packaged tarballs on one
  immutable static mirror, verified by SHA-256 ``integrity`` hashes—shrinking
  network allowlists, surviving upstream outages, and simplifying offline
  mirrors.
* **Vetted boundary, retention, and quarantine**: Disabling ``bcr.bazel.build``
  (for example, ``common --registry=https://<mirror>/pigweed-registry`` in
  ``.bazelrc``) prevents silent fallback to unvetted transitive modules or
  versions while preserving MVS checks across the curated graph. Published
  versions stay available for fixed release branches and are removed only for
  emergency security revocations. Paired with
  ``common --incompatible_autoload_externally=`` (disabling legacy implicit rule
  loading, default in Bazel 9), Bazel downloads zero archives for unused rule
  sets or unselected versions.

Modular MODULE.bazel structure
------------------------------
Pigweed organizes its root :cs:`MODULE.bazel` into three named files using
Bazel's ``include()`` directive. Upstream-only tooling and default lockfile
bindings are isolated using ``dev_dependency = True`` on ``bazel_dep``,
``use_extension``, and ``register_toolchains`` declarations (plus root-only
overrides), giving each file a clear role:

* **Core Module File** (:cs:`MODULE.bazel`): Declares ``module(name =
  "pigweed")``, minimum compatible consumer ``bazel_dep()`` versions, Pigweed's
  wrapper and toolchain extensions (``pw_rust_crates``, ``pw_py_packages``,
  ``pw_cxx_toolchain``, ``pw_rust``, and ``cipd``), ``use_repo(...)`` bindings,
  and ``include()`` calls to the Resolution Module File and Dev Module File.
  Every downstream project evaluates this file.
* **Resolution Module File** (``bazel/resolve.MODULE.bazel``): Holds Pigweed's
  root-only module overrides along with ``crate`` (``crate.from_cargo``) and
  ``pip`` (``pip.default`` / ``pip.parse``) initialized via
  ``use_extension(..., dev_dependency = True)``. Because ``include()`` files are
  lexically scoped, it binds its own ``dev_dependency = True`` handles to
  ``pw_rust_crates`` and ``pw_py_packages`` and connects Pigweed's resolved
  hubs via ``override_repo``. Bazel skips ``dev_dependency = True`` extension
  usages and ignores overrides when Pigweed is consumed as ``@pigweed``, so
  downstream builds never evaluate Pigweed's Cargo or ``pip`` manifests or hit
  extension hub conflicts. Downstream projects cannot ``include()`` this file
  directly; to reuse Pigweed's Rust or Python pins, pass ``@pigweed//...``
  lockfile targets to ``crate.from_cargo`` or ``pip.parse`` in your own
  ``MODULE.bazel``.
* **Dev Module File** (``bazel/dev.MODULE.bazel``): Holds
  ``bazel_dep(..., dev_dependency = True)``,
  ``use_extension(..., dev_dependency = True)``, and dev-only toolchains
  required solely to develop, lint, test, and document Pigweed itself (such as
  Doxygen, ``mypy``, Sphinx, fuzzers, and board test runners). Downstream
  builds ignore these declarations (and registry packages omit the
  ``include()``).

.. code-block:: bazel

   # Core Module File (MODULE.bazel: works both at Pigweed's root and as @pigweed)
   module(name = "pigweed")

   bazel_dep(name = "bazel_skylib", version = "1.9.0")
   bazel_dep(name = "platforms", version = "1.1.0")
   bazel_dep(name = "rules_cc", version = "0.2.22")

   include("//bazel:resolve.MODULE.bazel")
   include("//bazel:dev.MODULE.bazel")

Because Bazel skips ``dev_dependency = True`` usages and ignores overrides in
non-root modules, ``bazelisk build //...`` works out of the box at Pigweed's
root while downstream roots keep full authority over dependency resolution. Note
that Bazel permits ``include()`` only in the root module or modules under a
non-registry override (such as ``git_override``, ``archive_override``, or
``local_path_override``), so publishing ``@pigweed`` itself as a module in PBR
requires stripping the two ``include()`` lines during registry packaging.

Rust crates (crates.io)
=======================
Pigweed manages third-party crates using
``@rules_rust//crate_universe:extensions.bzl`` (``crate.from_cargo``) and
separates consumer crates from upstream-only development crates:

* **Consumer crates (@rust_crates)**: Public host ``std`` and embedded
  ``no_std`` Rust targets depend on ``@rust_crates``, an alias repository
  generated by ``pw_rust_crates_extension``
  (:cs:`pw_build/pw_rust_crates_extension.bzl`; see
  :ref:`module-pw_third_party_crates_io`) that uses platform constraints to
  select between ``crates_std`` and ``crates_no_std``. Because ``rules_rust``
  rejects non-dev ``crate`` usages without ``.from_cargo`` tags, Pigweed keeps
  ``crate`` out of the Core Module File; instead, ``pw_rust_crates_extension``
  exposes stub ``crates_std`` and ``crates_no_std`` repositories that Pigweed's
  Resolution Module File wires up via ``override_repo``. Non-Rust downstream
  projects never trigger Cargo evaluation.
* **Upstream dev-only crates (@rust_dev_crates)**: Crates used only by Pigweed's
  own CLI tools, unit tests, and board examples live in ``crates_std_dev`` and
  ``crates_no_std_dev`` inside the Dev Module File
  (``bazel/dev.MODULE.bazel``), keeping downstream crate manifests minimal.

Crate updates are gated by ``Cargo.lock`` SHA-256 checksums and ``cargo deny``
license and security advisory checks. Downstream Rust consumers wire crates into
Pigweed uniformly with ``override_repo(pw_rust_crates, ...)``—either overriding
``crates_std`` and ``crates_no_std`` to preserve Pigweed's ``std`` / ``no_std``
feature splitting (against Pigweed's exported lockfiles or the product's own
manifests), or overriding ``rust_crates`` wholesale with a single unified or
vendored repository:

.. code-block:: bazel

   crate = use_extension("@rules_rust//crate_universe:extensions.bzl", "crate")
   crate.from_cargo(
       name = "my_crates_std",
       cargo_lockfile = "//:Cargo.std.lock",
       manifests = ["//:Cargo.std.toml"],
   )
   crate.from_cargo(
       name = "my_crates_no_std",
       cargo_lockfile = "//:Cargo.no_std.lock",
       manifests = ["//:Cargo.no_std.toml"],
   )
   use_repo(crate, "my_crates_no_std", "my_crates_std")

   pw_rust_crates = use_extension(
       "@pigweed//pw_build:pw_rust_crates_extension.bzl",
       "pw_rust_crates_extension",
   )
   override_repo(
       pw_rust_crates,
       crates_std = "my_crates_std",
       crates_no_std = "my_crates_no_std",
   )
   # Or replace @rust_crates directly with a single custom repository:
   # override_repo(pw_rust_crates, rust_crates = "local_rust_crates")

Python packages (PyPI)
======================
Pigweed resolves Python packages through ``rules_python``'s ``pip`` extension
(``pip.default`` and ``pip.parse``) from platform-specific, SHA-256 hash-pinned
lockfiles. Packages resolve to prebuilt binary wheels (``.whl``) wherever
available; ``sdist``-only packages are built locally with ``--no-index`` and
``--no-build-isolation`` against ``rules_python``'s hermetic ``setuptools`` and
``wheel`` packages so ``pip`` never makes unverified network calls:

* **Configurable package index**: Builds default to ``https://pypi.org/simple``
  via ``pip.default(index_url = ...)`` paired with
  ``envsubst = ["PIGWEED_PIP_INDEX_URL"]`` on ``pip.parse``. Enterprise
  environments redirect fetches to an authenticated mirror in ``.bazelrc``
  without editing build files:

  .. code-block:: output

     # .bazelrc
     common --repo_env=PIGWEED_PIP_INDEX_URL=https://<mirror>/simple
     common --credential_helper=<mirror-host>=%workspace%/tools/cred_helper

  Note that ``rules_python`` evaluates ``pip.default`` tags only in the root
  module: downstream roots reusing Pigweed's lockfiles must also declare
  ``pip.default`` in their root ``MODULE.bazel`` when overriding ``index_url``
  or mapping Pigweed's ``none_any_mcu`` platform for MCU builds.
* **Consumer vs. dev split**: Consumer build, code-generation, and host
  library packages (``@pigweed_python_packages``) are exposed via a thin
  ``pw_py_packages`` wrapper extension (``pw_build/pw_py_packages_extension.bzl``)
  rather than binding ``pip`` directly in the Core Module File. Upstream Pigweed
  resolves ``@pigweed_python_packages`` via ``override_repo`` in the Resolution
  Module File, separated from upstream developer tools (such as ``sphinx``,
  ``mypy``, and ``pylint``) in the Dev Module File. Independent ``pip``
  consumers avoid hub collisions, and builds that do not use Pigweed Python
  never evaluate ``pip``.
* **Consistent override_repo binding**: Downstream projects that use
  Pigweed's Python dependencies—whether via root ``pip.parse`` (against
  Pigweed's lockfiles or their own) or a local wheelhouse—bind
  ``pigweed_python_packages`` uniformly with ``override_repo``:

  .. code-block:: bazel

     pw_py_packages = use_extension(
         "@pigweed//pw_build:pw_py_packages_extension.bzl",
         "pw_py_packages_extension",
     )
     override_repo(
         pw_py_packages,
         pigweed_python_packages = "project_python_packages",
     )

Prebuilt binaries and toolchains (CIPD)
=======================================
Pigweed fetches prebuilt host binaries and cross-compilation toolchains
(Clang/LLVM, Arm GCC, Rust, sysroots, and host utilities) through three module
extensions—``cipd`` (:cs:`pw_env_setup/bazel/cipd/defs.bzl`),
``pw_cxx_toolchain`` (:cs:`pw_toolchain/cc/pw_cxx_toolchain.bzl`), and
``pw_rust`` (:cs:`pw_toolchain/rust/extensions.bzl`):

* **Hardened CIPD default**: Package declarations pin immutable,
  content-addressed SHA-256 instance IDs backed by SLSA build provenance rather
  than mutable tags. Consumer packages use ``cipd_ext``, while upstream-only
  formatters, analyzers, and emulators use ``cipd_dev_ext``
  (``dev_dependency = True``) so downstream projects only fetch toolchains and
  tools required by their active target platforms.
* **Non-CIPD escape hatch**: Environments that supply custom toolchains or
  cannot run CIPD can avoid bootstrapping ``@cipd_client`` in either of two
  ways:

  * **Register your own C/C++ toolchain**: Declare and register your own
    ``cc_toolchain`` in the root ``MODULE.bazel`` instead of registering
    ``@pigweed//pw_toolchain/...`` toolchains. Because Bazel instantiates CIPD
    repositories lazily, ``@cipd_client`` is never fetched when no selected
    toolchain or tool target relies on CIPD.
  * **Override Pigweed's CIPD repositories**: When reusing
    ``@pigweed//pw_toolchain/host_clang`` offline, override every CIPD-backed
    repository active for the build—on Linux host builds, both
    ``llvm_toolchain`` (on ``pw_cxx_toolchain``) and ``"pigweed.linux_sysroot"``
    (on ``cipd`` via Starlark dict unpacking)—and ensure local repositories
    provide the expected ``DirectoryInfo`` targets (plus override or avoid
    Arm/Zephyr toolchains, ``pw_rust``, or ``@bloaty`` if built):

    .. code-block:: bazel

       local_repository = use_repo_rule(
           "@bazel_tools//tools/build_defs/repo:local.bzl",
           "local_repository",
       )
       local_repository(
           name = "local_llvm_toolchain",
           path = "third_party/toolchains/llvm",
       )
       local_repository(
           name = "local_linux_sysroot",
           path = "third_party/toolchains/sysroot",
       )

       pw_cxx_toolchain = use_extension(
           "@pigweed//pw_toolchain/cc:pw_cxx_toolchain.bzl",
           "pw_cxx_toolchain",
       )
       override_repo(pw_cxx_toolchain, llvm_toolchain = "local_llvm_toolchain")

       cipd_ext = use_extension(
           "@pigweed//pw_env_setup/bazel/cipd:defs.bzl",
           "cipd",
       )
       override_repo(cipd_ext, **{"pigweed.linux_sysroot": "local_linux_sysroot"})

Enforcing zero-network builds
=============================
Air-gapped CI pipelines and network-jailed AI agent sandboxes can
deterministically verify zero external network access—and block unvetted package
names at resolution time—by combining a local PBR checkout, repository
overrides, and Bazel's downloader controls:

.. code-block:: output

   # .bazelrc
   common --registry=file://%workspace%/third_party/pigweed_registry
   common --incompatible_autoload_externally=
   common --repository_disable_download
   common --downloader_config=%workspace%/bazel/offline_downloader.cfg

.. code-block:: output

   # bazel/offline_downloader.cfg
   block *

Because ``--repository_disable_download`` rejects ``http_archive`` fetches
(even for ``file://`` archive URLs) on a cold cache, cold zero-network builds
require ``"type": "local_path"`` or local ``"type": "git_repository"`` PBR
entries (alongside vendored or local crate, wheel, and toolchain repositories)
or a pre-populated ``--repository_cache`` / ``--distdir``.

Ecosystem summary
=================
The table below summarizes the repository bindings, default sources, integrity
controls, and override mechanisms across all four ecosystems:

.. list-table::
   :header-rows: 1

   * - Ecosystem
     - Repository / extension
     - Default source and integrity
     - Custom / offline override
   * - **Bzlmod**
     - ``bazel_dep()`` in the Core Module File (``MODULE.bazel``)
     - Pigweed Bazel Registry (PBR) + static archive mirror (public BCR
       disabled); SHA-256 ``integrity`` + registry patches
     - Root version pins against PBR, or custom/mirrored registry
       (``--registry=file:///path/to/pbr``)
   * - **crates.io**
     - ``@rust_crates`` (``pw_rust_crates_extension``)
     - ``crate.from_cargo`` in the Resolution Module File; SHA-256
       ``Cargo.lock`` + ``cargo deny`` audits
     - Root ``Cargo.toml`` or vendored crate repo via
       ``override_repo(pw_rust_crates, ...)`` (``crates_std`` /
       ``crates_no_std`` or ``rust_crates``)
   * - **PyPI**
     - ``@pigweed_python_packages`` (``pw_py_packages_extension``)
     - ``https://pypi.org/simple`` via ``pip.parse`` in the Resolution Module
       File; SHA-256 hash-pinned lockfiles + ``--no-index --no-build-isolation``
     - Proxy via ``PIGWEED_PIP_INDEX_URL``, root ``pip.parse``, or local
       wheelhouse via ``override_repo(pw_py_packages, ...)``
   * - **CIPD**
     - ``pw_cxx_toolchain``, ``pw_rust``, ``cipd``
     - Hardened CIPD packages; content-addressed SHA-256 instance IDs + SLSA
       provenance
     - Custom ``cc_toolchain`` registration, toolchain tag overrides, or local
       repos via ``override_repo(...)`` on active CIPD extensions

-------------------------------
Operating across user workflows
-------------------------------
Upstream Pigweed developers
===========================
Upstream Pigweed developers work directly at the Pigweed repository root using
standard ``bazelisk build //...`` and ``bazelisk test //...`` commands. Because
Pigweed is the root module, Bazel evaluates the Core Module File, Resolution
Module File, and Dev Module File together—giving upstream Pigweed developers
and IDE language servers full access to linters, documentation builders, and
hardware test runners.

Presubmit CI continuously builds ``@pigweed`` as an external ``bazel_dep`` via
:cs:`pw_build/bazel_tests/non_root_test` to catch any public target that
accidentally references a ``dev_dependency`` repository or root-only label.

Downstream product developers
=============================
Downstream developers building a shipping product declare
``bazel_dep(name = "pigweed", ...)`` in their product root ``MODULE.bazel``.
Bazel automatically ignores Pigweed's Resolution Module File and Dev Module
File, supporting fresh-rolling, slow-rolling, and air-gapped workflows without
forking ``@pigweed``:

* **Fresh-dependency workflow**: Point ``.bazelrc`` at PBR, bind
  ``pip.parse`` and ``crate.from_cargo`` in your product root ``MODULE.bazel``
  to Pigweed's exported lockfile labels (``@pigweed//pw_env_setup/py:...`` and
  ``@pigweed//third_party/crates_io/...``) via ``override_repo(pw_py_packages,
  ...)`` and ``override_repo(pw_rust_crates, ...)``, and roll ``@pigweed`` on a
  regular cadence. Vetted module updates and ``source.json`` patches apply
  automatically without local patch maintenance.
* **Controlled-cadence workflow**: Point ``.bazelrc`` at PBR while pinning
  ``@pigweed``, exact module versions (``MODULE.bazel`` and
  ``MODULE.bazel.lock``), ``Cargo.lock``, Python lockfiles, and toolchain
  digests in the product root so your team can advance versions on its own
  qualification schedule without maintaining a separate registry.
* **Custom-registry or air-gapped workflow**: Downstream organizations that
  require offline builds or proprietary modules can mirror PBR
  (``--registry=file://...``) or layer their own registry, register custom
  ``cc_toolchain`` targets, or swap ``@rust_crates``,
  ``@pigweed_python_packages``, and CIPD repositories uniformly via
  ``override_repo``.

Downstream platform builders (such as OpenPRoT and custom SDKs)
===============================================================
Downstream developers building a reusable platform on top of Pigweed—such as
`OpenPRoT <https://github.com/OpenPRoT/openprot>`__ or an internal silicon
or product-line SDK—must support standalone development at the platform root
while allowing their own downstream developers to control final dependency
versions and overrides.

Platform repositories achieve this by **mirroring Pigweed's architecture**:

#. **Use PBR directly or layer a platform registry**: Platform projects can
   rely directly on PBR, or optionally layer a platform Bazel registry ahead of
   PBR (``common --registry=<platform-registry>`` followed by
   ``common --registry=<pbr>``) when adding platform-specific modules or
   patches. Any patches in ``source.json`` propagate automatically to
   downstream builds.
#. **Separate Core, Resolution, and Dev Module Files**: The platform's Core
   Module File (``MODULE.bazel``) declares ``@pigweed`` and the platform's own
   minimum consumer ``bazel_dep()`` versions, while placing standalone lockfile
   bindings in its Resolution Module File (``bazel/resolve.MODULE.bazel``) and
   platform-only test/lint tools in its Dev Module File
   (``bazel/dev.MODULE.bazel``). (If publishing releases to a Bazel registry
   rather than consuming source or Git overrides, strip the two ``include()``
   lines as described earlier.)
#. **Expose overridable package repositories and extensions**: Platform-specific
   crates or Python dependencies use wrapper extensions whose default
   ``crate.from_cargo`` and ``pip.parse`` bindings live in the platform's
   Resolution Module File. When a downstream product integrates both
   ``@platform`` and ``@pigweed``, only the product's root bindings execute,
   wiring one top-level lockfile or offline mirror across all three tiers via
   ``override_repo``.

----------------------------
Package lifecycle operations
----------------------------
The sections below summarize how maintainers add, update, override, and remove
dependencies across Pigweed and downstream workspaces.

Adding a package
================
#. **Classify scope**: Determine whether the package is required by downstream
   consumers of public targets (**non-dev**) or only for developing, testing, or
   documenting the current repository (**dev-only**).
#. **Add and verify the dependency**:

   * **Bzlmod**: Add the vetted module version, ``BUILD.bazel`` overlay (if
     needed), patches, and SHA-256 archive ``integrity`` entry to PBR, then
     declare ``bazel_dep()`` in the Core Module File (non-dev) or Dev Module
     File (dev-only).
   * **crates.io**: Add the crate to the non-dev or ``_dev`` ``Cargo.toml``,
     run ``cargo deny check``, and regenerate the ``@rust_crates`` alias file
     (``alias_hub.BUILD``).
   * **PyPI**: Add the requirement to the non-dev or dev requirements file and
     regenerate the SHA-256 hash-pinned platform lockfiles.
   * **CIPD**: Declare the SHA-256 package digest in ``cipd_ext`` (non-dev) or
     ``cipd_dev_ext`` (dev-only).

Updating a package
==================
* **Upstream Pigweed and fast-rolling downstream projects**: Admit the new
  version to PBR or regenerate the lockfile (``MODULE.bazel.lock``,
  ``Cargo.lock``, or Python lockfiles), bump the minimum ``bazel_dep()`` floor
  in ``MODULE.bazel`` only if the code requires newer APIs, and verify host,
  target, and non-root CI builds.
* **Controlled-cadence projects**: Update pinned versions in your root
  ``MODULE.bazel`` and lockfiles directly against PBR (or promote versions into
  an optional internal registry) on your qualification branch. If an updated
  library requires a newer transitive version than your root pins allow, Bazel's
  MVS reports the exact version constraint mismatch immediately during module
  resolution.

Swapping or overriding a package
================================
* **Bzlmod**: Serve a patched or custom module version from a higher-priority
  ``--registry`` entry in ``.bazelrc``, or use ``single_version_override`` /
  ``archive_override`` / ``local_path_override`` in the root ``MODULE.bazel``
  for local testing.
* **crates.io, PyPI, and CIPD**: Use the extension ``override_repo`` hooks,
  ``--repo_env=PIGWEED_PIP_INDEX_URL=...``, or custom toolchain options
  summarized in *Ecosystem summary* above.

Removing a package
==================
Remove the declaration from the Core Module File (or Dev Module File)
and the corresponding ``Cargo.toml``, requirements file, or CIPD extension
declaration, regenerate lockfiles, and retire any temporary PBR patches once
upstream fixes land.
