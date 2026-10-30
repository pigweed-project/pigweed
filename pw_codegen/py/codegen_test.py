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
"""Tests for language-independent pw_codegen utilities."""

import unittest

from pw_codegen import (
    OutputFile,
    camel_to_snake,
    camel_to_upper_snake,
    snake_to_pascal,
)


class CaseConversionTest(unittest.TestCase):
    """Tests identifier case-conversion helpers."""

    def test_camel_to_snake(self) -> None:
        self.assertEqual(camel_to_snake("MyStep"), "my_step")
        self.assertEqual(camel_to_snake("myStep"), "my_step")
        self.assertEqual(camel_to_snake("HTTPServer"), "http_server")
        self.assertEqual(camel_to_snake("JSONParser"), "json_parser")
        self.assertEqual(camel_to_snake("Http2Error"), "http2_error")
        self.assertEqual(camel_to_snake("already_snake"), "already_snake")
        self.assertEqual(camel_to_snake("_Leading"), "_leading")
        self.assertEqual(camel_to_snake("_FooBar"), "_foo_bar")
        self.assertEqual(camel_to_snake("Foo_BarBaz"), "foo_bar_baz")

    def test_camel_to_upper_snake(self) -> None:
        self.assertEqual(camel_to_upper_snake("MyEnum"), "MY_ENUM")
        self.assertEqual(camel_to_upper_snake("myEnum"), "MY_ENUM")
        self.assertEqual(camel_to_upper_snake("HTTPServer"), "HTTP_SERVER")
        self.assertEqual(camel_to_upper_snake("JSONParser"), "JSON_PARSER")
        self.assertEqual(camel_to_upper_snake("URL"), "URL")
        self.assertEqual(camel_to_upper_snake("Http2Error"), "HTTP2_ERROR")
        self.assertEqual(camel_to_upper_snake("ALREADY_SNAKE"), "ALREADY_SNAKE")
        self.assertEqual(camel_to_upper_snake("_Leading"), "_LEADING")
        self.assertEqual(camel_to_upper_snake("pw_MyEnum"), "PW_MY_ENUM")

    def test_snake_to_pascal(self) -> None:
        self.assertEqual(snake_to_pascal("my_field"), "MyField")
        self.assertEqual(snake_to_pascal("IO_ERROR"), "IoError")
        self.assertEqual(snake_to_pascal("HTTP2_ERROR"), "Http2Error")
        self.assertEqual(snake_to_pascal("V1_2"), "V12")
        self.assertEqual(snake_to_pascal("single"), "Single")
        self.assertEqual(snake_to_pascal(""), "")

    def test_reexported_output_file(self) -> None:
        output = OutputFile("test.h")
        output.write_line("#pragma once")
        self.assertEqual(output.content(), "#pragma once\n")


if __name__ == "__main__":
    unittest.main()
