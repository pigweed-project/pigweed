.. _module-pw_ide:

------
pw_ide
------
.. pigweed-module::
   :name: pw_ide

.. toctree::
   :maxdepth: 1
   :hidden:

   Bazel <guide/bazel/index>
   Bootstrap <guide/bootstrap/index>
   Contributing <contributing/index>

Rich, modern IDE and code editor support for embedded systems projects.

Modern software development takes advantage of language servers and advanced
editor features to power experiences like:

* Fast, accurate code navigation, including finding definitions,
  implementations, and references

* Code autocompletion based on a deep understanding of the code structure, not
  just dictionary lookups

* Instant compiler errors and warnings as you write your code, powered by the language server.

Most embedded systems development still lacks these features.
**When you use pw_ide, you get all of them!**

.. grid:: 2

   .. grid-item-card:: :octicon:`code` pw_ide for Bazel
      :link: module-pw_ide-bazel
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to use Visual Studio Code for blazingly fast embedded software
      development for Bazel-based Pigweed projects

   .. grid-item-card:: :octicon:`terminal` pw_ide for Bootstrap
      :link: module-pw_ide-bootstrap
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to use the ``pw_ide`` command-line interface with
      bootstrap-based Pigweed projects using GN or CMake

.. grid:: 1

   .. grid-item-card:: :octicon:`repo` Contributing
      :link: module-pw_ide-contributing
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how to contribute to the Pigweed IDE module and understand its design
