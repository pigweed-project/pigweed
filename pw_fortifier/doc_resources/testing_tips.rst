Execution flags and permissions
===============================
Scanners operate with safe defaults that do not modify remote trackers or
repositories unless explicitly permitted via command-line flags:

- Findings are reported locally and not filed in Buganizer unless
  :ref:`-b,--create-bugs <module-pw_fortifier-scanner_cli-create_bugs>` is
  specified.
- Code modifications and validation builds are skipped unless
  :ref:`-e,--allow-edits <module-pw_fortifier-scanner_cli-allow_edits>` is
  specified.
- Local Git changes are kept locally and not uploaded to Gerrit unless
  :ref:`-u,--allow-uploads <module-pw_fortifier-scanner_cli-allow_uploads>` is
  specified.

Run resumption
==============
If an invocation of the scanner is interrupted, the in-flight inputs will be
saved as files under stage-specific subdirectories of the
:ref:`working directory <module-pw_fortifier-scanner_cli-working_dir>`.
Implementers may examine these files to debug assertions and exceptions. They
may also resume an interrupted run via the
:ref:`-r,--resume <module-pw_fortifier-scanner_cli-resume>` command line
argument.

Retry configuration
===================
By default,
:py:class:`consumers <pw_fortifier.pipeline_stage.PipelineConsumerMixin>` will
try to process each input, and retry on request or subprocess failure up to the
maximum number of times specified via the
:ref:`-m,--max-retries <module-pw_fortifier-scanner_cli-max_retries>` command
line argument. When the maximum number of retries has been reached, the input
will be moved to a stage-specific "error" subdirectory of the
:ref:`working directory <module-pw_fortifier-scanner_cli-working_dir>`, e.g.
"triager_err". By setting the maximum number of retries to zero, i.e. ``-m 0``,
implementers can cause the scanner pipeline to fail fast and then inspect the
offending inputs to debug.

Input preservation
==================
Each pipeline
:py:class:`consumer <pw_fortifier.pipeline_stage.PipelineConsumerMixin>` has a
``_preserve_inputs`` attribute.

- When ``_preserve_inputs=False`` (the default for most intermediate stages),
  the consumer moves incoming files from the upstream stage's output directory
  into its own ``<stage>_in/`` directory and deletes (unlinks) them once
  processing succeeds.
- When ``_preserve_inputs=True``, the consumer processes input files directly in
  place in the upstream directory without moving or deleting them.

This attribute is enabled by default on initial analysis stages (such as
:py:class:`~pw_fortifier.code_analyzer.CodeAnalyzer` and
:py:class:`~pw_fortifier.package_analyzer.PackageAnalyzer`) so that source
repository files are never moved or deleted.

Stage implementers can also enable ``_preserve_inputs=True`` on their stage and
the subsequent stage to debug transformations: the inputs to the stage remain in
``<prev_stage>_out/`` and the outputs remain in ``<stage>_out/``, allowing
direct comparison of the input and output state.
