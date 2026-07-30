# Security bulletin creation

## General guidelines

The goal of the security bulletins is to communicate to Pigweed consumers about
security vulnerabilities that have been found and fixed. The most important
pieces of information to convey are what the impact of each vulnerability is if
left unpatched and what Git revision of Pigweed contains its fix.

## 1. Get the Git revision for the fix.

When this skill is invoked, a vulnerability should have been fixed by the most
recent Git revision. Verify using `git status` that there is nothing to commit
and that the working tree is clean. If not, inform the user and remind them that
this skill should only be run after a embargoed fix has been cherry-picked from
the internal repo.

Display the first line of the commit message of the most recent Git revision to
the user, and ask them to confirm that it is a cherry-picked embargoed fix.

## 2. Get the bug number.

Examine the commit message and find the corresponding bug number, typically in
a line like "Bug: {bug number}" or "Fixes: {bug number}". If the commit message
is missing a bug number, infrom the user and prompt them to provide one;
otherwise, prompt them to confirm it. Do not guess.

## 3. Determine the impact

Prompt the user to identify the **impact** of the vulnerability, choosing from
one of the codes below:

* CE: Potential code execution, including most memory corruptions that affect
  control flow.
* S: Spoofing, such as injection of malicious data from what appears to be a
  valid source.
* T: Tampering, such as modifying state or user data in way that affects device
  behavior, e.g. changing parameters used to connect or where data is sent.
* ID: Information disclosure, such as an out-of-bound reads or other info leak.
* DoS: Denial of service, such as triggering assertions or corrupting memory in
  a way that is **guaranteed** to crash.
* N/A: Classification not available. (Use this sparingly!)

This classification will be the vulnerability's "Impact code".

## 4. Determine the modules affected

Take note of the name or name(s) of the Pigweed module(s) that contained the
vulnerability, e.g. pw_allocator. One such module is typically at the start
of the commit message of the Git revision that fixes the vulnerability. Others
may be referenced in the bug.

## 5. Create a bulletin directory for the current year, if needed.

Check if a directory exists at "docs/sphinx/security/{YYYY}/"
where 'YYYY' is the current year, e.g. "docs/sphinx/security/2026/".

If the directory already exists, skip to step 6.

Otherwise, create the "docs/sphinx/security/{YYYY}/" directory and add a file at
"docs/sphinx/security/{YYYY}/index.rst" using the following template:

```
.. _docs-security-{YYYY}:

=======================
{YYYY} Security Bulletins
=======================

.. toctree::
   :maxdepth: 1

```

Add a line like "   {YYYY}/index" to the end of the second, non-hidden 'toctree'
section at the end of "security/index.rst"

Add "security/{YYYY}/index.rst" at the correctly sorted location
in 'srcs' list of the sphinx_docs_library target named "content" in
docs/sphinx/BUILD.bazel.

## 6. Create a bulletin file for the current month, if needed.

Check if a file exists at "docs/sphinx/security/{YYYY}/{MM}.rst"
where 'YYYY' is the current year and 'MM' is the current month, e.g.
"docs/sphinx/security/2026/07.rst".

If the file already exists, skip to step 7.

Otherwise, create the "docs/sphinx/security/{YYYY}/{MM}.rst" file using the
following template, where 'MonthName' is the month name in English, e.g. "July",
and 'OrdinalDay' is the day of the month as an ordinal, e.g. "15th":

```
.. _docs-security-{YYYY}-{MM}:

=========================
Security bulletin {YYYY}-{MM}
=========================

This bulletin lists the critical and high severity vulernabilities that were
fixed for the month of {MonthName}, {YYYY}.

This bulletin was last updated on {MonthName} {OrdinalDay}, {YYYY}.

---------------------
Vulnerability details
---------------------
.. list-table::
   :header-rows: 1

   * - CVE
     - Reference
     - Impact
     - Modules affected
     - Fixed by

--------------------------
How to interpret the table
--------------------------
* CVEs are assigned after a fix is released, and may be listed as "TBD" in the
  interim.
* If the 'Reference' column is "N/A", the bug report cannot be made public at
  this time.
* The 'Impact' column uses the following abbreviations:

  .. list-table::
     :header-rows: 1

     * - Abbreviation
       - Definition
     * - CE
       - Potential code execution, including most memory corruptions that affect
         control flow.
     * - S
       - Spoofing, such as injection of malicious data from what appears to be
         a valid source
     * - T
       - Tampering, such as modifying state or user data in way that affects
         device behavior
     * - ID
       - Information disclosure, including memory corruptions such as
         out-of-bound reads
     * - DoS
       - Denial of service, such as triggering assertions
     * - N/A
       - Classification not available

* The 'Fixed by' column provides the revision that consumers should upgrade to
  or cherry-pick.
* The 'Modules affected' column lists the primary modules affected. Be aware
  that other modules may be affected through transiaitve dependencies.
```

Add a line like "   {MonthName} {YYYY} <{YYYY}/{MM}>" to the end of the
'toctree' section at the end of "security/{YYYY}/index.rst".

Add "security/{YYYY}/{MM}.rst" at the correctly sorted location
in 'srcs' list of the sphinx_docs_library target named "content" in
docs/sphinx/BUILD.bazel.

## 7. Add the vulnerabilty to the bulletin file.

Modify "security/{YYYY}/{MM}.rst" according to the following:

* Find the line that starts with "This bulletin was last updated on" and replace
  it with "This bulletin was last updated on {MonthName} {OrdinalDay}, {YYYY}."
* Add a row to the table in the 'Vulnerability details' section with the
  following values:
  * CVE: "TBD"
  * Reference: "b/{bug number}"
  * Impact: The 'Impact code' from step 2.
  * Modules affected: The name(s) from step 4 of the module(s) that contained the
    vulnerability, separated by commas.
  * Fixed by: The Git revision from step 3.

Ensure there is only one blank line between the last row of the table and the
next section heading.

## 8. Format the bulletin and index

Format any of the following RST files using the skill at
`//docs/agents/rst/AGENTS.md`:

  * docs/sphinx/security/{YYYY}/index.rst
  * docs/sphinx/security/{YYYY}/{MM}.rst

Run `bazelisk build //docs` to ensure the docs build.
