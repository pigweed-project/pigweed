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
"""Generates Rust data-driven detokenizer test code."""

import io
from typing import Iterator

from pw_build.generated_tests import (
    Context,
    rust_bytes,
    rust_string,
)
from pw_tokenizer import tokens
from pw_tokenizer.detokenize import Detokenizer
import detokenize_test_cases as shared

_RUST_HEADER = """\
#![cfg(feature = "std")]

use pw_tokenizer::detokenize::Detokenizer;
"""

_TEST_DB = Detokenizer(
    tokens.Database(tokens.parse_binary(io.BytesIO(shared.TEST_DATABASE)))
)
_TEST_DB_STR = rust_string(str(_TEST_DB.database))

_ARGS_DB = Detokenizer(
    tokens.Database(tokens.parse_binary(io.BytesIO(shared.DATA_WITH_ARGUMENTS)))
)
_ARGS_DB_STR = rust_string(str(_ARGS_DB.database))

_COLLISIONS_DB = Detokenizer(
    tokens.Database(
        tokens.parse_binary(io.BytesIO(shared.DATA_WITH_COLLISIONS))
    )
)
_COLLISIONS_DB_STR = rust_string(str(_COLLISIONS_DB.database))

_RUST_DATABASE_DEFS = f"""\
const TEST_DATABASE: &str = {_TEST_DB_STR};
const DATA_WITH_ARGUMENTS: &str = {_ARGS_DB_STR};
const DATA_WITH_COLLISIONS: &str = {_COLLISIONS_DB_STR};
"""


def _rust_test_text(ctx: Context) -> Iterator[str]:
    data, expected = ctx.test_case
    yield f"""\
    #[test]
    fn {ctx.py_name()}() {{
        let detok = Detokenizer::from_csv(TEST_DATABASE).unwrap();
        assert_eq!(
            detok.detokenize_text({rust_string(data)}),
            {rust_string(expected)}
        );
    }}"""


def _generate_rust_detokenize_test(
    ctx: Context, detok: Detokenizer, database_const_ident: str
) -> Iterator[str]:
    data, expected = ctx.test_case
    if not detok.detokenize(data).ok():
        assertions = "assert!(!res.is_ok);"
    else:
        assertions = f"""assert!(res.is_ok);
        assert_eq!(res.best_string(), {rust_string(expected)});"""
    yield f"""\
    #[test]
    fn {ctx.py_name()}() {{
        let detok = Detokenizer::from_csv({database_const_ident}).unwrap();
        let res = detok.detokenize({rust_bytes(data)});
        {assertions}
    }}"""


def _rust_test_optional(ctx: Context) -> Iterator[str]:
    return _generate_rust_detokenize_test(ctx, _TEST_DB, "TEST_DATABASE")


def _rust_test_with_args(ctx: Context) -> Iterator[str]:
    return _generate_rust_detokenize_test(ctx, _ARGS_DB, "DATA_WITH_ARGUMENTS")


def _rust_test_with_collisions(ctx: Context) -> Iterator[str]:
    return _generate_rust_detokenize_test(
        ctx, _COLLISIONS_DB, "DATA_WITH_COLLISIONS"
    )


BASIC_TESTS = (
    _rust_test_text,
    _RUST_HEADER
    + _RUST_DATABASE_DEFS
    + "mod basic_tests {\n    use super::*;\n",
    "}\n",
)
OPTIONALLY_TOKENIZED_TESTS = (
    _rust_test_optional,
    "mod optionally_tokenized_tests {\n    use super::*;\n",
    "}\n",
)
WITH_ARGS_BINARY_TESTS = (
    _rust_test_with_args,
    "mod with_args_binary_tests {\n    use super::*;\n",
    "}\n",
)
WITH_COLLISIONS_TESTS = (
    _rust_test_with_collisions,
    "mod with_collisions_tests {\n    use super::*;\n",
    "}\n",
)
