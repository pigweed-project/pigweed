.. _module-pw_ide-bazel:

=================
pw_ide for Bazel
=================
.. pigweed-module-subpage::
   :name: pw_ide

.. toctree::
   :maxdepth: 1
   :hidden:

   quickstart
   usage
   rust
   configuration
   commands
   troubleshooting

Pigweed provides rich and robust support for development in `Visual Studio Code <https://code.visualstudio.com/>`_,
including:

* High-quality C/C++ and Rust code intelligence for embedded systems projects
  using `clangd <https://clangd.llvm.org/>`_ and
  `rust-analyzer <https://rust-analyzer.github.io/>`_ integrated directly with
  your project's Bazel build graph

* Bundled core Bazel tools, letting you get started immediately without the need
  to install global system dependencies

* Interactive browsing, building, and running build targets

---------------
Getting started
---------------
See the :ref:`Getting Started Guide<module-pw_ide-bazel-quickstart>` to learn how to
install the extension and get code intelligence.



-----------------------
C/C++ code intelligence
-----------------------
Learn more about using C/C++ code intelligence in the :ref:`Usage Guide<module-pw_ide-bazel-usage>`.

----------------------
Rust code intelligence
----------------------
Learn more about configuring and using Rust code intelligence with
`rust-analyzer <https://rust-analyzer.github.io/>`_ in
:ref:`module-pw_ide-bazel-rust`.

----------------
Project settings
----------------
Learn more about configuring the extension in the :ref:`Configuration Guide<module-pw_ide-bazel-configuration>`.

--------
Commands
--------
See the :ref:`Commands Reference<module-pw_ide-bazel-commands>` for a list of available
commands in the Pigweed extension.
