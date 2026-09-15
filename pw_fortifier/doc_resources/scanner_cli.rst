:orphan:

.. list-table::
   :header-rows: 1

   * - Argument
     - Description
     - Associated stages
   * - .. _module-pw_fortifier-scanner_cli-src_repo:

       ``-s``, ``--src-repo PATH``
     - Path to a local read-only Git repository workspace to scan. If omitted,
       the scanner automatically clones the remote ``repo_url`` into
       ``<working-dir>/src``.
     - :ref:`module-pw_fortifier-design-emitter`
       :ref:`module-pw_fortifier-design-analyzer`
   * - .. _module-pw_fortifier-scanner_cli-dst_repo:

       ``-d``, ``--dst-repo PATH``
     - Path to a local writable Git repository workspace where code editors
       apply patches and commit changes. If omitted, the scanner clones
       ``repo_url`` into ``<working-dir>/dst``.
     - :ref:`module-pw_fortifier-design-code_editor`
   * - .. _module-pw_fortifier-scanner_cli-files:

       ``-f``, ``--files [FILES...]``
     - One or more specific file paths or glob patterns to scan instead of
       enumerating the full repository.
     - :ref:`module-pw_fortifier-design-emitter`
   * - .. _module-pw_fortifier-scanner_cli-issues:

       ``-i``, ``--issue ID``
     - Issue ID to process (can be specified multiple times). Providing issue
       IDs disables the ``Emitter`` and analyzer stages; the ``IssueReader``
       fetches the specified issues from the issue tracker and injects them
       directly into the code editor.
     - :ref:`module-pw_fortifier-design-issue_reader`
   * - .. _module-pw_fortifier-scanner_cli-hotlists:

       ``-l``, ``--hotlist ID``
     - Hotlist ID to process (can be specified multiple times). Providing
       hotlist IDs disables file enumeration and analyzer stages, querying the
       issue tracker for all issues on the hotlist and injecting them into the
       code editor.
     - :ref:`module-pw_fortifier-design-issue_reader`
   * - .. _module-pw_fortifier-scanner_cli-create_bugs:

       ``-b``, ``--create-bugs``
     - If enabled, creates Buganizer issues for findings. Otherwise, issue
       details are printed to stdout (defaults to False).
     - :ref:`module-pw_fortifier-design-issue_writer`
   * - .. _module-pw_fortifier-scanner_cli-allow_edits:

       ``-e``, ``--allow-edits``
     - If enabled, creates or updates local Git revisions and runs validation
       builds (defaults to False).
     - :ref:`module-pw_fortifier-design-code_editor`
   * - .. _module-pw_fortifier-scanner_cli-allow_uploads:

       ``-u``, ``--allow-uploads``
     - If enabled, uploads candidate commits to Gerrit as Change Lists (implies
       ``-e`` / ``--allow-edits``; defaults to False).
     - :ref:`module-pw_fortifier-design-code_editor`
   * - .. _module-pw_fortifier-scanner_cli-resume:

       ``-r``, ``--resume``
     - Resumes an interrupted run. Reuses existing intermediate files in stage
       directories (``*_in``, ``*_out``) and picks up enumeration from
       ``last_emitted.txt`` where it stopped, rather than cleaning the working
       directory.
     - All stages.
   * - .. _module-pw_fortifier-scanner_cli-max_retries:

       ``-m``, ``--max-retries COUNT``
     - Maximum number of retries for transient errors (network timeouts,
       subprocess failures) before moving a failed item to the stage's
       ``*_err/`` directory (defaults to 3).
     - All stages.
   * - .. _module-pw_fortifier-scanner_cli-working_dir:

       ``-w``, ``--working-dir PATH``
     - Working directory for intermediate results and stage queues (defaults to
       ``/tmp/<program_name>``). Each stage maintains isolated ``*_in``,
       ``*_out``, and ``*_err`` subdirectories.
     - All stages.
   * - .. _module-pw_fortifier-scanner_cli-output:

       ``-o``, ``--output PATH``
     - Path to write the final results as a CSV file, in addition to printing
       formatted tabular results to stdout.
     - :ref:`module-pw_fortifier-design-collector`
   * - .. _module-pw_fortifier-scanner_cli-verbose:

       ``-v``, ``--verbose``
     - If enabled, prints verbose diagnostic output and detailed issue
       information to stdout (defaults to False).
     - All stages.
