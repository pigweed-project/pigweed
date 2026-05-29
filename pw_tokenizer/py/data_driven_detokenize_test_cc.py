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
"""Generates C++ data-driven detokenizer test code."""

from typing import Iterator

from pw_build.generated_tests import Context, cc_string
import detokenize_test_cases as shared

_CPP_HEADER = """
#include "pw_tokenizer/detokenize.h"

#include <string_view>

#include "pw_bytes/array.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace std::literals::string_view_literals;
"""

_CPP_FOOTER = """
}  // namespace
"""

_CPP_DATABASE_DEFS = f"""
constexpr char kTestDatabaseRaw[] = {cc_string(shared.TEST_DATABASE)};
constexpr pw::tokenizer::TokenDatabase kTestDatabase =
    pw::tokenizer::TokenDatabase::Create<kTestDatabaseRaw>();

constexpr char kDataWithArgumentsRaw[] =
    {cc_string(shared.DATA_WITH_ARGUMENTS)};
constexpr pw::tokenizer::TokenDatabase kDataWithArguments =
    pw::tokenizer::TokenDatabase::Create<kDataWithArgumentsRaw>();

constexpr char kDataWithCollisionsRaw[] =
    {cc_string(shared.DATA_WITH_COLLISIONS)};
constexpr pw::tokenizer::TokenDatabase kDataWithCollisions =
    pw::tokenizer::TokenDatabase::Create<kDataWithCollisionsRaw>();
"""


def _cpp_test_text(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
TEST(DetokenizeTest, {ctx.cc_name()}) {{
  const pw::tokenizer::Detokenizer detok(kTestDatabase);
  EXPECT_EQ(detok.DetokenizeText({cc_string(data)}sv), {cc_string(expected)});
}}"""


def _cpp_test_optional(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
TEST(DetokenizeOptionalTest, {ctx.cc_name()}) {{
  const pw::tokenizer::Detokenizer detok(kTestDatabase);
  constexpr std::string_view data = {cc_string(data)}sv;
  EXPECT_EQ(
      detok.DecodeOptionallyTokenizedData(pw::as_bytes(pw::span(data))),
      {cc_string(expected)}
  );
}}"""


def _cpp_test_with_args(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
TEST(DetokenizeWithArgsBinTest, {ctx.cc_name()}) {{
  const pw::tokenizer::Detokenizer detok(kDataWithArguments);
  constexpr std::string_view data = {cc_string(data)}sv;
  EXPECT_EQ(detok.Detokenize(pw::as_bytes(pw::span(data))).BestString(),
  {cc_string(expected)});
}}"""


def _cpp_test_with_collisions(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
TEST(DetokenizeWithCollisionsTest, {ctx.cc_name()}) {{
  const pw::tokenizer::Detokenizer detok(kDataWithCollisions);
  constexpr std::string_view data = {cc_string(data)}sv;
  EXPECT_EQ(
      detok.Detokenize(pw::as_bytes(pw::span(data))).BestString(),
      {cc_string(expected)}
  );
}}"""


BASIC_TESTS = (_cpp_test_text, _CPP_HEADER + _CPP_DATABASE_DEFS, '')
OPTIONALLY_TOKENIZED_TESTS = (_cpp_test_optional, '', '')
WITH_ARGS_BINARY_TESTS = (_cpp_test_with_args, '', '')
WITH_COLLISIONS_TESTS = (_cpp_test_with_collisions, '', _CPP_FOOTER)
