.. _module-pw_bloat:

========
pw_bloat
========
.. pigweed-module::
   :name: pw_bloat

``pw_bloat`` provides tools and helpers around using
`Bloaty McBloatface <https://github.com/google/bloaty>`_ including generating
size report cards for output binaries through :ref:`Pigweed's GN build
system <module-pw_build-gn>`.

Bloat report cards allow tracking the memory usage of a system over time as code
changes are made and provide a breakdown of which parts of the code have the
largest size impact.

------------------------
``pw bloat`` CLI command
------------------------
``pw_bloat`` includes a plugin for the Pigweed command line capable of running
size reports on ELF binaries.

.. note::

   The bloat CLI plugin is still experimental and only supports a small subset
   of ``pw_bloat``'s capabilities.

Basic usage
===========

Running a size report on a single executable
--------------------------------------------
By default, ``pw bloat`` assumes that
:ref:`memoryregions <module-pw_bloat-memoryregions>` symbols are defined in
binaries, and uses them to automatically generate a Bloaty config file.

.. code-block:: sh

   $ pw bloat out/docs/obj/pw_result/size_report/bin/ladder_and_then.elf

   ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
    ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
    ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
    ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
    ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀

   +----------------------+---------+
   |     memoryregions    |  sizes  |
   +======================+=========+
   |FLASH                 |1,048,064|
   |RAM                   |  196,608|
   |VECTOR_TABLE          |      512|
   +======================+=========+
   |Total                 |1,245,184|
   +----------------------+---------+

Running a size report diff
--------------------------

.. code-block:: sh

   $ pw bloat out/docs/obj/pw_metric/size_report/bin/one_metric.elf \
         --diff out/docs/obj/pw_metric/size_report/bin/base.elf \
         -d symbols

   ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
    ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
    ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
    ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
    ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀

   +-----------------------------------------------------------------------------------+
   |                                                                                   |
   +-----------------------------------------------------------------------------------+
   | diff|     memoryregions    |                    symbols                    | sizes|
   +=====+======================+===============================================+======+
   |     |FLASH                 |                                               |    -4|
   |     |                      |[section .FLASH.unused_space]                  |  -408|
   |     |                      |main                                           |   +60|
   |     |                      |__sf_fake_stdout                               |    +4|
   |     |                      |pw_boot_PreStaticMemoryInit                    |    -2|
   |     |                      |_isatty                                        |    -2|
   |  NEW|                      |_GLOBAL__sub_I_group_foo                       |   +84|
   |  NEW|                      |pw::metric::Group::~Group()                    |   +34|
   |  NEW|                      |pw::intrusive_list_impl::List::insert_after()  |   +32|
   |  NEW|                      |pw::metric::Metric::Increment()                |   +32|
   |  NEW|                      |__cxa_atexit                                   |   +28|
   |  NEW|                      |pw::metric::Metric::Metric()                   |   +28|
   |  NEW|                      |pw::metric::Metric::as_int()                   |   +28|
   |  NEW|                      |pw::intrusive_list_impl::List::Item::unlist()  |   +20|
   |  NEW|                      |pw::metric::Group::Group()                     |   +18|
   |  NEW|                      |pw::intrusive_list_impl::List::Item::previous()|   +14|
   |  NEW|                      |pw::metric::TypedMetric<>::~TypedMetric()      |   +14|
   |  NEW|                      |__aeabi_atexit                                 |   +12|
   +-----+----------------------+-----------------------------------------------+------+
   |     |RAM                   |                                               |     0|
   |     |                      |[section .stack]                               |   -32|
   |  NEW|                      |group_foo                                      |   +16|
   |  NEW|                      |metric_x                                       |   +12|
   |  NEW|                      |[section .static_init_ram]                     |    +4|
   +=====+======================+===============================================+======+
   |Total|                      |                                               |    -4|
   +-----+----------------------+-----------------------------------------------+------+

Specifying a custom Bloaty config file
--------------------------------------
If the linker script for a target does not define memory regions, a custom
Bloaty config can be provided using the ``-c / --custom-config`` option.

.. code-block::

   $ pw bloat out/pw_strict_host_clang_debug/obj/pw_status/test/status_test -c targets/host/linux.bloaty

    ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
     ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
     ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
     ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
     ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀

   +------------+---------------------+-------+
   |  segments  |       sections      | sizes |
   +============+=====================+=======+
   |LOAD #3 [RX]|                     |138,176|
   |            |.text                |137,524|
   |            |.plt                 |    608|
   |            |.init                |     24|
   |            |.fini                |     20|
   +------------+---------------------+-------+
   |LOAD #2 [R] |                     | 87,816|
   |            |.rela.dyn            | 32,664|
   |            |.rodata              | 23,176|
   |            |.eh_frame            | 23,152|
   |            |.eh_frame_hdr        |  4,236|
   |            |.gcc_except_table    |  1,140|
   |            |.dynsym              |  1,008|
   |            |.rela.plt            |    888|
   |            |[ELF Program Headers]|    616|
   |            |.dynstr              |    556|
   |            |.gnu.version_r       |    116|
   |            |.gnu.version         |     84|
   |            |[ELF Header]         |     64|
   |            |.note.ABI-tag        |     32|
   |            |.gnu.hash            |     28|
   |            |.interp              |     28|
   |            |.note.gnu.build-id   |     28|
   +------------+---------------------+-------+
   |LOAD #5 [RW]|                     | 20,216|
   |            |.bss                 | 19,824|
   |            |.got.plt             |    328|
   |            |.data                |     64|
   +------------+---------------------+-------+
   |LOAD #4 [RW]|                     | 15,664|
   |            |.data.rel.ro         | 12,240|
   |            |.relro_padding       |  2,872|
   |            |.dynamic             |    464|
   |            |.got                 |     56|
   |            |.fini_array          |     16|
   |            |.init_array          |     16|
   +============+=====================+=======+
   |Total       |                     |261,872|
   +------------+---------------------+-------+

.. _bloat-howto:

------------------------------
Defining size reports in Bazel
------------------------------

Size diff reports
=================
A size diff report compares the symbols present in two binaries and outputs an
RST table snippet detailing any changes.

To define a size diff, first define targets for the two binaries being compared
using the ``pw_cc_size_binary`` rule. This rule accepts the same attributes a
regular ``cc_binary``.

.. code-block:: bazel

   load("//pw_bloat:pw_cc_size_binary.bzl", "pw_cc_size_binary")

   pw_cc_size_binary(
      name = "base_without_foo",
      srcs = ["base_without_foo.cc"],
      deps = [
         "//pw_bloat:bloat_this_binary",
      ],
   )

   pw_cc_size_binary(
      name = "with_foo",
      srcs = ["with_foo.cc"],
      deps = [
         "//pw_bloat:bloat_this_binary",
         "//pw_foo",
      ],
   )

Then, use the ``pw_size_diff`` rule to run the size report on these binaries.
Each size diff report outputs a single row within an RST table. To collect the
rows from one or more reports into a final RST output, list them within a
``pw_size_table`` target.

.. code-block:: bazel

   load("//pw_bloat:pw_size_diff.bzl", "pw_size_diff")
   load("//pw_bloat:pw_size_table.bzl", "pw_size_table")

   pw_size_diff(
      name = "foo_size",
      base = ":base_without_foo",
      label = "Cost of linking in //pw_foo and performing several of its operations",
      target = ":with_foo",
   )

   pw_size_table(
      name = "foo_size_report",
      reports = [
         ":foo_size",
      ],
   )

``pw_size_table`` produces an RST output, which can then be listed in the
``srcs`` of a docs target, and included within one of its own RST files.

.. code-block:: bazel

   load("@sphinxdocs//sphinxdocs:sphinx_docs_library.bzl", "sphinx_docs_library")

   sphinx_docs_library(
      name = "docs",
      srcs = [
         "docs.rst",
         ":foo_size_report",
      ],
      prefix = "pw_foo/",
      target_compatible_with = incompatible_with_mcu(),
   )

Then, within ``docs.rst``, simply include the generated file using the name of
its Bazel target.

.. code-block:: rst

   ``pw_foo`` size report
   ^^^^^^^^^^^^^^^^^^^^^^
   The following table demonstrates the code size of pulling in ``pw_foo``.

   .. include:: foo_size_report

---------------------------
Defining size reports in GN
---------------------------

Size diff reports
=================
Size reports can be defined using the GN template ``pw_size_diff``. The template
requires at least two executable targets on which to perform a size diff. The
base for the size diff can be specified either globally through the top-level
``base`` argument, or individually per-binary within the ``binaries`` list.

Arguments
---------

* ``base``: Optional default base target for all listed binaries.
* ``source_filter``: Optional global regex to filter labels in the diff output.
* ``data_sources``: Optional global list of datasources from bloaty config file
* ``binaries``: List of binaries to size diff. Each binary specifies a target,
  a label for the diff, and optionally a base target, source filter, and data
  sources that override the global ones (if specified).


.. code-block::

   import("$dir_pw_bloat/bloat.gni")

   executable("empty_base") {
     sources = [ "empty_main.cc" ]
   }

   executable("hello_world_printf") {
     sources = [ "hello_printf.cc" ]
   }

   executable("hello_world_iostream") {
     sources = [ "hello_iostream.cc" ]
   }

   pw_size_diff("my_size_report") {
     base = ":empty_base"
     data_sources = "symbols,segments"
     binaries = [
       {
         target = ":hello_world_printf"
         label = "Hello world using printf"
       },
       {
         target = ":hello_world_iostream"
         label = "Hello world using iostream"
         data_sources = "symbols"
       },
     ]
   }

A sample ``pw_size_diff`` reStructuredText size report table can be found
within module docs. For example, see the :ref:`pw_checksum-size-report`
section of the ``pw_checksum`` module for more detail.

Single binary size reports
==========================
Size reports can also be defined using ``pw_size_report``, which provides
a size report for a single binary. The template requires a target binary.

Arguments
---------
* ``target``: Binary target to run size report on.
* ``data_sources``: Optional list of data sources to organize outputs.
* ``source_filter``: Optional regex to filter labels in the output.
* ``json_key_prefix``: Optional prefix for key names in json size report.
* ``full_json_summary``: Optional boolean to print json size report by label
*  level hierarchy. Defaults to only use top-level label in size report.
* ``ignore_unused_labels``: Optional boolean to remove labels that have size of
*  zero in json size report.

.. code-block::

   import("$dir_pw_bloat/bloat.gni")

   executable("hello_world_iostream") {
     sources = [ "hello_iostream.cc" ]
   }

  pw_size_report("hello_world_iostream_size_report") {
    target = ":hello_iostream"
    data_sources = "segments,symbols"
    source_filter = "pw::hello"
    json_key_prefix = "hello_world_iostream"
    full_json_summary = true
    ignore_unused_labels = true
  }

Example of the generated ASCII table for a single binary:

.. code-block::

   ┌─────────────┬──────────────────────────────────────────────────┬──────┐
   │segment_names│                      symbols                     │ sizes│
   ├═════════════┼══════════════════════════════════════════════════┼══════┤
   │FLASH        │                                                  │12,072│
   │             │pw::kvs::KeyValueStore::InitializeMetadata()      │   684│
   │             │pw::kvs::KeyValueStore::Init()                    │   456│
   │             │pw::kvs::internal::EntryCache::Find()             │   444│
   │             │pw::kvs::FakeFlashMemory::Write()                 │   240│
   │             │pw::kvs::internal::Entry::VerifyChecksumInFlash() │   228│
   │             │pw::kvs::KeyValueStore::GarbageCollectSector()    │   220│
   │             │pw::kvs::KeyValueStore::RemoveDeletedKeyEntries() │   220│
   │             │pw::kvs::KeyValueStore::AppendEntry()             │   204│
   │             │pw::kvs::KeyValueStore::Get()                     │   194│
   │             │pw::kvs::internal::Entry::Read()                  │   188│
   │             │pw::kvs::ChecksumAlgorithm::Finish()              │    26│
   │             │pw::kvs::internal::Entry::ReadKey()               │    26│
   │             │pw::kvs::internal::Sectors::BaseAddress()         │    24│
   │             │pw::kvs::ChecksumAlgorithm::Update()              │    20│
   │             │pw::kvs::FlashTestPartition()                     │     8│
   │             │pw::kvs::FakeFlashMemory::Disable()               │     6│
   │             │pw::kvs::FakeFlashMemory::Enable()                │     6│
   │             │pw::kvs::FlashMemory::SelfTest()                  │     6│
   │             │pw::kvs::FlashPartition::Init()                   │     6│
   │             │pw::kvs::FlashPartition::sector_size_bytes()      │     6│
   │             │pw::kvs::FakeFlashMemory::IsEnabled()             │     4│
   ├─────────────┼──────────────────────────────────────────────────┼──────┤
   │RAM          │                                                  │ 1,424│
   │             │test_kvs                                          │   992│
   │             │pw::kvs::(anonymous namespace)::test_flash        │   384│
   │             │pw::kvs::(anonymous namespace)::test_partition    │    24│
   │             │pw::kvs::FakeFlashMemory::no_errors_              │    12│
   │             │borrowable_kvs                                    │     8│
   │             │kvs_entry_count                                   │     4│
   ├═════════════┼══════════════════════════════════════════════════┼══════┤
   │Total        │                                                  │13,496│
   └─────────────┴──────────────────────────────────────────────────┴──────┘


Size reports are typically included in reStructuredText, as described in
`Documentation integration`_. Size reports may also be printed in the build
output if desired. To enable this in the GN build
(``pigweed/pw_bloat/bloat.gni``), set the ``pw_bloat_SHOW_SIZE_REPORTS``
build arg to ``true``.

Collecting size report data
===========================
Each ``pw_size_report`` target outputs a JSON file containing the sizes of all
top-level labels in the binary. (By default, this represents "segments", i.e.
ELF program headers.) If ``full_json_summary`` is set to true, sizes for all
label levels are reported (i.e. default labels would show size of each symbol
per segment). If a build produces multiple images, it may be useful to collect
all of their sizes into a single file to provide a snapshot of sizes at some
point in time --- for example, to display per-commit size deltas through CI.

The ``pw_size_report_aggregation`` template is provided to collect multiple size
reports' data into a single JSON file.

Arguments
---------
* ``deps``: List of ``pw_size_report`` targets whose data to collect.
* ``output``: Path to the output JSON file.

.. code-block::

   import("$dir_pw_bloat/bloat.gni")

   pw_size_report_aggregation("image_sizes") {
      deps = [
        ":app_image_size_report",
        ":bootloader_image_size_report",
      ]
      output = "$root_gen_dir/artifacts/image_sizes.json"
   }

.. _module-pw_bloat-docs:

-------------------------
Documentation integration
-------------------------
Bloat reports are easy to add to documentation files. All ``pw_size_diff``
and ``pw_size_report`` targets output a file containing a tabular report card.
This file can be imported directly into a reStructuredText file using the
``include`` directive.

For example, the ``simple_bloat_loop`` and ``simple_bloat_function`` size
reports under ``//pw_bloat/examples`` are imported into this file as follows:

.. code-block:: rst

   Simple bloat loop example
   ^^^^^^^^^^^^^^^^^^^^^^^^^
   .. include:: examples/simple_bloat_loop

   Simple bloat function example
   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   .. include:: examples/simple_bloat_function

Resulting in this output:

Simple bloat loop example
=========================
.. include:: examples/simple_bloat_loop

Simple bloat function example
=============================
.. include:: examples/simple_bloat_function

.. _module-pw_bloat-sources:

------------------------------
Additional Bloaty data sources
------------------------------
`Bloaty McBloatface <https://github.com/google/bloaty>`_ by itself cannot help
answer some questions which embedded developers frequently face such as
understanding how much space is left. To address this, Pigweed provides Python
tooling (``pw_bloat.bloaty_config``) to generate bloaty configuration files
based on the final ELF files through small tweaks in the linker scripts to
expose extra information.

See the sections below on how to enable the additional data sections through
modifications in your linker script(s).

As an example to generate the helper configuration which enables additional data
sources for ``example.elf`` if you've updated your linker script(s) accordingly,
simply run
``python -m pw_bloaty.bloaty_config example.elf > example.bloaty``. The
``example.bloaty``  can then be used with bloaty using the ``-c`` flag, for
example
``bloaty -c example.bloaty example.elf --domain vm -d memoryregions,utilization``
which may return something like:

.. code-block::

   84.2%  1023Ki    FLASH
     94.2%   963Ki    Free space
      5.8%  59.6Ki    Used space
   15.8%   192Ki    RAM
    100.0%   192Ki    Used space
    0.0%     512    VECTOR_TABLE
     96.9%     496    Free space
      3.1%      16    Used space
    0.0%       0    Not resident in memory
      NAN%       0    Used space

.. _module-pw_bloat-utilization:

``utilization`` data source
===========================
The most common question many embedded developers face when using ``bloaty`` is
how much space you are using and how much space is left. To correctly answer
this, section sizes must be used in order to correctly account for section
alignment requirements.

The generated ``utilization`` data source will work with any ELF file, where
``Used Space`` is reported for the sum of virtual memory size of all sections.
``Padding`` captures the amount of memory that is utilized to enfore alignment
requirements. Tracking ``Padding`` size can help monitor application growth
for changes that are too small to force realignment.

In order for ``Free Space`` to be reported, your linker scripts must include
properly aligned sections which span the unused remaining space for the relevant
memory region with the ``unused_space`` string anywhere in their name. This
typically means creating a trailing section which is pinned to span to the end
of the memory region.

For example imagine this partial example GNU LD linker script:

.. code-block::

   MEMORY
   {
     FLASH(rx) : \
       ORIGIN = PW_BOOT_FLASH_BEGIN, \
       LENGTH = PW_BOOT_FLASH_SIZE
     RAM(rwx) : \
       ORIGIN = PW_BOOT_RAM_BEGIN, \
       LENGTH = PW_BOOT_RAM_SIZE
   }

   SECTIONS
   {
     /* Main executable code. */
     .code : ALIGN(4)
     {
       /* Application code. */
       *(.text)
       *(.text*)
       KEEP(*(.init))
       KEEP(*(.fini))

       . = ALIGN(4);
       /* Constants.*/
       *(.rodata)
       *(.rodata*)
     } >FLASH

     /* Explicitly initialized global and static data. (.data)*/
     .static_init_ram : ALIGN(4)
     {
       *(.data)
       *(.data*)
       . = ALIGN(4);
     } >RAM AT> FLASH

     /* Zero initialized global/static data. (.bss) */
     .zero_init_ram (NOLOAD) : ALIGN(4)
     {
       *(.bss)
       *(.bss*)
       *(COMMON)
       . = ALIGN(4);
     } >RAM
   }

Could be modified as follows to enable ``Free Space`` reporting:

.. code-block::

   MEMORY
   {
     FLASH(rx) : ORIGIN = PW_BOOT_FLASH_BEGIN, LENGTH = PW_BOOT_FLASH_SIZE
     RAM(rwx) : ORIGIN = PW_BOOT_RAM_BEGIN, LENGTH = PW_BOOT_RAM_SIZE

     /* Each memory region above has an associated .*.unused_space section that
      * overlays the unused space at the end of the memory segment. These
      * segments are used by pw_bloat.bloaty_config to create the utilization
      * data source for bloaty size reports.
      *
      * These sections MUST be located immediately after the last section that is
      * placed in the respective memory region or lld will issue a warning like:
      *
      *   warning: ignoring memory region assignment for non-allocatable section
      *      '.VECTOR_TABLE.unused_space'
      *
      * If this warning occurs, it's also likely that LLD will have created quite
      * large padded regions in the ELF file due to bad cursor operations. This
      * can cause ELF files to balloon from hundreds of kilobytes to hundreds of
      * megabytes.
      *
      * Attempting to add sections to the memory region AFTER the unused_space
      * section will cause the region to overflow.
      */
   }

   SECTIONS
   {
     /* Main executable code. */
     .code : ALIGN(4)
     {
       /* Application code. */
       *(.text)
       *(.text*)
       KEEP(*(.init))
       KEEP(*(.fini))

       . = ALIGN(4);
       /* Constants.*/
       *(.rodata)
       *(.rodata*)
     } >FLASH

     /* Explicitly initialized global and static data. (.data)*/
     .static_init_ram : ALIGN(4)
     {
       *(.data)
       *(.data*)
       . = ALIGN(4);
     } >RAM AT> FLASH

     /* Defines a section representing the unused space in the FLASH segment.
      * This MUST be the last section assigned to the FLASH region.
      */
     PW_BLOAT_UNUSED_SPACE(FLASH)

     /* Zero initialized global/static data. (.bss). */
     .zero_init_ram (NOLOAD) : ALIGN(4)
     {
       *(.bss)
       *(.bss*)
       *(COMMON)
       . = ALIGN(4);
     } >RAM

     /* Defines a section representing the unused space in the RAM segment. This
      * MUST be the last section assigned to the RAM region.
      */
     PW_BLOAT_UNUSED_SPACE(RAM)
   }

The preprocessor macro ``PW_BLOAT_UNUSED_SPACE`` is defined in
``pw_bloat/bloat_macros.ld``. To use these macros include this file in your
``pw_linker_script`` as follows:

.. code-block::

   pw_linker_script("my_linker_script") {
     includes = [ "$dir_pw_bloat/bloat_macros.ld" ]
     linker_script = "my_project_linker_script.ld"
   }

Note that linker scripts are not natively supported by GN and can't be provided
through ``deps``, the ``bloat_macros.ld`` must be passed in the ``includes``
list.

.. _module-pw_bloat-memoryregions:

``memoryregions`` data source
=============================
Understanding how symbols, sections, and other data sources can be attributed
back to the memory regions defined in your linker script is another common
problem area. Unfortunately the ELF format does not include the original memory
regions, meaning ``bloaty`` can not do this today by itself. In addition, it's
relatively common that there are multiple memory regions which alias to the same
memory but through different buses which could make attribution difficult.

Instead of taking the less portable and brittle approach to parse ``*.map``
files, ``pw_bloat.bloaty_config`` consumes symbols which are defined in the
linker script with a special format to extract this information from the ELF
file: ``pw_bloat_config_memory_region_NAME_{start,end}{_N,}``.

These symbols are defined by the preprocessor macros ``PW_BLOAT_MEMORY_REGION``
and ``PW_BLOAT_MEMORY_REGION_MAP`` with the right address and size for the
regions. To use these macros include the ``pw_bloat/bloat_macros.ld`` in your
``pw_linker_script`` as follows:

.. code-block::

   pw_linker_script("my_linker_script") {
     includes = [ "$dir_pw_bloat/bloat_macros.ld" ]
     linker_script = "my_project_linker_script.ld"
   }

These symbols are then used to determine how to map segments to these memory
regions. Note that segments must be used in order to account for inter-section
padding which are not attributed against any sections.

As an example, if you have a single view in the single memory region named
``FLASH``, then you should include the following macro in your linker script to
generate the symbols needed for the that region:

.. code-block::

   PW_BLOAT_MEMORY_REGION(FLASH)

As another example, if you have two aliased memory regions (``DCTM`` and
``ITCM``) into the same effective memory named you'd like to call ``RAM``, then
you should produce the following four symbols in your linker script:

.. code-block::

   PW_BLOAT_MEMORY_REGION_MAP(RAM, ITCM)
   PW_BLOAT_MEMORY_REGION_MAP(RAM, DTCM)

------------------------------------------
Preventing unwanted compiler optimizations
------------------------------------------
Compilers can often be very effective at detecting and removing code that does
not have an observable effect. For example, if attempting to measure two methods
that are the inverse of each other, some compilers may recognize the object they
are being called on will always return to its original state and remove both
calls. This is desirable when trying to create small and fast code, but not when
the goal is to actually measure the code size impact of those methods.

To help counteract these unwanted optimizations, this module provides a few
macros that can conditionally execute statements depending on the value of a
``mask`` variable. Size report authors can use a ``volatile`` variable to ensure
the compiler cannot assume which calls will or will not be executed,

-------------
API reference
-------------
Moved: :cc:`pw_bloat`
