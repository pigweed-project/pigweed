.. _contrib-docs-website:

===============
Website updates
===============
This page discusses how to make frontend and backend website changes
to ``pigweed.dev``, Pigweed's main documentation website.

.. _contrib-docs-website-overview:

--------
Overview
--------
.. _Doxygen: https://www.doxygen.nl
.. _rustdoc: https://doc.rust-lang.org/rustdoc/
.. _Sphinx: https://www.sphinx-doc.org
.. inclusive-language: ignore
.. _extensions: https://www.sphinx-doc.org/en/master/development/tutorials/extending_build.html

The key thing to understand is that ``pigweed.dev`` is actually 3 completely
separate documentation websites combined together. The C/C++ API reference is
generated with `Doxygen`_. The Rust API reference is generated with `rustdoc`_.
Everything else is generated with `Sphinx`_. We use Bazel to ensure that the
rustdoc and Doxygen builds finish before the Sphinx build and are available as
inputs to Sphinx. We then use Sphinx `extensions`_ to glue the 3 sites together
into one cohesive whole.

.. _contrib-docs-website-overview-build:

Life of a docs build
====================
A brief explanation of what happens when you run ``bazelisk build //docs``:

#. Bazel resolves the ``//docs`` alias to ``//docs/sphinx:docs`` and attempts
   to build that target. I.e. it attempts to run the Sphinx build.

#. Bazel detects that it must actually run the Doxygen and rustdoc builds first
   because the ``//docs/sphinx:docs`` target lists  ``//docs/doxygen:html`` and
   ``//pw_rust:docs`` as ``deps``. When these targets finish building, the
   complete Doxygen and rustdoc subsites are essentially provided as inputs to
   the Sphinx build.

#. The Sphinx build runs. Sphinx invokes our extensions. These extensions
   inspect and mutate Doxygen, rustdoc, and Sphinx information in order to glue
   the 3 subsites together.

.. _contrib-docs-website-overview-deploy:

Docs deployments
================
How do docs updates get deployed to ``pigweed.dev``? When a new commit
merges, we have a CI pipeline that runs ``bazelisk build //docs`` and uploads
the built site to a Google Cloud Storage bucket. A Google App Engine server
runs the production site. All of the docs deployment workflow is closed source.

.. _contrib-docs-website-images:

-------------
Image hosting
-------------
Images should not be checked into the Pigweed repository because
it significantly slows down the repository cloning process.
Images should instead be hosted on Pigweed's image CDN,
``https://www.gstatic.com/pigweed/``.

If you're adding an image to a ``pigweed.dev`` doc, here's the
recommended workflow:

#. When drafting a change, it's OK to temporarily check
   in the image so that there is a record of it in Gerrit.

#. When your change is almost ready to merge, a Pigweed teammate
   will upload your image to Pigweed's image CDN, and then
   leave a comment on your change asking you to delete the
   checked-in image and replace the reference to it with the
   URL to the CDN-hosted image.

.. _go/pigweed-gstatic: http://go/pigweed-gstatic

Google employees working with external contributors should go to
`go/pigweed-gstatic`_ to upload images.

.. _contrib-docs-website-redirects:

----------------
Create redirects
----------------
.. _sphinx-reredirects: https://pypi.org/project/sphinx-reredirects/

``pigweed.dev`` supports client-side HTML redirects. The redirects are powered
by `sphinx-reredirects`_.

To create a redirect:

#. Open ``//docs/sphinx/redirects.json``.

.. _Usage: https://documatt.com/sphinx-reredirects/usage.html

#. Create a new key-value pair. The key is the obsolete path that should be
   redirected. The value is the redirect destination. See `Usage`_.

   * The path in the key should not have a filename extension. The path in the
     value should.

   * The path in the value should be relative to the path in the key.

   * The path in the value should contain the full HTML filename. E.g.
     values should be like ``./examples/index.html``, not ``./examples/``.

Example of the redirect that ``sphinx-reredirects`` auto-generates:

.. code-block:: html

   <html>
     <head>
       <meta http-equiv="refresh" content="0; url=pw_sys_io_rp2040/docs.html">
     </head>
   </html>

.. _meta refresh and its HTTP equivalent: https://developers.google.com/search/docs/crawling-indexing/301-redirects#metarefresh

.. note::

   Server-side redirects are the most robust solution, but client-side
   redirects are good enough for our needs:

   * Client-side redirects are supported in all browsers and should
     therefore work for all real ``pigweed.dev`` readers.

   * Client-side redirects were much easier and faster to implement.

   * Client-side redirects can be stored alongside the rest of the
     ``pigweed.dev`` source code.

   * Google Search interprets the kind of client side redirects that we use
     as permanent redirects, which is the behavior we want. See
     `meta refresh and its HTTP equivalent`_. The type of client-side redirect
     we used is called a "instant ``meta refresh`` redirect" in that guide.

.. _contrib-docs-website-urls:

-----------------------------
Auto-generated metadata links
-----------------------------
In the site nav, ``C/C++ API reference``, ``Rust API reference``,
``Source code``, and ``Issues`` URLs are auto-generated for each module.
The auto-generation logic lives in
``//docs/sphinx/_extensions/module_metadata.py``.

By default, these links are appended to the end of each module homepage's
``.. toctree::``. To control their placement within the TOC, authors can insert
standard placeholder slugs (``pw://cc-api-ref``, ``pw://rust-api-ref``,
``pw://source-code``, ``pw://issues``) into the ``.. toctree::`` directive.

.. _contrib-docs-website-categories:

---------------------
Module categorization
---------------------
All Pigweed modules listed in :cs:`PIGWEED_MODULES` are organized into
top-level categories across Sphinx (:ref:`docs-module-guides` and category
landing pages such as :doc:`/os/index`), Doxygen (``//docs/doxygen/modules.h``),
and ``rustdoc`` (``//pw_rust/rust/pigweed/src/lib.rs``).

Category metadata is defined in :cs:`docs/common/metadata/categories.toml`
(validated against :cs:`docs/common/metadata/categories_schema.json`), and each
module's metadata and category assignment lives in ``//<module>/module.toml``
(validated against :cs:`docs/common/metadata/module_schema.json`). Everything
else is generated from them by the synchronization CLI:

.. code-block:: console

   $ bazelisk run //docs/common/metadata:sync

Generated content lives between ``categories-start`` and ``categories-end``
marker comments and is labeled ``AUTO-GENERATED``. Don't edit it by hand. The
docs build (``bazelisk build //docs``) fails if any generated content is out of
date or if any ``module.toml`` file is missing or invalid; the error message
explains how to fix the problem. To run the same checks without building the
docs:

.. code-block:: console

   $ bazelisk run //docs/common/metadata:sync -- --check

How categories are structured
=============================
* In :cs:`docs/common/metadata/categories.toml`, each ``[<category_id>]`` table
  maps a category ID to its metadata (``title``, ``homepage``, ``description``,
  and either ``icon`` for a top-level category or ``parent`` for a subcategory).
  Categories are displayed in the order they are listed, and the first 12
  top-level categories get a card on the ``pigweed.dev`` homepage. Subcategories
  (such as ``zephyr`` with ``parent = "os_backends"`` or ``rpi`` with
  ``parent = "hw"``) are nested under their parent category in
  :ref:`docs-module-guides`, on the parent category's landing page, in Doxygen,
  and in ``rustdoc``. Within a category, direct modules are sorted
  alphabetically and placed before subcategories by default; set an optional
  ``pin = [...]`` list on a category table to pin specific direct module names
  or child subcategory IDs to the top in the specified order.
* In ``//<module>/module.toml``, each module defines its metadata, including a
  required ``status`` and at least one of ``[category]`` or ``[facade]``:

  * ``[category]`` has ``id = "<category_id>"`` and ``toctree = "<title>"`` to
    set the module's **canonical category**: the parent of the module's docs in
    the Sphinx ``toctree`` (rendered as ``<title> <../<module>/docs>`` on the
    category's landing page). Both ``id`` and ``toctree`` are required.
  * ``[facade]`` has ``module = "<facade_module>"`` and ``toctree = "<title>"``
    to identify the facade module that a backend module implements (for example,
    ``module = "pw_log"`` and ``toctree = "Zephyr"``). Backend modules are
    progressively disclosed on their facade's ``//<facade>/backends.rst`` page
    rather than repeating the facade's functional category (``observability``,
    ``hal``, ``os``, etc.). Both ``[category]`` and ``[facade]`` require their
    own ``toctree`` field because a backend's short title changes depending on
    where it is listed (for example, ``pw_log_zephyr`` is titled ``Logging`` on
    the Zephyr page and ``Zephyr`` under ``pw_log``'s backends). Target-specific
    backends (for an OS or hardware platform category such as ``zephyr``,
    ``fuchsia``, ``freertos``, or ``rpi``) specify both ``[category]`` and
    ``[facade]``, making the target category their canonical ``toctree`` parent
    while ``//<facade>/backends.rst`` cross-references them with ``pw://``.
    Target-agnostic backends (such as ``pw_assert_basic`` or
    ``pw_log_tokenized``) omit ``[category]``, making
    ``//<facade>/backends.rst`` their canonical ``toctree`` parent.

Add a module
============
For upstream modules, ``pw module create`` (see
:ref:`module-pw_module-module-create`) scaffolds ``//<module>/module.toml``
with a commented-out ``[category]`` / ``[facade]`` template and adds
``"module.toml"`` to the module's ``sphinx_docs_library`` in
``//<module>/BUILD.bazel``. Uncomment and fill out ``[category]`` or
``[facade]``, then run the synchronization CLI. Until you do,
``bazelisk build //docs`` fails with a validation error for the new module.

Update a module's category
==========================
#. Edit the module's ``[category]`` table in ``//<module>/module.toml`` (for
   example, :cs:`pw_alignment/module.toml`):

   .. code-block:: toml

      status = "stable"
      tagline = "Natural object alignment, guaranteed"
      languages = ["C++"]

      [category]
      id = "memory"
      toctree = "Alignment"

#. Run the synchronization CLI to update Sphinx, Doxygen, and rustdoc files
   in-place:

   .. code-block:: console

      $ bazelisk run //docs/common/metadata:sync

#. Verify the documentation build:

   .. code-block:: console

      $ bazelisk build //docs

Add, rename, or delete a category
=================================
#. Add, rename, or remove the ``[<category_id>]`` table in
   :cs:`docs/common/metadata/categories.toml`.
#. Update the ``[category]`` tables in ``//<module>/module.toml`` for the
   affected modules. Every category must contain at least one module.
#. If you deleted or renamed a category, delete its old landing page or remove
   its ``.. categories-start`` / ``.. categories-end`` block.
#. Run the synchronization CLI:

   .. code-block:: console

      $ bazelisk run //docs/common/metadata:sync

   This automatically:

   * Scaffolds the landing page for any newly added category and registers it in
     ``//docs/sphinx/BUILD.bazel``.
   * Updates ``//docs/sphinx/BUILD.bazel``, ``//docs/sphinx/platform.rst``,
     ``//docs/sphinx/modules.rst``, ``//docs/sphinx/index.rst``,
     ``//docs/doxygen/modules.h``, and ``//pw_rust/rust/pigweed/src/lib.rs``.
#. Run ``bazelisk build //docs``.

Customize a category landing page
=================================
Category landing pages live at the path specified by each category's
``homepage`` field in :cs:`docs/common/metadata/categories.toml` (for example,
:cs:`docs/sphinx/memory/index.rst` or :cs:`docs/sphinx/os/index.rst`). Each
category's ``description`` is automatically rendered as a subtitle below the H1
heading on its landing page.

The synchronization CLI only modifies the content between the
``.. categories-start`` and ``.. categories-end`` marker comments. Hand-written
guides, overviews, headings, and diagrams before or after the markers are
preserved; see :cs:`docs/sphinx/os/index.rst` for an example.

To add non-module pages (such as cross-module guides or third-party
integrations) to a category's sidebar navigation, don't edit the auto-generated
``.. toctree::`` inside ``.. categories-start`` and ``.. categories-end``.
Instead, add a separate ``.. toctree::`` directive outside the markers:

* Place the ``.. toctree::`` **after** ``.. categories-end`` (omitting
  ``self``) to append pages after the category's modules (see
  :cs:`docs/sphinx/observability/index.rst` for an example).
* Place the ``.. toctree::`` (starting with ``self``) **before**
  ``.. categories-start`` to list introductory guides before the category's
  modules; the synchronization CLI automatically omits ``self`` from the
  generated ``.. toctree::`` so ``self`` appears only once (see
  :cs:`docs/sphinx/toolchains/index.rst` for an example).

To use an existing page as a category's landing page, set the category's
``homepage`` to the page's path and add ``.. categories-start`` and
``.. categories-end`` lines where the module table should appear. The
synchronization CLI fills in the content between them.

.. _contrib-docs-website-copy:

----------------------------------------
Copy-to-clipboard feature on code blocks
----------------------------------------
.. _sphinx-copybutton: https://sphinx-copybutton.readthedocs.io/en/latest/
.. _Remove copybuttons using a CSS selector: https://sphinx-copybutton.readthedocs.io/en/latest/use.html#remove-copybuttons-using-a-css-selector

The copy-to-clipboard feature on code blocks is powered by `sphinx-copybutton`_.

``sphinx-copybutton`` recognizes ``$`` as an input prompt and automatically
removes it.

There is a workflow for manually removing the copy-to-clipboard button for a
particular code block but it has not been implemented yet. See
`Remove copybuttons using a CSS selector`_.

.. _contrib-docs-website-intersphinx:

---------------------------------------
External cross-references (intersphinx)
---------------------------------------
.. inclusive-language: disable
.. _autodoc: https://www.sphinx-doc.org/en/master/usage/extensions/autodoc.html
.. _intersphinx: https://www.sphinx-doc.org/en/master/usage/extensions/intersphinx.html
.. inclusive-language: enable

Cross-references to external documentation are powered by `intersphinx`_ and
configured via ``intersphinx_mapping`` in :cs:`docs/sphinx/conf.py`.

When `autodoc`_ generates Python API references or when reStructuredText files
use ``:py:*:`` roles (such as ``:py:class:`pathlib.Path```), Sphinx first
checks whether the symbol is defined in Pigweed's own documentation. If the
symbol is not found locally, ``intersphinx`` looks it up in the external
inventories configured in ``intersphinx_mapping`` (which fetches
``https://docs.python.org/3/objects.inv`` over the network at build time) and
links the reference to the official Python documentation.

.. _contrib-docs-website-fonts:

--------------------
Fonts and typography
--------------------
``pigweed.dev`` is taking an iterative approach to its fonts and typography.
See :bug:`353530954` for context, examples of how to update fonts, and to
leave feedback.

.. _Typography: https://m3.material.io/styles/typography/fonts

Rationale for current choices:

* Headings: ``Lato``. Per UX team's recommendation.
* Copy: ``Noto Sans``. ``Noto`` is one of two fonts recommended by Material
  Design 3. It seems to complement ``Lato`` well. See `Typography`_.
* Code: ``Roboto Mono``. Also per UX team's recommendation. ``Roboto Mono``
  is mature and well-established in this space.

.. _contrib-docs-website-search:

--------------
In-site search
--------------
In the header of every page there is a search box. When you focus that search
box (or press :kbd:`Ctrl+K` or :kbd:`/`), a search modal appears. After you
type a query, search results appear instantly.

In-site search is powered by `Pagefind <https://pagefind.app/>`_. Pagefind
builds a static search index after Sphinx finishes building the HTML output.
Because Pagefind indexes the rendered HTML files rather than Sphinx source files
directly, it indexes all generated subsites—achieving 100% search index
coverage across Sphinx documentation, Rustdoc crates, and Doxygen C/C++ API
references. The index generation is handled by the Sphinx extension located at
``//docs/common/search.py``.

The search modal UI is rendered by Pagefind's web components integrated into
the universal header component (``//docs/common/header.js`` and
``//docs/common/search.css``).

.. _contrib-docs-website-search-customize:

Customize search indexing
=========================
Search index generation is configured in ``//docs/common/search.py``.

Exclude UI elements from the index
----------------------------------
To prevent UI elements (such as navigation headers, breadcrumbs, buttons, or
anchor links) from polluting search results and excerpts, add their CSS
selectors to ``_EXCLUDE_SELECTORS`` in ``//docs/common/search.py``.

Selectors are grouped by subsite:

* ``_PW_EXCLUDE_SELECTORS``: Universal Pigweed components like ``pw-header``,
  ``pw-breadcrumbs``, and the skip link (``.pw-skip-link``).
* ``_SPHINX_EXCLUDE_SELECTORS``: Sphinx elements like section permalink
  symbols (``.headerlink``).
* ``_DOXYGEN_EXCLUDE_SELECTORS``: Doxygen elements like member and section
  anchors (``.anchor``).
* ``_RUSTDOC_EXCLUDE_SELECTORS``: Rustdoc elements like copy buttons
  (``#copy-path``), heading anchors (``.doc-anchor``), "Expand description"
  buttons (``.hideme``), source links (``.main-heading .sub-heading .src``),
  and native skip links (``.skip-main-content``).

.. note::

   Supposedly you can also can exclude elements directly in HTML templates
   or components by adding the ``data-pagefind-ignore="all"`` attribute, but
   this does not seem to work as reliably as the ``_PW_EXCLUDE_SELECTORS``
   approach.

.. _contrib-docs-website-search-nosearch:

Remove a page from search results
=================================
To exclude a page from search results, add ``:nosearch:`` to the top of the
page's reStructuredText source file, or use Pagefind's ``data-pagefind-ignore``
attribute on HTML elements.

.. _contrib-docs-website-sitemap:

------------------
Sitemap generation
------------------
``https://pigweed.dev/sitemap.xml`` is generated by the custom Sphinx Extension
located at ``//docs/sphinx/_extensions/sitemap.py``. At the end of the HTML
build, the extension walks the rendered output directory to index all pages
across Sphinx, Doxygen, and Rustdoc while excluding client-side redirects,
verbatim source pages (``rustdoc/src/``), and non-content utility pages. A
custom extension is also necessary because the ``pigweed.dev`` production
server redirects pages that end in ``…/docs.html`` or ``…/index.html`` to
``…/`` (e.g. ``pigweed.dev/pw_string/docs.html`` redirects to
``pigweed.dev/pw_string/``) and no third-party extension supports the kind of
URL rewrite customization that we need. See :bug:`386257958` and
:bug:`445247697`.

.. _contrib-docs-website-analytics:

-------------------
Google Analytics ID
-------------------
The ``pigweed.dev`` Google Analytics ID is not hardcoded anywhere in the
upstream Pigweed repo. It is passed through the environment like this:

#. Docs builders provide a Google Analytics ID as a command line argument.

#. ``//docs/sphinx/conf.py`` looks for the existence of a ``GOOGLE_ANALYTICS_ID``
   OS environment variable and passes the variable along to Sphinx when found.

#. ``//pw_docgen/py/pw_docgen/sphinx/google_analytics.py`` looks for the
   Sphinx build environment variable and injects the ID (and related
   JavaScript code) into each page's HTML when found.

Passing the ID through the environment helps us ensure that the production
ID is only used when someone views the docs from the production domain
(``pigweed.dev``).

.. _contrib-docs-website-tests:

-------
Testing
-------
All documentation tests are located in ``//docs/tests``. The directory is
a standalone Bazel workspace because the integration tests are slow and pull
in heavy dependencies e.g. `Playwright <https://playwright.dev>`_.

There are 2 types of tests:

* **Static tests** inspect the built HTML files directly without running a
  browser. E.g. verifying that every page contains a ``<pw-header>`` element.
* **Runtime tests** launch a headless Chromium browser instance via
  Playwright to test interactive UI behavior. E.g. verifying that searching via
  Pagefind navigates to expected results.

Quickstart
==========
#. Build the docs:

   .. code-block:: console

      bazelisk build //docs

#. ``cd`` into the tests directory:

   .. code-block:: console

      cd docs/tests

#. Run all tests:

   .. code-block:: console

      bazelisk test --test_output=all //...

   Or run an individual test e.g. ``header_test``:

   .. code-block:: console

      bazelisk test --test_output=all //:header_test

.. _contrib-docs-website-header:

----------------
Universal header
----------------
As mentioned in :ref:`contrib-docs-website-overview`, ``pigweed.dev`` is
actually 3 separate sites glued together: Sphinx, rustdoc, and Doxygen. Yet
as you browse the website there is a consistent header at the top of all pages.
How does that work?

It's essentially postprocessing. We inject an HTML comment
(``<!-- pw-sentinel -->``) into every page. At the end of the Sphinx build, one
of our Sphinx extensions replaces this sentinel comment with the header HTML,
CSS, and JS. We also have to override a lot of the default Doxygen and rustdoc
CSS styling in order to make our custom header look correct on those subsites.
All of the header customization code is located at ``//docs/common/header.*``.

.. _contrib-docs-website-url-rewriting:

Client-side URL rewriting
=========================
``pigweed.dev`` documentation is published in various deployment environments:

* Production site: Rooted at ``https://pigweed.dev/``
* Staging builds: Rooted at arbitrary subpaths (e.g.
  ``https://storage.googleapis.com/pigweed-docs-try/8673174856411998625/index.html``)
* Local preview servers: Rooted at custom ports or local filesystem paths (e.g.
  ``http://localhost:8000/`` or ``file:///path/to/docs/out/``)

Because navigation links cannot assume that the root of the website is
always ``/`` or ``/index.html``, absolute links like
``https://pigweed.dev/pw_string/`` would break staging and local preview
workflows by navigating users away to the production site.

To solve this, ``//docs/common/dev.js`` dynamically inspects the
current document's relative root path (derived from Sphinx's
``DOCUMENT_NAME`` or ``data-content_root`` attribute) and rewrites absolute
production URLs on page load so that links resolve relative to the active
server's root.

.. _contrib-docs-website-theme:

--------------
Theme selector
--------------
The universal site header includes a light/dark theme switcher component
(``<pw-theme>``).

The theme selector component is defined in ``//docs/common/header.js`` and
``//docs/common/header.css``, with root-level color scheme variables defined in
``//docs/common/theme.css``. It automatically syncs with the user's operating
system color scheme preference and persists user selections in
``localStorage``.

.. _contrib-docs-website-breadcrumbs:

-----------
Breadcrumbs
-----------
Immediately below the universal header on all subpages is a universal
breadcrumb bar (``<pw-breadcrumbs>``).

Breadcrumbs are generated statically at build time during the single
postprocessing pass over the output directory (``//docs/common/breadcrumbs.py``)
rather than injected dynamically via client-side JavaScript. This ensures that
navigation remains fully accessible when JavaScript is disabled and renders
immediately without client-side layout shift:

* **Sphinx pages:** Derived from the root documentation toctree hierarchy.
* **C/C++ reference pages (Doxygen):** Formatted as ``Home`` > ``Reference`` >
  ``C/C++`` > ``<category>`` > ``<group>`` > ``<symbol>``.
* **Rust reference pages (Rustdoc):** Formatted as ``Home`` > ``Reference`` >
  ``Rust`` > ``<crate>`` > ``<item>``.
* **Homepage:** Omitted to maintain a clean landing experience.
