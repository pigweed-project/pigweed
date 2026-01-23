# Change Log

## [1.10.2] - 2026-01-21

### 🐛 Bug Fixes

 * Fix where a subdir of the workspace was being treated as the workspace root.
 * Simplify Bazel wrapper script generation.
 * Handle non-Pigweed or non-Bazel workspaces more gracefully.
 * Remove unneeded user confirmation prompt on first run.

## [1.10.1] - 2025-12-19

### 🐛 Bug Fixes

 * Fix bazelisk not found error when triggering compile commands generation.

## [1.10.0] - 2025-12-18

### ✨ Features & Improvements

 * Pre-configured Compile Commands: Compile commands can now be configured in
   your BUILD.bazel file. See https://pigweed.dev/pw_ide/guide/vscode/code_intelligence.html
   for details.

## [1.9.12] - 2025-10-20

### 🐛 Bug Fixes

 * Compile Commands: Fixed two issues related to the manual compile commands
   generation option, ensuring it works correctly with the wrapper.

 * Clangd Path: The IDE will now show a "Repair" button when the clangd
   path is empty, allowing for easier recovery from a misconfiguration.

 * Symlinked Files: Corrected an issue where active files were not being
   detected properly if the file path was a symbolic link.

## [1.9.11] - 2025-10-03

### ✨ Features & Improvements

* **Compile Commands Rework**: The system for creating the compile commands
database has been reworked and now defaults to the more robust aspect-based
generator.

* **UI & UX Enhancements**:
    * The output from the wrapper has been tuned for a better user experience.
    * A new "Help" section with a welcome header has been added to guide users.
    * The sidebar UI now automatically refreshes when a new build is completed.
    * The target selection dropdown now has a variable width to accommodate longer target names.

* **Improved Error Visibility**: Errors related to per-file compile commands
are now clearly displayed in the IDE's banner.

* **Configuration**:
	* A new flag has been added to avoid the creation of Bazel symlinks in the
      aspect wrapper.
	* The last used Bazel command is now saved in a `.txt` file within the
      project instead of the global VS Code settings.

### 🐛 Bug Fixes

* **Performance**:
	* To prevent excessive restarts, the `clangd` process is now "debounced"
      when compile commands are changed frequently.
	* The number of CPU cores used by the `clangd` process during indexing has
      been reduced to improve system performance.

* **Stability**:
	* Fixed a crash that could occur if the Pigweed project root directory was
      not found.
	* Corrected an issue that prevented pasting and other keyboard events from
      working properly in the manual build input field.

## [1.9.9] - 2025-09-05

### Bug Fixes

 - Increase max direct dependencies for provider collector.
    + This increases the maximum number of direct targets the aspect will
      evaluate to handle these kinds of large targets.

## [1.9.8] - 2025-09-04

### Features

 - New experimental compile commands generator based on Bazel's
   [aspects](https://bazel.build/extending/aspects) feature.
 - The default commands generator is now based on the previous experimental
   python based generator.

### Bug Fixes

 - Clicking the targets in the status bar now opens the target selection.
 - Don't parse compile commands for headers.

## [1.9.7] - 2025-08-19

### Bug Fixes

 - Show a message of the correct targets for currently untracked files.
 - Automatically fix and correct clangd settings if they are incorrect or
   missing. This ensures code intelligence is always functional for the user.
 - Remove zxh404.vscode-proto3 as dependency.

## [1.9.6] - 2025-08-01

### Bug Fixes

 - Fix incremental build failures on compile commands generator test.
   ([b/429233254](https://issuetracker.google.com/b/429233254))
 - Improve the development guide for running, building, and debugging the
   VSCode extension.
 - Fix the clang path when using the fish shell.
 - Run the compile commands target with the same flags as real bazel
   invocation. This ensures the artifacts in bazel-bin/ are not cleared.

## [1.9.5] - 2025-07-07

### Features

 - Revamped code intelligence integration using clangd.
 - Automatic target recognition for Bazel workflows.
 - A new python based commands generator (this is experimental) that is more
   accurate and simpler to maintain moving forward.

### Bug Fixes

 - Fix fish shell support for the commands generator ([b/428243657](https://issuetracker.google.com/428243657))

## [1.3.3]

- The `pigweed.activateBazeliskInNewTerminals` option has been set to a default
  of `false` because, as currently implemented, this feature interferes with
  interactive tasks like flashing scripts and `pw_console`.

## [1.3.2]

- When you manually run `Pigweed: Refresh Compile Commands`, we now show a
  progress notification so you know what's happening.

- Synchronizing settings between the committed settings (`settings.shared.json`)
  and the local settings (`settings.json`) is now dramatically faster (pretty
  much instantaneous).

- If you manually change the code analysis target by editing a settings file
  directly, the effect will now be the same as if you had run the
  `Pigweed: Select Code Analysis Target`.

## [1.3.1]

- Newly-launched integrated terminals will now automatically have the
  configured path to `bazelisk` patched into the shell path. This is essentially
  running `Pigweed: Activate Bazelisk in Terminal` automatically when launching
  a new integrated terminal. This behavior can be disabled by setting
  `pigweed.activateBazeliskInNewTerminals` to `false` in your settings.

## [1.3.0]

- You can now disable `clangd` code intelligence for files that are not built
  as part of the target you're using for code intelligence. Why would you want
  to do this? By default, `clangd` will try to *infer* compile commands for
  files that won't actually be built, producing incorrect and misleading code
  intelligence information ([see this doc](https://pigweed.dev/pw_ide/guide/vscode/code_intelligence.html#inactive-and-orphaned-source-files)
  for more information).

- File indicators in the file explorer and file tabs now show each file's status
  with regard to the currently-selected code intelligence target. For example,
  files that are not part of the target's build are faded out so you're not
  surprised when they don't show code intelligence ([learn more here](https://pigweed.dev/pw_ide/guide/vscode/code_intelligence.html#inactive-and-orphaned-source-files)).

- We now provide a mechanism for shared settings that can be committed to source
  control, along with personal user settings that also include ephemeral editor
  state, like the currently-selected code intelligence target and `clangd`
  settings. [Check out this doc](https://pigweed.dev/pw_ide/guide/vscode/#project-settings)
  for more details.

- Output from the compile commands refresh process is now streamed in real-time
  to the Pigweed output window.

- The method of finding the path to `clangd` in the toolchain brought in by
  Bazel is now more robust and reliable.

- Bundled tools (`bazelisk` and `buildifier`) now have their paths automatically
  updated whenever a new version of the extension is installed.

## [1.1.4]

- Properly supports [bzlmod](https://docs.bazel.build/versions/5.1.0/bzlmod.html)
  projects.

## [1.1.3]

- Fixes a project root inference bug.

## [1.1.2]

- Output from the compile commands refresh process is now streamed to the
  Pigweed output panel in real time, allowing you to monitor progress in the
  same way you would if you had invoked it from the terminal. Before, the output
  was just buffered and sent to the output window when complete.

- The extension will no longer attempt to infer the project root if one is set
  manually via [pigweed.projectRoot](https://pigweed.dev/pw_ide/guide/vscode/#pigweed.projectRoot).
  This prevents spurious failures if you have an uncommon project structure but
  have pointed the Pigweed extension in the right direction by specifying the
  project root.

- Missing command implementation stubs for bootstrap-based projects are no
  longer missing.

## [1.1.1]

- Adds support for [fish shell](https://fishshell.com/) to the
  `Pigweed: Activate Bazelisk in Terminal` command (in addition to the existing
  support for `bash` and `zsh`).

## [1.1.0]

- Adds `Pigweed: Activate Bazelisk in Terminal`: This command will modify the
  `$PATH` in your active integrated terminal to include the configured path to
  Bazelisk. This allows you to run Bazel actions via editor commands or via
  `bazelisk ...` invocations in the terminal, while working in the same Bazel
  environment.

## [1.0.0]

- Initial release of the new and improved Pigweed extension.
