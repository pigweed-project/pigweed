# Module metadata and categorization

How to manage Pigweed module metadata and documentation categories across
Sphinx, Doxygen, and rustdoc.

## Canonical files

* `//docs/common/metadata/categories.toml`: The single source of truth for
  category metadata (`[<category_id>]` tables, in display order).
* `//docs/common/metadata/categories_schema.json`: JSON schema for
  `categories.toml`.
* `//<module>/module.toml`: Each module's metadata (`status`, at least one
  of `[category]` or `[facade]`, and optionally `tagline`, `languages`, and
  `size`). Must be listed in `srcs` of the module's `sphinx_docs_library` in
  `//<module>/BUILD.bazel`.
* `//docs/common/metadata/module_schema.json`: JSON schema for
  `//<module>/module.toml`.
* `//docs/common/metadata/sync.py` (`bazelisk run //docs/common/metadata:sync`):
  CLI tool that regenerates `//docs/sphinx/platform.rst`,
  `//docs/sphinx/modules.rst`, `//docs/sphinx/index.rst`, category landing
  pages, facade `//<facade>/backends.rst` pages, `//docs/sphinx/BUILD.bazel`,
  `//docs/doxygen/modules.h`, and `//pw_rust/rust/pigweed/src/lib.rs` from
  `categories.toml` and `//<module>/module.toml`. Pass `-- --check` to report
  out-of-date files without modifying them.
* `//docs/common/metadata/validate.py`: Sphinx `build-finished` hook that fails
  `bazelisk build //docs` if `categories.toml` or any `//<module>/module.toml`
  is invalid, inconsistent with `//PIGWEED_MODULES`, or out of sync with the
  generated files.

See the `Module categorization` section of
`//docs/sphinx/contributing/docs/website.rst` for more context.

## Workflow: Adding a module

`pw module create` scaffolds `//<module>/module.toml` with a commented-out
`[category]` / `[facade]` template, and `bazelisk build //docs` fails until at
least one is uncommented and populated. Follow the workflow below to complete
it.

## Workflow: Adding or updating a module's metadata or category

1. Ensure the module is listed in `//PIGWEED_MODULES` and `"module.toml"` is
   in `srcs` of its `sphinx_docs_library` in `//<module>/BUILD.bazel`.
2. Add or update `//<module>/module.toml` (for example,
   `//pw_alignment/module.toml`):

   ```toml
   status = "stable"
   tagline = "Natural object alignment, guaranteed"
   languages = ["C++"]

   [category]
   id = "memory"
   toctree = "Alignment"
   ```

   * `[category]`: `id = "<category_id>"` and `toctree = "<title>"` specify the
     module's **canonical category** in the Sphinx `toctree` (rendered as
     `<title> <../<module>/docs>` on the category's landing page). Both
     `id` (a table in `//docs/common/metadata/categories.toml`) and `toctree`
     (a short human-readable title without `pw_` or target suffixes) are
     required.
   * **Backend modules (`[facade]`)**: Backend modules that implement a facade
     set `[facade]` with `module = "<facade_module>"` and `toctree = "<title>"`
     (for example, `module = "pw_assert"` and `toctree = "Zephyr"`) so they are
     progressively disclosed on `//<facade>/backends.rst` instead of cluttering
     the facade's functional category tables (`observability`, `hal`, `os`,
     etc.). `toctree` sets the short display title in
     `//<facade>/backends.rst` (separate from `category.toctree` because the
     short title changes depending on where the module is listed).
     Target-specific backends (for an OS or hardware platform category such as
     `zephyr`, `fuchsia`, `freertos`, `threadx`, `embos`, `android`, `ambiq`,
     `arduino`, `emcraft`, `nxp`, `rpi`, `stm`, `ti`) set both `[category]` and
     `[facade]` (for
     example, `[category]` with `id = "zephyr"`, `toctree = "Logging"` and
     `[facade]` with `module = "pw_log"`, `toctree = "Zephyr"` in
     `//pw_log_zephyr/module.toml`), making the target category their
     canonical `toctree` parent while `//<facade>/backends.rst` cross-references
     them with `pw://`. Target-agnostic backends (such as `pw_assert_basic` or
     `pw_log_tokenized`) omit `[category]`, making `//<facade>/backends.rst`
     their canonical `toctree` parent.
3. Run the synchronization tool to update Sphinx, Doxygen, and rustdoc files
   in-place:

   ```console
   $ bazelisk run //docs/common/metadata:sync
   ```

4. Verify with `bazelisk build //docs`.

## Workflow: Adding, renaming, or deleting a category

1. Update `//docs/common/metadata/categories.toml`. The order of the
   `[<category_id>]` tables is the display order; the first 12 top-level
   categories get a card on the homepage. Top-level categories require `icon`;
   subcategories set `parent = "<parent_category_id>"` (for example,
   `parent = "os_backends"` on `[zephyr]` or `parent = "hw"` on `[rpi]`) and
   omit `icon` so they are nested as H3+ subsections under their parent
   category. By default, a category's direct modules are sorted alphabetically
   and appear before its subcategories; set `pin = [...]` on a category table to
   pin specific direct module names or child subcategory IDs to the top in that
   exact order.
2. Update the `[category]` tables in `//<module>/module.toml` of affected
   modules. Every category must contain at least one module directly or via a
   descendant subcategory.
3. If you deleted or renamed a category, delete its old landing page or remove
   its `.. categories-start` / `.. categories-end` block.
4. Run `bazelisk run //docs/common/metadata:sync` to scaffold new category
   landing pages and update `//docs/sphinx/BUILD.bazel`,
   `//docs/sphinx/platform.rst`, `//docs/sphinx/modules.rst`,
   `//docs/sphinx/index.rst`, `//docs/doxygen/modules.h`, and
   `//pw_rust/rust/pigweed/src/lib.rs`.
5. Verify with `bazelisk build //docs`.

## Customizing category landing pages

* Category landing pages live at the path specified by each category's
  `homepage` in `//docs/common/metadata/categories.toml` (e.g.
  `//docs/sphinx/toolchains/index.rst`, `//docs/sphinx/os/index.rst`). Each
  category's `description` is automatically rendered as a subtitle below the H1
  heading on its landing page by `//docs/sphinx/_extensions/module_metadata.py`.
* Do not manually edit regions inside `.. categories-start` /
  `.. categories-end`, `# categories-start` / `# categories-end`,
  `// categories-start` / `// categories-end`, or
  `//! <!-- categories-start -->` / `//! <!-- categories-end -->` markers; they
  are overwritten by `bazelisk run //docs/common/metadata:sync`. Hand-written
  content before or after the markers is preserved.
* To add non-module docs (such as cross-module guides or third-party
  integrations) to a category's sidebar navigation, do not edit the
  auto-generated `.. toctree::` inside `.. categories-start` /
  `.. categories-end`. Instead, add a separate `.. toctree::` directive outside
  the markers: place it **after** `.. categories-end` (without `self`) to
  append items after the category's modules (see
  `//docs/sphinx/observability/index.rst`), or **before**
  `.. categories-start` (starting with `self`) to list introductory guides
  before the category's modules (`sync.py` automatically omits `self` from the
  generated `.. toctree::`; see `//docs/sphinx/toolchains/index.rst`).
