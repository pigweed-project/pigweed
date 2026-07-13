.. _module-pw_build-project_builder:

===============
Project Builder
===============
.. pigweed-module-subpage::
   :name: pw_build

The ``pw_build`` Python module contains a light-weight build command execution
library used for projects that require running multiple commands to perform a
build. For example: running ``cmake`` alongside ``gn``.

.. grid:: 3

   .. grid-item-card:: :octicon:`rocket` Get started
      :link: module-pw_build-project_builder-start
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      How to write your own build script.

   .. grid-item-card:: :octicon:`code-square` Reference
      :link: module-pw_build-project_builder-reference
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Reference details about the ``pw_build`` Python API.

   .. grid-item-card:: :octicon:`terminal` ``pw build`` CLI
      :link: module-pw_build-project_builder-cli
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Command line interface usage.

.. _module-pw_build-project_builder-start:

-----------
Get Started
-----------
The quickest way to get started with Project Builder is to create a build script
based on existing examples.

Example Build Scripts
=====================
Examples of Project Builder based ``pw build`` commands:

- :cs:`Upstream Pigweed repo __main__.py <main:pw_build/py/pigweed_upstream_build/__main__.py>`
- `Examples repo build_project.py <https://cs.opensource.google/pigweed/examples/+/main:tools/sample_project_tools/build_project.py>`_
- `Kudzu repo build_project.py <https://pigweed.googlesource.com/pigweed/kudzu/+/refs/heads/main/tools/kudzu_tools/build_project.py>`_

.. _module-pw_build-project_builder-reference:

---------
Reference
---------
At a high level:

- A single :py:class:`BuildRecipe <pw_build.build_recipe.BuildRecipe>`
  contains many :py:class:`BuildCommands <pw_build.build_recipe.BuildCommand>`
  which are run sequentially.
- All :py:class:`BuildRecipe <pw_build.build_recipe.BuildRecipe>` names must be
  unique and can have dependencies on other recipes.
- Multiple :py:class:`BuildRecipes <pw_build.build_recipe.BuildRecipe>` are
  passed into the :py:class:`ProjectBuilder <pw_build.project_builder.ProjectBuilder>`
  class which provides options for logging and terminal output.
- Calling the :py:func:`run_builds <pw_build.project_builder.ProjectBuilder.run_builds>`
  function starts executing commands. This function allows specifying the number
  of parallel workers (the number of recipes which are executed in parallel).

.. raw:: html

   <style>
     #arch > svg { height: 350px; }
   </style>

.. mermaid::
   :name: arch

   flowchart TB
       subgraph BuildRecipeA ["<strong>BuildRecipe</strong>: ninja"]
           buildCommandA1["<strong>BuildCommand</strong><br> gn gen out"]
           buildCommandA2["<strong>BuildCommand</strong><br> ninja -C out default"]
           buildCommandA1-->buildCommandA2
       end

       subgraph BuildRecipeB ["<strong>BuildRecipe</strong>: bazel"]
           buildCommandB1["<strong>BuildCommand</strong><br> bazel build //...:all"]
           buildCommandB2["<strong>BuildCommand</strong><br> bazel test //...:all"]
           buildCommandB1-->buildCommandB2
       end

       ProjectBuilder["builder = <strong>ProjectBuilder</strong>(build_recipes=...)"]
       BuildRecipeA-->ProjectBuilder
       BuildRecipeB-->ProjectBuilder

       run_builds["builder.<strong>run_builds</strong>(workers=1)"]
       ProjectBuilder-->run_builds

BuildCommand
============
.. autoclass:: pw_build.build_recipe.BuildCommand

BuildCommand Run Filters
------------------------
.. autofunction:: pw_build.build_recipe.should_gn_gen

.. autofunction:: pw_build.build_recipe.should_gn_gen_with_args

.. autofunction:: pw_build.build_recipe.should_regenerate_cmake

BuildRecipe
===========
.. autoclass:: pw_build.build_recipe.BuildRecipe

ProjectBuilder
==============
.. autoclass:: pw_build.project_builder.ProjectBuilder


.. _module-pw_build-project_builder-cli:

--------------------------------------------------
Upstream ``pw build`` Command-Line Interface Usage
--------------------------------------------------
.. argparse::
   :module: pigweed_upstream_build.__main__
   :func: _build_argument_parser
   :prog: pw build

   This is the command line interface provided by the ``pw build`` command
   in upstream Pigweed:
   :cs:`__main__.py <main:pw_build/py/pigweed_upstream_build/__main__.py>`
