# Add a security note

This is a short skill for annotating vulnerability scan false positives.

## Objective

Sometimes, the vulnerability scanning agents believe a section of code is
vulnerable when in reality it is not. This may be due to environmental or
operational constraints that are not apparent from inspecting the code alone.

Your goal will be to annotate code that has been incorrectly flagged as
vulnerable with a security note that helps explain to subsequent scans why the
coude in question is not vulnerable.

## 1. Find the code in question.

The user will provide a filename and symbol, e.g. "Add a security note to
StatusWithSize in pw_status/public/pw_status/status_with_size.h". If they do not
provide the filename or symbol, prompt them for it. Do not guess.

Read the file and examine the code related to the given symbol. The file should
be a C or C++ source or header file. If it is not, alert the user and STOP HERE.

## 2. Examine the related bug.

A user may optionally provide a bug ID in the form of "b/{nug number}", e.g.
"b/512562635. If they do not provide a bug ID, or if it is not in the correct
format, inform the user of the format and ask if they wish to provide a bug ID
or skip ahead.

If they provide a bug ID, analyze it using the integrated Buganizer tool to
understand how the code in question is vulnerable.

## 3. Get the user's reasoning

Prompt the user to explain why the code in question is not vulnerable. Analyze
their response and evaluate whether it does in fact address the vulnerable code.
If it doesn't, explain why it doesn't to the user and ask them to clarify.

Repeat until the user cancels the workflow or their reasoning is sound.

## 4. Add the security note.

Summarize the user's reasoning to be succinct while preserving any important
details.

For the steps in this section, you may neglect proper indentation and line
wrapping. These will be handled in the next section when the file is formatted.

For the steps in this section, a "comment line" is one starting with "// " or
"/// ". Prefer to use the one that is already used with the given symbol, or
"/// " if the symbol is not commented. Similarly, a "blank comment line" is
either "//" or "///" for the same conditions.

If the symbol in question is already preceded by a comment line, append a blank
comment line.

Right before the symbol in question, add the comment line "@security" and a
blank comment line.

Add the summarized user's reasoning as a comment line followed by a blank
comment line.

If a bug number was provided, add a comment line
"See b/{bug number} for additional details." (with {bug number} replaced by
the bug number), followed by a blank comment line.

Add the comment line "@endsecurity".

## 5. Format the changes.

Run `./pw format` in the root of the Pigweed repository to ensure the security
note is properly formatted.

## Example

Assume the user asked to "Add a security note to StatusWithSize in
pw_status/public/pw_status/status_with_size.h to address b/512562635".

After obtaining the user's reasoning, the completed security note might appear
as follows when denoted as a git diff:

```
   /// Creates a StatusWithSize with the provided status and size.
+  ///
+  /// @security
+  ///
+  /// StatusWithSize doesn't check that the StatusCode it receives is valid.
+  /// This isn't a vulnerability, because if an attacker can set a StatusCode,
+  /// they can already set it to whatever they want. There is nothing gained by
+  /// setting an invalid value that is later truncated to a valid value when an
+  /// attacker can just set it to a valid value.
+  ///
+  /// See b/512562635 for additional details.
+  ///
+  /// @endsecurity
   explicit constexpr StatusWithSize(Status status, size_t size)

```
