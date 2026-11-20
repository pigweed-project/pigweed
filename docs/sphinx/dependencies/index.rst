.. _docs-dependencies:

============
Dependencies
============
.. note::

   Pigweed's dependency architecture is actively rolling out. Components are
   written in the present tense to describe Pigweed's end state rather than
   what is implemented at ``HEAD`` today. See :ref:`docs-dependencies-roadmap`
   for current status.

Modern projects assemble software across many ecosystems: C and C++ libraries,
Rust crates, Python, Go, and Java packages, code generators, silicon vendor
SDKs, and cross-compilation toolchains. Governing these external dependencies is
one of the largest sources of supply chain risk, maintenance toil, and
integration overhead in modern software projects.

This page explains the dependency problems common to large projects, the design
principles Pigweed follows to address them, and how Pigweed's dependency model
works at a high level.

Three terms appear throughout this page:

* A **manifest** is the file where a project or library lists the dependencies it
  directly uses and the minimum compatible versions it expects.
* A **lockfile** records the exact versions and cryptographic hashes used for a
  reproducible build.
* A **registry** is a catalog that hosts package metadata (and often source
  archives and patches) that a build system uses to fetch dependencies.

---------------------------------
Why dependency management is hard
---------------------------------
Supply chain security and project control
=========================================
Product teams are accountable for everything that ships in a release and every
tool that runs during a build:

* **Provenance and auditing**: Frameworks such as `SLSA <https://slsa.dev/>`__
  (Supply-chain Levels for Software Artifacts), Software Bills of Materials
  (SBOMs), and corporate security policies require cryptographic hashes,
  auditable build provenance, and license tracking for both source archives and
  prebuilt binaries.
* **Local patches and vendor SDKs**: Projects frequently need to patch a library
  for target hardware, substitute a vendor-qualified SDK, or route downloads
  through an internal artifact proxy without forking upstream build files.
* **Offline builds**: Regulated industries, factory lines, and sandboxed AI
  agent environments often run with little or no internet access. Every
  dependency must support offline mirroring, and teams must be able to block
  public registries so builds cannot pull unapproved packages.
* **Centralized vs. distributed vulnerability management**: When every team
  vendors dependencies locally or maintains private forks, each project must
  track security advisories, triage disclosures, and propagate patches on its
  own. Centralizing dependency curation lowers that duplicated effort and lets
  compromised packages be quarantined immediately across builds.

Different teams update on different schedules
=============================================
Updating one library often bumps shared indirect (transitive) dependencies
unexpectedly, potentially triggering new security and license reviews.

Projects also upgrade on different schedules:

* **Active development projects** update dependencies frequently so upgrades
  stay small and bug fixes or security patches land quickly.
* **Release and regulated branches** (such as certified products, hardware
  bring-up lines, and long-term support branches) hold dependency versions steady
  and upgrade only at planned qualification milestones.

A single dependency model has to support both workflows cleanly, letting release
branches stay frozen without blocking fast-moving teams from taking routine
updates.

Multi-tier platforms
====================
Projects rarely consist of just one library and one shipping product. Teams
often build an **intermediate platform** (such as `OpenPRoT
<https://github.com/OpenPRoT/openprot>`__ or a silicon vendor SDK) on top
of Pigweed and distribute that platform to product teams:

.. mermaid::

   flowchart TD
       Product["Product firmware (build root)<br/>Owns concrete lockfiles, versions, and mirrors"]
       Platform["Intermediate platform (such as OpenPRoT or a silicon SDK)<br/>Builds standalone, or declares min versions downstream"]
       Pigweed["Foundational SDK (Pigweed)<br/>Declares minimum versions and vetted fixes"]
       External["Resolved external dependencies<br/>Final C/C++, Rust, Python, Go, Java, and toolchain versions"]

       Product -->|"depends on"| Platform
       Platform -->|"depends on"| Pigweed
       Product ==>|"owns concrete versions & mirrors"| External
       Platform -.->|"minimum versions only"| External
       Pigweed -.->|"minimum versions only"| External

*(Thick arrow: the product build root governs concrete versions, lockfiles, and
mirrors. Dashed arrows: lower tiers declare only minimum compatible versions.)*

These layers have competing needs. An intermediate platform has to build and
run tests on its own for platform contributors, yet behave like an ordinary
library when pulled into a shipping product whose maintainers control final
versions, patches, and mirrors. Security and portability fixes needed by lower
layers must also flow upward automatically instead of being copied into every
repository. At the same time, contributor tools, documentation generators, and
compilers for unused architectures must not bleed into downstream builds and slow
down fresh builds or CI runs.

-----------------
Design principles
-----------------
Pigweed's design rests on four build-system-agnostic principles:

* **Separate what a library needs from how a project resolves it**: A library's
  manifest declares the minimum compatible versions its code requires, while the
  top-level project controls exact versions (via lockfiles), patches, and
  download mirrors. Separating the two prevents build-file forks and lets
  multi-tier platforms compose cleanly.
* **Keep version-graph checks and lazy downloading**: Keep machine-readable
  version constraints across the entire dependency graph, and download archives
  only for the targets you actually build.
* **Keep contributor tooling separate from library dependencies**: Separate tools
  used only to lint, test, or document an upstream repository from the
  dependencies required by consumers, and check that boundary continuously in
  CI. Otherwise, downstream projects waste time downloading unused tools on fresh
  builds and end up mirroring, auditing, and listing software in their SBOM that
  they never use or ship.
* **Verify archives by hash and make external sources easy to swap**: Verify
  every source archive, package, and prebuilt binary by immutable cryptographic
  hash, and reference external ecosystems through configurable names rather than
  hardcoded URLs. Hashes catch tampering and drift, while configurable names let
  the same build definitions work online, behind a proxy, or completely offline.

-------------------
High-level approach
-------------------
Pigweed puts these principles to work in three ways.

Keep dev tools and default lockfiles out of library manifests
=============================================================
A library's public manifest lists only the dependencies and minimum compatible
versions its own targets need. Tools used only to develop, lint, test, or
document the repository live in a separate development manifest, and default
lockfile bindings live in a separate defaults file.

* **Developing inside Pigweed:** Builds load all three files, so contributors
  get linters, tests, documentation tools, and working lockfile defaults out of
  the box.
* **Depending on Pigweed:** Builds read only Pigweed's public manifest.
  Downstream projects skip Pigweed's contributor tools and keep control over
  their own lockfiles.

Intermediate platforms follow the same layout so contributors can test them on
their own without leaking dev tools or lockfiles downstream.

Publish vetted source packages through a curated registry
=========================================================
Pigweed contributes fixes and module definitions upstream to public community
registries where practical, but public registries alone do not solve patch
propagation, non-registry embedded SDKs, or download availability for
downstream firmware teams. Pigweed therefore publishes its vetted dependency
closure—including build definitions for packages absent upstream, shared
patches while upstream fixes land, and deterministic archive mirrors—through a
single curated registry rather than falling back to arbitrary upstream URLs.

Registry metadata and patches are code-reviewed in source control, and every
source archive is verified against a cryptographic hash. Centralizing curation
also narrows external network allowlists to one static mirror and lets Pigweed
handle vulnerability triage, shared patching, and emergency package revocations
in one place rather than leaving every project to audit and patch the same
dependencies independently.

One registry supports both update schedules without forcing downstream teams to
run their own registry infrastructure:

* **Teams tracking recent updates** follow Pigweed's registry and default
  versions, receiving tested upgrades and lower-layer patches automatically.
* **Teams on fixed release branches** pin exact versions in their own manifests
  and lockfiles against the same registry, upgrading only at planned
  qualification milestones. Historical versions stay available unless revoked for
  an active security compromise, balancing emergency fail-closed protection with
  predictable release stability.

Teams that build offline or carry proprietary packages can mirror this registry
locally or layer an internal registry on top of it.

Let top-level projects swap language packages and toolchains
============================================================
Language package managers (such as Cargo for Rust and ``pip`` for Python) and
prebuilt binary toolchains cannot always be served from one source registry.
Pigweed's build definitions therefore refer to language package sets and
toolchains by name rather than hardcoding upstream URLs or lockfiles.

By default, builds resolve those names against Pigweed's hash-verified
lockfiles and default package sources. When your project needs different
sources or versions, you can override those defaults without editing Pigweed:

* Redirect downloads through an internal artifact proxy or mirror.
* Resolve Pigweed's Rust or Python dependencies through your project's own
  lockfiles so every layer shares one set of resolved versions.
* Point to locally vendored packages or custom compilers for offline and
  target-specific builds.

Who controls what
=================
The table below summarizes how responsibilities split across layers:

.. list-table::
   :header-rows: 1

   * - Layer
     - What it controls
   * - **Foundational library (Pigweed)**
     - Minimum compatible versions in its public manifest, shared patches in
       the curated registry, and standalone defaults and dev tools used only
       when building Pigweed directly.
   * - **Intermediate platform**
     - Minimum compatible versions for platform dependencies in its public
       manifest, optional layered patches or registries, and standalone defaults
       and dev tools for platform contributors.
   * - **Product project**
     - Final resolved versions and lockfiles across every layer (using
       Pigweed's defaults out of the box, with optional overrides for custom
       mirrors or proxies, offline vendoring, or custom toolchains if needed).

---------------
Explore further
---------------
.. grid:: 2

   .. grid-item-card:: :octicon:`workflow` Dependency architecture
      :link: docs-build-dependency-architecture
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      See how Pigweed implements this architecture across Bzlmod, crates.io,
      PyPI, and CIPD, how upstream Pigweed developers, downstream product
      developers, and platform builders operate it, and how packages move
      through their lifecycle.

   .. grid-item-card:: :octicon:`milestone` Roadmap & status
      :link: docs-dependencies-roadmap
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Review what is live in Pigweed today, current limitations, and the
      upcoming milestones to complete the architecture.

.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Architecture <pw://build/dependency_architecture.html>
   Roadmap & status <roadmap>
