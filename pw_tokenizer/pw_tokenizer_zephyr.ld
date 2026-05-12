/*
 * Copyright 2023 The Pigweed Authors
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
 * This file is separate from pw_tokenizer_linker_sections.ld because Zephyr
 * already defines the top level SECTIONS label and requires new linker
 * scripts to only add the individual sections. Additionally, the 0x0 address
 * is incompatible with Zephyr's linker scripts.
 *
 * See detailed section info at pw_tokenizer_linker_sections.ld
 */

#ifdef CONFIG_ARCH_POSIX
#define PW_TOKENIZER_ADDR
#else
#define PW_TOKENIZER_ADDR 0x0
#endif

.pw_tokenizer.info PW_TOKENIZER_ADDR (INFO) :
{
  KEEP(*(.pw_tokenizer.info))
}

.pw_tokenizer.entries PW_TOKENIZER_ADDR (INFO) :
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
