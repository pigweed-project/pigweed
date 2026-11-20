/*
 * Copyright 2025 The Pigweed Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

/*
 * Add tokenizer sections as LOAD-able data so it is available at runtime.
 * This allows a program to use itself as a token database and detokenize its
 * own tokens. This is primarily intended for tests.
 *
 * This linker script fragment is similar to
 * add_tokenizer_sections_to_default_script.ld and
 * pw_tokenizer_linker_sections.ld but with the following differences:
 *
 * * Sections are *not* marked as type INFO, so they are allocated in the
 *   program image and are loaded into memory where they can be accessed at
 *   runtime.
 * * Symbols are provided to allow code to access the ranges of data.
 */

SECTIONS
{
  .pw_tokenizer.info :
  {
    __pw_tokenizer_info_start = .;
    KEEP(*(.pw_tokenizer.info))
    __pw_tokenizer_info_end = .;

  }

  .pw_tokenizer.entries :
  {
    __pw_tokenizer_entries_start = .;
    KEEP(*(.pw_tokenizer.entries.*))

    /* GCC has a known bug (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88061)
     * that causes it to ignore any user-specified section placement for
     * variables declared inside function templates. The symbols for these
     * variables instead end up in a .rodata.* subsection. The subsection names
     * for these symbols all contain the string "_pw_tokenizer_string_entry_"
     * (as long as -fdata-sections was used when compiling). Thus we can pick
     * the relevant sections by using an appropriate wildcard.
     *
     * Note that this technique only works because nothing in the source code
     * references the *_pw_tokenizer_string_entry_* symbols. This ensures the
     * linker will never place such symbols in the final .data (aka .rodata)
     * output section, meaning such symbols remain available for us to place
     * into the  .pw_tokenizer_entries section.
     */
    KEEP(*(.rodata.*_pw_tokenizer_string_entry_*))
    __pw_tokenizer_entries_end = .;
  }
}

/*
 * The INSERT directive instructs the linker to merge the directives in this
 * script with the default linker script, rather than replace the default with
 * this script. The tokenized sections need to appear in the merged script
 * before the .rodata section, to ensure that all of the correct symbols end up
 * in the tokenized section.
 */
INSERT BEFORE .rodata
