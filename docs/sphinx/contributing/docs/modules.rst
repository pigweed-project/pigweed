.. _docs-contrib-docs-modules:

==================================
Module docs contributor guidelines
==================================
Read this page if you're writing documentation for a Pigweed module.
This page shows you how to create high-quality module documentation.

.. _#docs: https://discord.com/channels/691686718377558037/1111766977950797824

Please start a discussion in the `#docs`_ channel of the Pigweed Discord
for any questions or comments related to writing module docs.

.. _docs-contrib-docs-modules-quickstart:

----------
Quickstart
----------
Follow these instructions and you're 95% done!

.. _docs-contrib-docs-modules-quickstart-goldens:

Emulate the golden module docs
==============================
Decide whether your module falls into the "simple", "backend", or "complex"
category, and then just emulate the provided "golden examples" when writing
your own module's docs:

* **Simple** modules generally have small API surfaces, support only one
  programming language, and can be completely documented in less than 2000
  words. Golden examples:

  * :ref:`module-pw_span`
  * :ref:`module-pw_result`
  * :ref:`module-pw_alignment`

* **Backend** modules implement the facade or interface of another module.
  Golden examples:

  * :ref:`module-pw_i2c_rp2040`

* **Complex** modules are everything else. In other words, if it's not a
  simple module or a backend module, then it's a complex module. Golden
  examples:

  * :ref:`module-pw_hdlc`
  * :ref:`module-pw_status`
  * :ref:`module-pw_unit_test`

Simple and backend modules should only have one page of documentation,
``//pw_*/docs.rst``. Complex modules might need multiple pages of
documentation. See :ref:`docs-contrib-docs-modules-theory-multi`.

.. _docs-contrib-docs-modules-metadata:

Add metadata
============
Create your module's metadata:

1. Add a ``pigweed-module`` directive right after the title in your
   module's ``docs.rst`` page:

   .. code-block:: rst

      ====
      pw_*
      ====
      .. pigweed-module::
         :name: pw_*

2. Add metadata for your module in ``//docs/sphinx/module_metadata.json``.
   See ``//docs/sphinx/module_metadata_schema.json`` for the schema
   definition.

   .. code-block:: json

      {
        "pw_alignment": {
          "tagline": "Natural object alignment, guaranteed",
          "status": "stable",
          "languages": [
            "C++"
          ]
        }
      }

   The ``tagline`` should concisely summarize your module's value proposition.

3. Add a ``pigweed-module-subpage`` directive right after the title
   in each of your other docs pages (if your module has multiple docs
   pages):

   .. code-block:: rst

      =============
      API reference
      =============
      .. pigweed-module-subpage::
         :name: pw_*

.. _docs-contrib-docs-modules-quickstart-usage:

Demonstrate basic code usage
============================
If your module provides an API, provide a code example after your
module metadata that demonstrates basic usage of your API.

.. _docs-contrib-docs-modules-quickstart-build:

Show build system setup
=======================
If your module requires build system setup, make sure your
quickstart section provides setup instructions for *all* of
Pigweed's supported build systems:

* Bazel
* GN
* CMake

.. _docs-contrib-docs-modules-quickstart-reference:

Auto-generate complete API references
=====================================
.. inclusive-language: disable

.. _autodoc: https://www.sphinx-doc.org/en/master/usage/extensions/autodoc.html

.. inclusive-language: enable

If your module has a C++ API, use :ref:`Doxygen <style-doxygen>` to
auto-generate your API reference. For a Python API use `autodoc`_.

.. tip::

   Code samples make your class, function, and method references **much**
   more usable and helpful!

.. _docs-contrib-docs-modules-theory:

-------------------
Theory of operation
-------------------
This section explains the theory behind the module docs contributor
guidelines. It's intended for the maintainers of the guidelines. If
you're just writing docs for a module you own you don't need to read the
theory of operation. But if you're curious about details, history, and
rationale, you'll find that context here.

.. _docs-contrib-docs-modules-theory-single:

Single-page ordering
====================
If the module only has a single page of documentation (``docs.rst``)
the sections should be ordered like this:

* :ref:`docs-contrib-docs-modules-theory-sales`
* :ref:`docs-contrib-docs-modules-theory-quickstart`
* :ref:`docs-contrib-docs-modules-theory-guides`
* :ref:`docs-contrib-docs-modules-theory-reference`
* :ref:`docs-contrib-docs-modules-theory-design`
* :ref:`docs-contrib-docs-modules-theory-roadmap`
* :ref:`docs-contrib-docs-modules-theory-size`

The sales pitch must come first, followed by the quickstart instructions.
Everything else beyond that is optional and can be rearranged in whatever way
seems to flow best.

The file must be located at ``//pw_*/docs.rst``.

.. _docs-contrib-docs-modules-theory-multi:

Multi-page ordering
===================
If the module has multiple pages of documentation the pages should
be ordered like this:

.. list-table::
   :header-rows: 1

   * - Page Title
     - Filename
     - Description
   * - ``pw_*``
     - ``docs.rst``
     - The :ref:`docs-contrib-docs-modules-theory-sales` content.
   * - ``Quickstart & guides``
     - ``guides.rst``
     - The :ref:`docs-contrib-docs-modules-theory-quickstart` content followed
       by the :ref:`docs-contrib-docs-modules-theory-guides` content. See the
       note below.
   * - ``API reference``
     - ``api.rst``
     - The :ref:`docs-contrib-docs-modules-theory-reference` content.
   * - ``Design & roadmap``
     - ``design.rst``
     - The :ref:`docs-contrib-docs-modules-theory-design` content. See the
       note below.
   * - ``Code size analysis``
     - ``size.rst``
     - The :ref:`docs-contrib-docs-modules-theory-size` content.

The sales pitch and quickstart instructions are required. Everything else
is optional and can be rearranged in whatever way seems to flow best.

You can split ``Quickstart & guides`` into 2 docs if that works better for
your module. The filenames should be ``get_started.rst`` and ``guides.rst``.

``Design & roadmap`` can also be split into 2 docs. The filenames should be
``design.rst`` and ``roadmap.rst``.

.. _docs-contrib-docs-modules-theory-sales:

Sales pitch
===========
The sales pitch should:

* Assume that the reader is an embedded developer.
* Clearly explain how the reader's work as an embedded developer
  will improve if they adopt the module.
* Provide a code sample demonstrating one of the most important
  problems the module solves. (Only required for modules that expose
  an API.)

.. _docs-contrib-docs-modules-theory-quickstart:

Quickstart
==========
The quickstart instructions should:

* Show how to get set up in Bazel, GN, and CMake.
* Present Bazel instructions first.
* Clearly state when a build system isn't supported.
* Format the instructions with the ``.. tab-set::`` directive. See
  ``//pw_string/guide.rst`` for an example. The Bazel instructions are
  presented in the first tab, the GN instructions in the next, and so on.
* Demonstrate how to complete a common use case. See the next paragraph.

If your quickstart content is on the same page as your guides, then the
quickstart section doesn't need to demonstrate a common use case. The reader
can just scroll down and see how to complete common tasks. If your quickstart
content is a standalone page, it should demonstrate how to complete a common
task. The reader shouldn't have to dig around multiple docs just to figure out
how to do something useful with the module.

.. _docs-contrib-docs-modules-theory-guides:

Guides
======
The guides should:

* Focus on how to solve real-world problems with the module. See
  `About how-to guides <https://diataxis.fr/how-to-guides/>`_.

.. _docs-contrib-docs-modules-theory-reference:

API reference
=============
The API reference should:

* Be auto-generated from :ref:`style-doxygen` (for C++ / C APIs) or
  `autodoc`_ (for Python APIs).
* Provide a code example demonstrating how to use the class, at minimum.
  Consider whether it's also helpful to provide more granular examples
  demonstrating how to use each method, variable, etc.

The typical approach is to order everything alphabetically. Some module docs
group classes logically according to the tasks they're related to. We don't
have a hard guideline here because we're not sure one of these approaches is
universally better than the other.

.. _docs-contrib-docs-modules-theory-design:

Design
======
The design content should:

* Focus on `theory of operation <https://en.wikipedia.org/wiki/Theory_of_operation>`_
  or `explanation <https://diataxis.fr/explanation/>`_.

.. _docs-contrib-docs-modules-theory-roadmap:

Roadmap
=======
The roadmap should:

* Focus on things known to be missing today that could make sense in the
  future. The reader should be encouraged to talk to the Pigweed team.

The roadmap should not:

* Make very definite guarantees that a particular feature will ship by a
  certain date. You can get an exception if you really need to do this, but
  it should be avoided in most cases.

.. _docs-contrib-docs-modules-theory-size:

Size analysis
=============
The size analysis should:

* Be auto-generated. See the ``pw_size_diff`` targets in ``//pw_string/BUILD.gn``
  for examples.

The size analysis is elevated to its own section or page because it's a very
important consideration for many embedded developers.
