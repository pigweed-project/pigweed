/*
 * Copyright 2020 The Pigweed Authors
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

/* LINT.IfChange(tokenizer_sections) */
SECTIONS
{
  /*
   * This section stores metadata that may be used during tokenized string
   * decoding. This metadata describes properties that may affect how the
   * tokenized string is encoded or decoded -- the maximum length of the hash
   * function and the sizes of certain integer types.
   *
   * Metadata is declared as key-value pairs. See the metadata variable in
   * tokenize.cc for further details.
   */
  .pw_tokenizer.info 0x0 (INFO) :
  {
    KEEP(*(.pw_tokenizer.info))
  }

  /*
   * Tokenized string entries are stored in this section. Each entry contains
   * the original string literal and the calculated token that represents it. In
   * the compiled code, the token and a compact argument list encoded in a
   * uint32_t are used in place of the format string. The compiled code
   * contains no references to the tokenized string entries in this section.
   *
   * The tokenized string entry format is specified by the
   * pw::tokenizer::internal::Entry class in
   * pw_tokenizer/public/pw_tokenizer/internal/tokenize_string.h.
   *
   * The section contents are declared with KEEP so that they are not removed
   * from the ELF. These are never emitted in the final binary or loaded into
   * memory.
   */
  .pw_tokenizer.entries 0x0 (INFO) :
  {
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
  }
}
/* LINT.ThenChange(pw_tokenizer_linker_sections.ld:tokenizer_sections) */

/*
 * The INSERT directive instructs the linker to merge the directives in this
 * script with the default linker script, rather than replace the default with
 * this script. The tokenized sections need to appear in the merged script
 * before the .rodata section, to ensure that all of the correct symbols end up
 * in the tokenized section. Here we insert them just after the .debug_info
 * section, which both clang and GCC use.
 */
INSERT AFTER .debug_info
