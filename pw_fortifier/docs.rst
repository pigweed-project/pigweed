.. _module-pw_fortifier:

============
pw_fortifier
============
.. pigweed-module::
   :name: pw_fortifier

**Find and fix your security weaknesses before attackers do!**

``pw_fortifier`` provides a :ref:`framework <module-pw_fortifier-design>` to
build tools that can harness agents and automate several key steps in sustaining
secure software development:

- Find software vulnerabilities in your code, and stale third-party packages in
  your build.
- Filter out false positives and duplicate findings.
- Assess the impact of findings on your product.
- Automatically fix and test vulnerabilities and update dependencies.
- Export results to be published to project dashboards.

-----------
Try it out!
-----------
A sample freshness scanner is included in ``pw_fortifier`` that scans several
types of third party dependencies.

Run it from within your project directory using the
:ref:`command line <module-pw_fortifier-freshness_user-cli>`:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      .. code-block:: console

         # Run a full scan across the entire repository
         $ bazelisk run @pigweed//pw_fortifier/py:demo_freshness_scanner -- -b

         # List all generated issues.
         $ bazelisk run @pigweed//pw_fortifier/py:demo_issue_tracker

         # View a specific generated issue.
         $ bazelisk run @pigweed//pw_fortifier/py:demo_issue_tracker -- \
             --issue 8675309


   .. tab-item:: Python
      :sync: python

      .. code-block:: console

         $ cd path/to/pigweed/pw_fortifier/py

         # Run a full scan across the entire repository
         $ python3 demo_freshness_scanner.py -b

         # List all generated issues.
         $ python3 -m pw_fortifier.demo_issue_tracker

         # View a specific generated issue.
         $ python3 -m pw_fortifier.demo_issue_tracker --issue 8675309

.. note::
   This implementation is meant for demonstration purposes only. It uses a
   simple, filesystem-backed issue tracker instead of a real issue tracker like
   Buganizer. It also will never push CLs to Gerrit, even when asked to.

-------------------------------
Build and run your own scanners
-------------------------------
To create freshness or defect scanning tools for your project, you will need to
supply implementations of several stagesas as described by the implementation
guides:

- :ref:`module-pw_fortifier-defect_impl`
- :ref:`module-pw_fortifier-freshness_impl`

To run scanning tools already created for your project, check the user guides:

- :ref:`module-pw_fortifier-defect_user`
- :ref:`module-pw_fortifier-freshness_user`

.. grid:: 3

   .. grid-item-card:: :octicon:`bug` Defect scanner user guide
      :link: module-pw_fortifier-defect_user
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How to scan for security defects

   .. grid-item-card:: :octicon:`package` Freshness scanner user guide
      :link: module-pw_fortifier-freshness_user
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How to scan for stale third-party packages

   .. grid-item-card:: :octicon:`tools` Other utilities
      :link: module-pw_fortifier-other_utils
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How to use other ``pw_fortifier`` utilities

.. grid:: 3

   .. grid-item-card:: :octicon:`shield-check` Defect scanner implementation guide
      :link: module-pw_fortifier-defect_impl
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How to build a security defect scanner

   .. grid-item-card:: :octicon:`plug` Freshness scanner implementation guide
      :link: module-pw_fortifier-freshness_impl
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      How to build a third-party package scanner

   .. grid-item-card:: :octicon:`workflow` Design
      :link: module-pw_fortifier-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn more about ``pw_fortifier``'s asynchronous pipeline

.. toctree::
   :hidden:
   :maxdepth: 1

   guides/defect_user
   guides/freshness_user
   other_utils
   guides/defect_impl
   guides/freshness_impl
   design
   api
