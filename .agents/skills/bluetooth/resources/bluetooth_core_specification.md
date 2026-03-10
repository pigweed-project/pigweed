# Bluetooth Core Specification Management

This resource explains how to access, search, and cite the Bluetooth Core Specification effectively using the dedicated local cache and management tools.

## The Local Cache Strategy

Since the official Bluetooth specification is massive and fragmented, we use a local cache of the seven most critical "Core" chapters. This allows for instant, reliable searching even for complex data like packet formats and section numbers.

### **The Three-File System**
For every chapter in the core specification, the shared cache contains three versions:
1. **`<name>.md` (The Search Index):** Our primary search target. It features **Markdown tables** and **stitched headers** (e.g., "3.5.1. Pairing Request") on single lines for perfect `grep_search` results.
2. **`<name>_pretty.html` (The Context View):** A formatted HTML file used for `view_file`. Use this to see original tables, lists, and diagram references once you've found the correct section in the `.md` file.
3. **`<name>.html` (The Source):** The raw, minified official HTML from bluetooth.com.

### **Section Index**
We maintain a global **`index.md`** in the specifications directory.
* **Use this first:** To find which file contains a specific section number or topic, run `grep_search` on `index.md`. The `index.md` output will contain the file name and the exact line number (e.g., `logical-link-control-and-adaptation-protocol-specification.md:1057`), allowing you to use `view_file` to jump directly to the correct line in the corresponding `.md` file.

---

## Management Tooling

### **Synchronizing the Cache**
To refresh all core specifications or initialize the cache, run the downloader via Bazel without arguments:
`bazelisk run //.agents/skills/bluetooth/scripts:bluetooth_specification_downloader`

*Note: Since the script is executed via Bazel, the generated Markdown files, HTML copies, and global `index.md` will be placed in the Bazel execution sandbox (`runfiles`) rather than the local source folder. You can find the exact sandbox location by running `bazelisk info bazel-bin`.*
*The full path will be: `$(bazelisk info bazel-bin)/.agents/skills/bluetooth/scripts/bluetooth_specification_downloader.runfiles/_main/.agents/skills/bluetooth/specifications/`*

This tool automatically:
* Downloads the latest HTML from official URLs.
* Converts HTML tables into readable Markdown tables.
* Stitches fragmented section numbers into searchable headers.
* Re-generates the `index.md`.

---

## How to Research and Search

1. **Start with the Core Specs:**
   * First, run `bazelisk info bazel-bin` to get the base path to the generated files.
   * Use `grep_search` on `<bazel-bin-output>/.agents/skills/bluetooth/scripts/bluetooth_specification_downloader.runfiles/_main/.agents/skills/bluetooth/specifications/index.md` to find the correct file and line number.
   * **Use Case Insensitivity:** The Bluetooth specification uses inconsistent capitalization. Always use `CaseInsensitive: true` when searching.
   * Once you have the target file name and line number from `index.md`, use `view_file` to read the exact section in the corresponding `.md` file.
2. **When the topic is outside the Core Specs:**
   * Use the `search_web` tool with `site:bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62 "term"`.
3. **Citing the Specification:**
   * Always provide the Volume, Part, and Section (e.g., "Vol 3, Part H, Section 3.5.1").
