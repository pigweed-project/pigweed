.. _contrib-docs-website:

===============
Website updates
===============
.. _Sphinx: https://www.sphinx-doc.org

This page discusses how to make frontend and backend website changes
to ``pigweed.dev``, Pigweed's main documentation website, and how to
customize `Sphinx`_, the website generator that powers ``pigweed.dev``.

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

------------------------------------------
Auto-generated source code and issues URLS
------------------------------------------
In the site nav there's a ``Source code`` and ``Issues`` URL for each module.
These links are auto-generated. The auto-generation logic lives in
``//pw_docgen/py/pw_docgen/sphinx/module_metadata.py``.

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

-------------
Inline search
-------------
In the header of every page there's a search box. When you focus that search box
(or press :kbd:`Ctrl+K`) a search modal appears. After you type some text in the
search modal, you immediately see results below your search query. This was
originally Pigweed-specific code, but we upstreamed it to PyData Sphinx Theme in
PR `#2093 <https://github.com/pydata/pydata-sphinx-theme/pull/2093>`_. The
feature is enabled by setting ``html_theme_options['search_as_you_type']`` to
``True`` in :cs:`docs/sphinx/conf.py`.

.. _contrib-docs-website-search-nosearch:

Remove a page from the search results
=====================================
To exclude a page from the search results, add ``:nosearch:`` to the top of the
page's reStructuredText source file.

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
