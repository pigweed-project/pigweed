# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Generates Java data-driven detokenizer test code."""

import io
from typing import Iterator

from pw_build.generated_tests import (
    Context,
    java_bytes,
    java_string,
)
from pw_tokenizer import tokens
from pw_tokenizer.detokenize import Detokenizer
import detokenize_test_cases as shared

_JAVA_HEADER = """\
package dev.pigweed.pw_tokenizer;

import static com.google.common.truth.Truth.assertThat;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class DataDrivenDetokenizerTest {
"""

_JAVA_FOOTER = """\
}
"""

_TEST_DB = Detokenizer(
    tokens.Database(tokens.parse_binary(io.BytesIO(shared.TEST_DATABASE)))
)
_TEST_DB_STR = java_string(str(_TEST_DB.database))

_ARGS_DB = Detokenizer(
    tokens.Database(tokens.parse_binary(io.BytesIO(shared.DATA_WITH_ARGUMENTS)))
)
_ARGS_DB_STR = java_string(str(_ARGS_DB.database))

_COLLISIONS_DB = Detokenizer(
    tokens.Database(
        tokens.parse_binary(io.BytesIO(shared.DATA_WITH_COLLISIONS))
    )
)
_COLLISIONS_DB_STR = java_string(str(_COLLISIONS_DB.database))

_JAVA_DATABASE_DEFS = f"""\
  private static final String TEST_DATABASE = {_TEST_DB_STR};
  private static final String DATA_WITH_ARGUMENTS = {_ARGS_DB_STR};
  private static final String DATA_WITH_COLLISIONS = {_COLLISIONS_DB_STR};
"""


def _java_test_text(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
  @Test
  public void {ctx.java_name()}() {{
    try (Detokenizer detok = Detokenizer.fromCsv(TEST_DATABASE)) {{
      assertThat(detok.detokenizeText({java_string(data)}))
          .isEqualTo({java_string(expected)});
    }}
  }}"""


def _java_test_optional(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    db = tokens.Database(tokens.parse_binary(io.BytesIO(shared.TEST_DATABASE)))
    detok = Detokenizer(db)
    if not detok.detokenize(data).ok():
        assertion = ".isNull()"
    else:
        assertion = f".isEqualTo({java_string(expected)})"

    yield f"""\
  @Test
  public void {ctx.java_name()}() {{
    try (Detokenizer detok = Detokenizer.fromCsv(TEST_DATABASE)) {{
      assertThat(detok.detokenize({java_bytes(data)}))
          {assertion};
    }}
  }}"""


def _java_test_with_args(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    db = tokens.Database(
        tokens.parse_binary(io.BytesIO(shared.DATA_WITH_ARGUMENTS))
    )
    detok = Detokenizer(db)
    if not detok.detokenize(data).ok():
        assertion = ".isNull()"
    else:
        assertion = f".isEqualTo({java_string(expected)})"

    yield f"""\
  @Test
  public void {ctx.java_name()}() {{
    try (Detokenizer detok = Detokenizer.fromCsv(DATA_WITH_ARGUMENTS)) {{
      assertThat(detok.detokenize({java_bytes(data)}))
          {assertion};
    }}
  }}"""


def _java_test_with_collisions(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    db = tokens.Database(
        tokens.parse_binary(io.BytesIO(shared.DATA_WITH_COLLISIONS))
    )
    detok = Detokenizer(db)
    if not detok.detokenize(data).ok():
        assertion = ".isNull()"
    else:
        assertion = f".isEqualTo({java_string(expected)})"

    yield f"""\
  @Test
  public void {ctx.java_name()}() {{
    try (Detokenizer detok = Detokenizer.fromCsv(DATA_WITH_COLLISIONS)) {{
      assertThat(detok.detokenize({java_bytes(data)}))
          {assertion};
    }}
  }}"""


BASIC_TESTS = (_java_test_text, _JAVA_HEADER + _JAVA_DATABASE_DEFS, '')
OPTIONALLY_TOKENIZED_TESTS = (_java_test_optional, '', '')
WITH_ARGS_BINARY_TESTS = (_java_test_with_args, '', '')
WITH_COLLISIONS_TESTS = (_java_test_with_collisions, '', _JAVA_FOOTER)
