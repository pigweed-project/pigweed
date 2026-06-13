# Copyright 2025 The Pigweed Authors
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
"""Runs Python-based data-driven tests for detokenize."""

import io

from pw_build.generated_tests import TestGenerator, PyTest, Context, main
from pw_tokenizer import tokens
from pw_tokenizer.detokenize import Detokenizer

from detokenize_test_cases import (
    TEST_CASES,
    OPTIONALLY_TOKENIZED_TEST_CASES,
    WITH_ARGS_SUCCESSFUL_CASES_BINARY,
    WITH_ARGS_STAR_CASES_BINARY,
    WITH_ARGS_PERCENT_G_CASES_BINARY,
    WITH_ARGS_PERCENT_E_CASES_BINARY,
    WITH_COLLISIONS_CASES_BINARY,
    TEST_DATABASE,
    DATA_WITH_ARGUMENTS,
    DATA_WITH_COLLISIONS,
)
import data_driven_detokenize_test_cc as cc
import data_driven_detokenize_test_java as java
import data_driven_detokenize_test_rust as rust

_BASIC_TESTS = TestGenerator(
    TEST_CASES,
    cc_test=cc.BASIC_TESTS,
    java_test=java.BASIC_TESTS,
    rust_test=rust.BASIC_TESTS,
)
_OPTIONALLY_TOKENIZED_TESTS = TestGenerator(
    OPTIONALLY_TOKENIZED_TEST_CASES,
    cc_test=cc.OPTIONALLY_TOKENIZED_TESTS,
    java_test=java.OPTIONALLY_TOKENIZED_TESTS,
    rust_test=rust.OPTIONALLY_TOKENIZED_TESTS,
)
_WITH_ARGS_BINARY_TESTS = TestGenerator(
    WITH_ARGS_SUCCESSFUL_CASES_BINARY,
    cc_test=cc.WITH_ARGS_BINARY_TESTS,
    java_test=java.WITH_ARGS_BINARY_TESTS,
    rust_test=rust.WITH_ARGS_BINARY_TESTS,
)
_WITH_ARGS_STAR_TESTS = TestGenerator(
    WITH_ARGS_STAR_CASES_BINARY,
    cc_test=cc.WITH_ARGS_BINARY_TESTS,
    java_test=java.WITH_ARGS_BINARY_TESTS,
    # TODO: b/523305904 - Support * width and precision in Rust.
    rust_test=rust.skip_test('with_args_star_tests'),
)
_WITH_ARGS_PERCENT_G_TESTS = TestGenerator(
    WITH_ARGS_PERCENT_G_CASES_BINARY,
    cc_test=cc.WITH_ARGS_BINARY_TESTS,
    java_test=java.WITH_ARGS_BINARY_TESTS,
    # TODO: b/523306892 - Support %g and %G format specifiers in Rust.
    rust_test=rust.skip_test('with_args_percent_g_tests'),
)
_WITH_ARGS_PERCENT_E_TESTS = TestGenerator(
    WITH_ARGS_PERCENT_E_CASES_BINARY,
    cc_test=cc.WITH_ARGS_BINARY_TESTS,
    java_test=java.WITH_ARGS_BINARY_TESTS,
    # TODO: b/352358580 - Support %e and %E format specifiers in Rust.
    rust_test=rust.skip_test('with_args_percent_e_tests'),
)
_WITH_COLLISIONS_TESTS = TestGenerator(
    WITH_COLLISIONS_CASES_BINARY,
    cc_test=cc.WITH_COLLISIONS_TESTS,
    java_test=java.WITH_COLLISIONS_TESTS,
    rust_test=rust.WITH_COLLISIONS_TESTS,
)


def _define_py_test_text(detokenizer: Detokenizer, ctx: Context) -> PyTest:
    """Defines a Python detokenizer test for text data."""
    data, expected = ctx.test_case

    def test(self) -> None:
        self.assertEqual(detokenizer.detokenize_text(data), expected)

    return test


def _define_py_test_binary(detokenizer: Detokenizer, ctx: Context) -> PyTest:
    """Defines a Python detokenizer test for data."""
    data, expected = ctx.test_case

    def test(self) -> None:
        result = detokenizer.detokenize(data).best_result()
        if result is None:
            self.assertEqual(
                data.decode(errors='ignore'), expected, f'Detokenizing {data}'
            )
        else:
            self.assertEqual(result.value, expected, f'Detokenizing {data}')

    return test


DetokenizeTest = _BASIC_TESTS.python_tests(
    'DetokenizeTest',
    lambda ctx: _define_py_test_text(
        Detokenizer(
            tokens.Database(tokens.parse_binary(io.BytesIO(TEST_DATABASE)))
        ),
        ctx,
    ),
)

DetokenizeOptionalTest = _OPTIONALLY_TOKENIZED_TESTS.python_tests(
    'DetokenizeOptionalTest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(tokens.parse_binary(io.BytesIO(TEST_DATABASE)))
        ),
        ctx,
    ),
)

DetokenizeWithArgsBinTest = _WITH_ARGS_BINARY_TESTS.python_tests(
    'DetokenizeWithArgsBinTest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(
                tokens.parse_binary(io.BytesIO(DATA_WITH_ARGUMENTS))
            )
        ),
        ctx,
    ),
)

DetokenizeWithArgsStarTest = _WITH_ARGS_STAR_TESTS.python_tests(
    'DetokenizeWithArgsStarTest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(
                tokens.parse_binary(io.BytesIO(DATA_WITH_ARGUMENTS))
            )
        ),
        ctx,
    ),
)

DetokenizeWithArgsPercentGTest = _WITH_ARGS_PERCENT_G_TESTS.python_tests(
    'DetokenizeWithArgsPercentGTest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(
                tokens.parse_binary(io.BytesIO(DATA_WITH_ARGUMENTS))
            )
        ),
        ctx,
    ),
)

DetokenizeWithArgsPercentETest = _WITH_ARGS_PERCENT_E_TESTS.python_tests(
    'DetokenizeWithArgsPercentETest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(
                tokens.parse_binary(io.BytesIO(DATA_WITH_ARGUMENTS))
            )
        ),
        ctx,
    ),
)

DetokenizeWithCollisionsTest = _WITH_COLLISIONS_TESTS.python_tests(
    'DetokenizeWithCollisionsTest',
    lambda ctx: _define_py_test_binary(
        Detokenizer(
            tokens.Database(
                tokens.parse_binary(io.BytesIO(DATA_WITH_COLLISIONS))
            )
        ),
        ctx,
    ),
)


if __name__ == '__main__':
    main(
        _BASIC_TESTS,
        _OPTIONALLY_TOKENIZED_TESTS,
        _WITH_ARGS_BINARY_TESTS,
        _WITH_ARGS_STAR_TESTS,
        _WITH_ARGS_PERCENT_G_TESTS,
        _WITH_ARGS_PERCENT_E_TESTS,
        _WITH_COLLISIONS_TESTS,
    )
