.. _module-pw_fortifier-defect_user:

==============================
User guide for defect scanners
==============================
Tools based on :py:class:`~pw_fortifier.defect_scanner.DefectScanner` can
be used to:

- Discover software defects with security implications, i.e. software
  vulnerabilities.
- Verify each vulnerability is reachable and impacts security.
- Triage the severity of each vulnerability based on its likelihood and impact.
- File an issue for each vulnerability in a secure Buganizer component.
- Attempt to create a "proof of concept" (PoC) that demonstrates the
  vulnerability.
- Attempt to create a fix for the vulnerability.

For more detail on how such tools can be implemented, see the
:ref:`module-pw_fortifier-defect_impl`.

-----------------
Example workflows
-----------------
Users run the defect scanner from the
:ref:`command line <module-pw_fortifier-defect_user-cli>`:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      .. code-block:: console

         # Run a full scan across the entire repository
         $ bazelisk run //path/to:my_defect_scanner

         # Scan specific source files and print findings
         $ bazelisk run //path/to:my_defect_scanner -- \
             -s /path/to/my/project \
             -f "pw_status/**" "pw_string/**"

         # Scan, create bug reports, and upload candidate fixes
         $ bazelisk run //path/to:my_defect_scanner -- \
             -s /path/to/my/project -b -u

         # Re-generate fixes for an existing bug ID
         $ bazelisk run //path/to:my_defect_scanner -- \
             -s /path/to/my/project \
             -w /path/to/working/dir -i 123456789 -u

   .. tab-item:: Python
      :sync: python

      .. code-block:: console

         # Run a full scan across the entire repository
         $ python3 my_defect_scanner.py

         # Scan specific source files and print findings
         $ python3 my_defect_scanner.py -s /path/to/my/project \
             -f "pw_status/**" "pw_string/**"

         # Scan, create bug reports, and upload candidate fixes
         $ python3 my_defect_scanner.py -s /path/to/my/project -b -u

         # Re-generate fixes for an existing bug ID
         $ python3 my_defect_scanner.py -s /path/to/my/project \
             -w /path/to/working/dir -i 123456789 -u

-------------
Sample output
-------------
To be added soon...

.. TODO(b/553617506): Add sample output here.

.. _module-pw_fortifier-defect_user-cli:

--------------------------------
Command-line arguments reference
--------------------------------
Since :py:class:`~pw_fortifier.defect_scanner.DefectScanner` is derived
from :py:class:`~pw_fortifier.scanner.Scanner`, it automatically provides a
uniform set of command-line arguments:

.. include:: ../doc_resources/scanner_cli.rst
