.. _docs-pw-style-go:

==============
Go style guide
==============
Pigweed's Go style guide extends the `Google Go Style Guide
<https://google.github.io/styleguide/go/>`_ (`Style Decisions
<https://google.github.io/styleguide/go/decisions>`_ and `Best Practices
<https://google.github.io/styleguide/go/best-practices>`_). Where the two
conflict, this guide wins.

All Pigweed Go code is formatted with ``gofmt`` via ``./pw format`` and targets
the Go version declared in the root ``go.mod`` (Go 1.24+).

.. _docs-pw-style-go-generics:

------------------------------------------
Standard library generics and modern Go
------------------------------------------
Use the standard library's generic packages (``slices``, ``maps``, ``cmp``) and
modern language features instead of writing open-coded loops or boilerplate
helpers:

* **Slice and map operations**: Use ``slices.Contains``, ``slices.IndexFunc``,
  ``slices.SortFunc``, ``slices.Compact``, ``slices.Clone``, and ``maps.Clone`` /
  ``maps.Keys`` instead of hand-written search, deduplication, or copy loops.
* **Comparisons and defaults**: Use ``cmp.Or(a, b, fallback)`` for fallback chains
  and ``cmp.Compare(a, b)`` inside ``slices.SortFunc`` comparators.
* **Built-in min/max and integer ranges**: Use the built-in ``min`` and ``max``
  functions and ``for i := range n`` integer loops.
* **Generic helpers**: When a data transformation or set operation (such as
  order-preserving deduplication) applies to multiple types, write a small
  generic function constrained by ``comparable`` or ``cmp.Ordered`` rather than
  duplicating type-specific helpers.

.. admonition:: **Yes**: Standard library ``slices`` and ``cmp``
   :class: checkmark

   .. code-block:: go

      slices.SortFunc(items, func(a, b Item) int {
          return cmp.Or(
              cmp.Compare(b.Priority, a.Priority),
              cmp.Compare(a.Name, b.Name),
          )
      })

.. admonition:: **No**: Open-coded ``sort.Slice`` or nested ``if/else`` comparators
   :class: error

   .. code-block:: go

      sort.Slice(items, func(i, j int) bool {
          if items[i].Priority != items[j].Priority {
              return items[i].Priority > items[j].Priority
          }
          return items[i].Name < items[j].Name
      })

.. _docs-pw-style-go-error-handling:

------------------------------------
Error handling and no silent failure
------------------------------------
Pigweed's :ref:`docs-code_reviews-checklist-no-silent-failures` rule applies
strictly to Go code:

* **Never swallow errors**: Do not write ``if err != nil { return nil }``,
  ``_ = fn()``, or empty error branches that hide failed RPCs, config parse
  errors, or subprocess failures.
* **Explicit best-effort contracts**: Returning ``nil`` on an error is allowed
  *only* when the function's documented contract is explicitly best-effort
  (named ``Try*`` or ``BestEffort*``), and the branch includes both a
  ``// OK: <reason>`` comment and a debug log (``slog.Debug("...", "err", err)``)
  or structured warning.
* **Wrap errors with** ``%w``: Always wrap underlying errors with
  ``fmt.Errorf("context: %w", err)`` (or ``errors.Join``) so callers can inspect
  the cause programmatically.
* **Inspect errors structurally**: Use ``errors.Is`` and ``errors.As`` (or HTTP
  ``resp.StatusCode``) to classify errors. Do not match raw error strings with
  ``strings.Contains(err.Error(), ...)`` when a typed error or status code is
  available.

.. admonition:: **Yes**: Propagate and wrap errors with ``%w``
   :class: checkmark

   .. code-block:: go

      cfg, err := LoadConfig(path)
      if err != nil {
          return fmt.Errorf("loading config: %w", err)
      }

.. admonition:: **No**: Silently discarding an error and returning ``nil``
   :class: error

   .. code-block:: go

      cfg, err := LoadConfig(path)
      if err != nil {
          return nil
      }

.. _docs-pw-style-go-cli-state:

---------------------------------------------
CLI command architecture and state isolation
---------------------------------------------
Go CLI tools in Pigweed must remain hermetic and safe to test in parallel
without process-wide state leaks:

* **Always use** ``RunE``: Every Cobra command that performs an action must set
  ``RunE: func(cmd *cobra.Command, args []string) error`` and never void ``Run``.
* **No** ``os.Exit`` **outside** ``main``: ``os.Exit`` is permitted only in
  ``cmd/main.go``. Library and subcommand functions must return ``error`` (using
  typed exit-code errors such as ``ExitError`` when a specific exit status is
  required) so ``defer`` statements run and unit tests can execute in-process.
* **Injected I/O streams**: Write all standard output to ``cmd.OutOrStdout()``
  and all diagnostics/warnings to ``cmd.ErrOrStderr()``. Never call
  ``fmt.Print``, ``fmt.Println``, or ``fmt.Printf``, or write directly to
  ``os.Stdout`` / ``os.Stderr`` in non-``main`` packages.
* **Avoid package-level mutable flag globals**: Bind flags to a per-command
  options struct inside a constructor function (``newFooCommand()``) rather than
  declaring package-level mutable ``var fooFlag string`` globals that leak
  across test cases.

.. _docs-pw-style-go-concurrency:

-------------------------------------------
Context propagation and bounded resources
-------------------------------------------
* **Plumb** ``context.Context`` **first**: Every function that performs network
  RPCs, spawns subprocesses (``exec.CommandContext``), or sleeps must accept
  ``ctx context.Context`` as its first parameter (derived from
  ``cmd.Context()``). Never introduce ``context.Background()`` mid-call-stack.
* **Bound all response reads**: Wrap every ``io.ReadAll(resp.Body)`` on external
  HTTP or subprocess streams in ``io.LimitReader(resp.Body, maxBytes)`` with a
  named constant so a large log or diff cannot exhaust host memory.
* **Bound concurrent fan-out**: Any goroutine fan-out over changes, files,
  worktrees, or builds must bound concurrency with a semaphore channel or
  worker pool (typically 4--8 concurrent operations) rather than spawning one
  unbounded goroutine per item.

.. _docs-pw-style-go-testing:

-------
Testing
-------
* **Modern** ``testing.T`` **helpers**: Use ``t.Context()`` for test contexts,
  ``t.TempDir()`` for temporary directories, ``t.Setenv()`` for environment
  variables, and ``t.Cleanup()`` to restore any mutated hooks.
* **Table-driven tests and** ``cmp.Diff``: Prefer table-driven subtests
  (``t.Run``) and compare structured values with
  ``cmp.Diff(want, got)`` from ``github.com/google/go-cmp/cmp``.
* **Realistic mocks**: When mocking external services (such as Gerrit or
  Buildbucket), verify that the request includes the required query options or
  parameters in addition to returning canned response payloads.
