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

1. Add a `filegroup` with name `doxygen` in the module's root `BUILD.bazel`
   file. List all Doxygen inputs in `srcs`.
2. Add the `doxygen` target to `//docs/doxygen:srcs`.
3. Annotate headers that contain public APIs with `@module` or `@submodule`
   inside the innermost `namespace` that declares those symbols (Doxygen won't
   attach declarations inside a nested `namespace` block to an outer group).
   Always close with `@endmodule` or `@endsubmodule`.
   * Doxygen publishes **everything** between `@module`/`@submodule` and
     `@endmodule`/`@endsubmodule` on the module page, **even symbols that have
     no `///` Doxygen comment**.
   * Symbols starting with `_` (such as `_PW_*` and `_pw_*`) are automatically
     excluded via `EXCLUDE_SYMBOLS = _*` in `//docs/doxygen/Doxyfile`.
   * Unless an `internal` or `detail` symbol must be exposed for Doxygen to
     document a public API (such as an internal base class whose public methods
     are inherited by a public class), keep `internal` and `detail` namespaces
     outside of `@module`/`@submodule` blocks. **Never move code around** to
     group public APIs together; if `internal` namespaces are interleaved with
     public APIs, close and reopen `@module`/`@endmodule` (or
     `@submodule`/`@endsubmodule`) around them.
   * Do not use `/// @cond` and `/// @endcond` to hide internal APIs. Reserve
     `@cond` for working around Doxygen C++ parser errors (such as on `friend`
     declarations, recursive template base classes, or out-of-line template
     method definitions in headers). Never place `@module`, `@endmodule`,
     `@submodule`, `@endsubmodule`, `@{`, or `@}` inside a `@cond` block.
4. `//docs/doxygen/modules.h` has an auto-generated section at the top (between
   `// categories-start` and `// categories-end`) for category and module
   groups, followed by a manual section at the end (after `// categories-end`)
   for submodule groups. If using `@submodule`, you must define its `@defgroup`
   and `@ingroup` in the manual section of `//docs/doxygen/modules.h` (for
   non-module APIs such as `//third_party/...`, use `@ingroup category_<id>`);
   uncategorized Doxygen groups fail `//docs/common/metadata:sync` and
   `bazelisk build //docs`.
5. Run `bazelisk run //docs/common/metadata:sync` to auto-generate the module's
   top-level `@defgroup` entry in `//docs/doxygen/modules.h` from
   `//<module>/module.toml` and `//docs/common/metadata/categories.toml`.
6. Follow Pigweed's Doxygen style guide (`//docs/sphinx/style/doxygen.rst`)
   when documenting headers.
7. Build the docs with `bazelisk build //docs`. Doxygen output can be inspected
   at `//bazel-bin/docs/sphinx/_docs/_sources/doxygen/api/cc/`.
   * Inspect the rendered module page (`group__<module>.html`, where underscores
     in the module name are doubled, e.g. `group__pw__metric.html`) to verify
     that all public APIs are present and no internal symbols or private macros
     leaked onto the page.
   * The [tagfile](https://www.doxygen.nl/manual/external.html) at
     `//bazel-bin/docs/sphinx/_docs/_sources/doxygen/api/cc/index.tag` indexes
     *documented* symbols, but omits undocumented symbols inside `@module` or
     `@submodule` blocks. Do not rely on `index.tag` alone to verify that
     internal symbols were excluded.

## Code examples

Pigweed uses a custom `@example` Doxygen alias to import testable code examples
directly from unit tests into API reference doc comments:

1. **Tag the example in the unit test:** Wrap the test code using
   `// DOCSTAG: [<tag-name>]` comments:

   ```cpp
   // DOCSTAG: [pw_base64-encode]
   constexpr uint8_t input[] = {0x14, 0xfb, 0x9c, 0x03, 0xd9, 0x7e};
   char output[EncodedSize(sizeof(input)) + 1] = {};

   Encode(as_bytes(span(input)), output);
   // DOCSTAG: [pw_base64-encode]
   ```

2. **Expose the test file to Doxygen:** In the module's `BUILD.bazel`, ensure the
   test file is included in the `doxygen` `filegroup`:

   ```starlark
   filegroup(
       name = "doxygen",
       srcs = [
           "base64_test.cc",
           "public/pw_base64/base64.h",
       ],
   )
   ```

3. **Include the example in the doc comment:** Use the
   `@example{<path>,<tag-name>}` alias:

   ```cpp
   /// @example{pw_base64/base64_test.cc,pw_base64-encode}
   ```

   The first argument is the repository-relative path to the test file, and the
   second is the snippet tag name. This alias automatically wraps the snippet in
   a `@par Example` section and generates a `Source: <path>` link to Pigweed Code
   Search with a `?q=<tag-name>` query parameter to locate the snippet.

See `//docs/sphinx/contributing/docs/examples.rst` for more details.

## Linking

* Sphinx to Doxygen: use the `:cc:` role. See
  `//docs/agents/rst/AGENTS.md`.
* Module TOC positioning: by default the C/C++ API reference link is auto-appended
  to the module homepage's TOC. To explicitly control its position, place
  `pw://cc-api-ref` in the `.. toctree::`. See `//docs/agents/rst/AGENTS.md`.
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
* `Doxygen group '…' is not under any category` - Fix any `@module` or
  `@submodule` typo, place `@module` inside the innermost `namespace` declaring
  the symbols rather than outside a nested `namespace`, or declare `@defgroup`
  and `@ingroup` in the manual section of `//docs/doxygen/modules.h`.
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
* `No uniquely matching class member found` - Wrap `friend` declarations or
  out-of-line template method definitions in `@cond` and `@endcond`.
* `Refusing to add group … to itself` - Check for `@module` or `@submodule`
  annotations that aren't closed with `@endmodule` or `@endsubmodule`.
* `Unbalanced grouping commands` - Look for unclosed groups in other files
  within the same `@module` or `@submodule`.
* `Unsupported xml/html tag` - Wrap text containing angle brackets (like
  `<algorithm>`) in backticks.
