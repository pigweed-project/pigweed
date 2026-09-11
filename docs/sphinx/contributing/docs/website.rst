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
located at ``//docs/sphinx/_extensions/sitemap.py``. A custom extension is necessary
because the ``pigweed.dev`` production server redirects pages that end in
``…/docs.html`` to ``…/`` (e.g. ``pigweed.dev/pw_string/docs.html`` redirects to
``pigweed.dev/pw_string/``) and no third-party extension supports the kind of
URL rewrite customization that we need. See :bug:`386257958`.

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

To solve this, the ``<pw-header>`` Web Component dynamically inspects the
current document's relative root path (derived from Sphinx's
``DOCUMENT_NAME`` or ``data-content_root`` attribute) and rewrites all
top-level header URLs on page load so that links resolve relative to the
active server's root.

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
