# C/C++ API reference (Doxygen)

How to contribute to Pigweed's Doxygen-based C/C++ API reference.

## Architecture

Most of https://pigweed.dev is built with Sphinx. The [C/C++ API
reference](https://pigweed.dev/api/cc/) is built with Doxygen.
[rules\_doxygen](https://github.com/TendTo/rules_doxygen) runs the Doxygen
build (`//docs/doxygen:build`) hermetically within Bazel.

The C/C++ API reference is organized along the lines of Pigweed modules. See
the [Modules](https://pigweed.dev/api/cc/modules.html) landing page. This is
achieved through Doxygen [groups](https://www.doxygen.nl/manual/grouping.html).
The groups are defined in `//docs/doxygen/modules.h`. Header files are
annotated with `@module` and `@submodule` (aliases defined in
`//docs/doxygen/Doxyfile`) to group APIs into their respective Pigweed modules.
Every `@module` and `@submodule` annotation must have an accompanying
`@endmodule` and `@endsubmodule` annotation, respectively.

## Quickstart

1. Add the Pigweed module to `//docs/doxygen/modules.h`.
2. Annotate headers that contain public APIs with `@module` or `@submodule`.
   Always close with `@endmodule` or `@endsubmodule`.
3. Add a `filegroup` with name `doxygen` in the module's root `BUILD.bazel`
   file. List all Doxygen inputs in `srcs`.
4. Add the `doxygen` target to `//docs/doxygen:srcs`.
5. Follow Pigweed's Doxygen style guide (`//docs/sphinx/style/doxygen.rst`)
   when documenting headers.
6. Build the docs with `bazelisk build //docs`. Doxygen output can be inspected
   and at `//bazel-bin/docs/sphinx/_docs/_sources/doxygen/api/cc/`. The
   [tagfile](https://www.doxygen.nl/manual/external.html) is an authoritative
   index of the entire public API that Doxygen is aware of. It can be found at
   `//bazel-bin/docs/sphinx/_docs/_sources/doxygen/api/cc/index.tag`.

## Linking

* Sphinx to Doxygen: use the `:cc:` role. See
  `//docs/agents/rst/AGENTS.md`.
* Everything else (Doxygen to Sphinx, Doxygen to Rustdoc, Rustdoc to
  Doxygen): use hardcoded Markdown links with relative paths.

## Troubleshooting

Below is a list of common Doxygen errors followed by the most common fix for
each. See `//docs/sphinx/contributing/docs/doxygen.rst` if the most common fix
doesn't resolve the error.

* `… has @param documentation sections but no arguments` - Remove the `@param`
  documentation.
* `@copybrief or @copydoc target … not found` - Ensure the target exists and is
  fully qualified (including namespace and signature).
* `Argument … from the argument list of … has multiple @param documentation
  sections` - Document each `@param` only once.
* `Argument … of command @param is not found in the argument list` - Match the
  `@param` name to the function signature exactly. Use `@tparam` for template
  parameters.
* `Detected potential recursive class relation` - Hide the inheritance from
  Doxygen by wrapping the symbol in `@cond` and `@endcond`.
* `Documented symbol … was not declared or defined` - Add the symbol to the
  `PREDEFINED` list in `//docs/doxygen/Doxyfile` to ignore or expand it.
* `End of file while inside a group` - Ensure all `@module`, `@submodule`, and
  explicit groups (`@{`) are closed with `@endmodule`, `@endsubmodule`, or `@}`.
* `Explicit link request to … could not be resolved` - Use `@code` and
  `@endcode` blocks instead of inline backticks for this code example.
* `Found ')' without opening '(' for trailing return type` - Expand or ignore
  the symbol by adding it to the `PREDEFINED` list in `//docs/doxygen/Doxyfile`.
* `Found documented return type for … that does not return anything` - Remove
  `@return` or `@returns` from void functions, constructors, or destructors.
* `Found recursive @copybrief or @copydoc relation for argument …` - Break the
  cycle by documenting one of the entities directly.
* `Found unknown command` - Verify the command is valid, use `@p <name>` to
  refer to a parameter, and avoid unsupported commands like `@important` (use
  `@attention` or `@warning` instead).
* `Include file … not found` - Declare the missing header in the `doxygen`
  target of the relevant `BUILD.bazel` file.
* `Missing title after \defgroup` - Add the group as `@submodule` in
  `//docs/doxygen/modules.h` instead.
* `No uniquely matching class member found` - Wrap `friend` declarations in
  `@cond` and `@endcond`.
* `Refusing to add group … to itself` - Check for `@module` or `@submodule`
  annotations that aren't closed with `@endmodule` or `@endsubmodule`.
* `Unbalanced grouping commands` - Look for unclosed groups in other files
  within the same `@module` or `@submodule`.
* `Unsupported xml/html tag` - Wrap text containing angle brackets (like
  `<algorithm>`) in backticks.
