.. _module-pw_fortifier-freshness_user:

=================================
User guide for freshness scanners
=================================
Tools based on :py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner` can
be used to:

- Discover third-party packages referenced by a project's build files.
- Examine upstream releases for each package to find available versions.
- Determine the earliest valid version of each package according to the
  `freshness policy <http://go/pigweed-3p-freshness-policy>`.
- File Buganizer issues for stale packages.
- Attempt to create Gerrit CLs for rolling packages to valid versions.

For more detail on how such tools can be implemented, see the
:ref:`module-pw_fortifier-freshness_impl`.

-----------------
Example workflows
-----------------
Users run the freshness scanner from the
:ref:`command line <module-pw_fortifier-freshness_user-cli>`:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      .. code-block:: console

         # Run a full scan across the entire repository
         $ bazelisk run //path/to:my_freshness_scanner

         # Scan specific dependency files and output a CSV report
         $ bazelisk run //path/to:my_freshness_scanner -- \
             -s /path/to/my/project \
             -f "**/MODULE.bazel" "**/Cargo.toml" -o freshness.csv

         # Scan, create bug reports, and upload candidate rolls
         $ bazelisk run //path/to:my_freshness_scanner -- \
             -s /path/to/my/project -b -u

         # Re-generate roll CLs for an existing tracking bug ID
         $ bazelisk run //path/to:my_freshness_scanner -- \
             -s /path/to/my/project \
             -w /path/to/working/dir -i 123456789 -u

         # Process all out-of-date dependencies from a Buganizer hotlist
         $ bazelisk run //path/to:my_freshness_scanner -- \
             -s /path/to/my/project \
             -w /path/to/working/dir -l 987654321 -u

   .. tab-item:: Python
      :sync: python

      .. code-block:: console

         # Run a full scan across the entire repository
         $ python3 my_freshness_scanner.py

         # Scan specific dependency files and output a CSV report
         $ python3 my_freshness_scanner.py -s /path/to/my/project \
             -f "**/MODULE.bazel" "**/Cargo.toml" -o freshness.csv

         # Scan, create bug reports, and upload candidate rolls
         $ python3 my_freshness_scanner.py -s /path/to/my/project -b -u

         # Re-generate roll CLs for an existing tracking bug ID
         $ python3 my_freshness_scanner.py -s /path/to/my/project \
             -w /path/to/working/dir -i 123456789 -u

         # Process all out-of-date dependencies from a Buganizer hotlist
         $ python3 my_freshness_scanner.py -s /path/to/my/project \
             -w /path/to/working/dir -l 987654321 -u

-------------
Sample output
-------------
.. code-block:: console

   $ bazelisk run //pw_fortifier/py:my_freshness_scanner
   INFO: Analyzed target //pw_fortifier/py:my_freshness_scanner (1 packages loaded, 41 targets configured).
   INFO: Found 1 target...
   Target //pw_fortifier/py:my_freshness_scanner up-to-date:
     bazel-bin/pw_fortifier/py/my_freshness_scanner
   INFO: Elapsed time: 0.489s, Critical Path: 0.24s
   INFO: 8 processes: 8 internal.
   INFO: Build completed successfully, 8 total actions
   INFO: Running command line: bazel-bin/pw_fortifier/py/my_freshness_scanner
   | package                        | freshness | current          | earliest         | source                            |
   +--------------------------------+-----------+------------------+------------------+-----------------------------------+
   | anyhow                         | 100       | 1.0.104          | 1.0.104          | ...rates_io/crates_std/Cargo.toml |
   | @protobuf-ts/protoc            | -462      | 2.9.4            | 2.11.1           | pw_web/package.json               |
   | bitfield-struct                | 100       | 0.13.0           | 0.13.0           | ...rates_io/crates_std/Cargo.toml |
   | buffer                         | 100       | 6.0.3            | 6.0.3            | pw_web/package.json               |
   | bitflags                       | 100       | 2.13.1           | 2.13.1           | ...rates_io/crates_std/Cargo.toml |
   | byteorder                      | 100       | 1.5.0            | 1.5.0            | ...rates_io/crates_std/Cargo.toml |
   | google-protobuf                | -1722     | 3.17.3           | 4.0.2            | pw_web/package.json               |
   | sphinx-copybutton              | -150      | 0.5.1            | 0.5.2            | ...weed_upstream_requirements.txt |
   | breathe                        | -861      | 4.35.0           | 5.0.0a2          | ...weed_upstream_requirements.txt |
   | pydata-sphinx-theme            | 100       | 0.18.0           | 0.18.0           | ...weed_upstream_requirements.txt |
   | pyparsing                      | 100       | 3.3.2            | 3.3.2            | ...weed_upstream_requirements.txt |
   | clap                           | 100       | 4.6.6            | 4.6.6            | ...rates_io/crates_std/Cargo.toml |
   | long                           | -896      | 5.2.1            | 5.3.2            | pw_web/package.json               |
   | sphinx-reredirects             | -780      | 0.1.3            | 1.1.0            | ...weed_upstream_requirements.txt |
   | pyserial                       | 100       | 3.5              | 3.5              | .../stm32f429i_disc1/py/setup.cfg |
   | cliclack                       | 100       | 0.5.6            | 0.5.6            | ...rates_io/crates_std/Cargo.toml |
   | prompt-toolkit                 | -995      | 3.0.34           | 3.0.52           | pw_watch/py/setup.cfg             |
   | watchdog                       | -1277     | 2.1.0            | 6.0.0            | pw_watch/py/setup.cfg             |
   | debugpy                        | -542      | 1.8.5            | 1.8.20           | pw_system/py/setup.cfg            |
   | papaparse                      | -788      | 5.4.1            | 5.5.3            | pw_web/package.json               |
   | futures                        | 100       | 0.3.34           | 0.3.34           | ...rates_io/crates_std/Cargo.toml |
   | pykwalify                      | 100       | 1.8.0            | 1.8.0            | pw_sensor/py/setup.cfg            |
   | mypy-protobuf                  | 100       | 3.7.0            | 3.7.0            | pw_protobuf_compiler/py/setup.cfg |
   | hex                            | 100       | 0.4.3            | 0.4.3            | ...rates_io/crates_std/Cargo.toml |
   | protobuf                       | -48       | 6.33.5           | 6.33.6           | pw_protobuf_compiler/py/setup.cfg |
   | types-protobuf                 | -73       | 6.32.1.20251210  | 6.32.1.20260221  | pw_protobuf_compiler/py/setup.cfg |
   ...

.. _module-pw_fortifier-freshness_user-cli:

--------------------------------
Command-line arguments reference
--------------------------------
Since :py:class:`~pw_fortifier.freshness_scanner.FreshnessScanner` is derived
from :py:class:`~pw_fortifier.scanner.Scanner`, it automatically provides a
uniform set of command-line arguments:

.. include:: ../doc_resources/scanner_cli.rst
