// Copyright 2023 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
use super::*;

mod random_data;
mod single_char;

#[test]
fn encoded_size_correctly_calculates_size() {
    for (input, expected_output) in single_char::test_cases() {
        assert_eq!(encoded_size(input.len()), expected_output.len());
    }
    for (input, expected_output) in random_data::test_cases() {
        assert_eq!(encoded_size(input.len()), expected_output.len());
    }
}

#[test]
fn single_characters_encode_correctly() {
    for (input, expected_output) in single_char::test_cases() {
        let mut output_buffer = vec![0u8; encoded_size(input.len())];
        let encode_len = encode(input, &mut output_buffer).unwrap();
        let output_str = core::str::from_utf8(&output_buffer[0..encode_len]).unwrap();
        assert_eq!(&output_str, &expected_output);
    }
}

#[test]
fn random_data_encodes_correctly() {
    for (input, expected_output) in random_data::test_cases() {
        let mut output_buffer = vec![0u8; encoded_size(input.len())];
        let encode_len = encode(input, &mut output_buffer).unwrap();
        let output_str = core::str::from_utf8(&output_buffer[0..encode_len]).unwrap();
        assert_eq!(&output_str, &expected_output);
    }
}

#[test]
fn too_small_output_buffer_returns_error() {
    let mut output_buffer = vec![0u8; 4];
    let input = b"hi";
    // A 3 byte buffer is too small to encode the output with padding.
    assert_eq!(
        encode(input, &mut output_buffer[0..3]),
        Err(Error::OutOfRange)
    );

    // A 4 byte buffer is just enough space.
    assert_eq!(encode(input, &mut output_buffer[0..4]), Ok(4));
    assert_eq!(&output_buffer, b"aGk=");
}

#[test]
fn random_data_encodes_to_string_correctly() {
    for (input, expected_output) in random_data::test_cases() {
        let mut output_buffer = vec![0u8; encoded_size(input.len())];
        let output_str = encode_str(input, &mut output_buffer).unwrap();
        assert_eq!(&output_str, &expected_output);
    }
}

#[test]
fn examples_from_rfc4648_section_2_encode_correctly() {
    let input = b"foobar";
    let mut output_buffer = vec![0u8; encoded_size(input.len())];

    assert_eq!(encode_str(&input[0..0], &mut output_buffer), Ok(""));
    assert_eq!(encode_str(&input[0..1], &mut output_buffer), Ok("Zg=="));
    assert_eq!(encode_str(&input[0..2], &mut output_buffer), Ok("Zm8="));
    assert_eq!(encode_str(&input[0..3], &mut output_buffer), Ok("Zm9v"));
    assert_eq!(encode_str(&input[0..4], &mut output_buffer), Ok("Zm9vYg=="));
    assert_eq!(encode_str(&input[0..5], &mut output_buffer), Ok("Zm9vYmE="));
    assert_eq!(encode_str(&input[0..6], &mut output_buffer), Ok("Zm9vYmFy"));
}

#[test]
fn decoded_size_correctly_calculates_size() {
    for (expected_decoded_output, encoded_input) in single_char::test_cases() {
        assert_eq!(
            decoded_size(encoded_input.as_bytes()),
            expected_decoded_output.len()
        );
    }
    for (expected_decoded_output, encoded_input) in random_data::test_cases() {
        assert_eq!(
            decoded_size(encoded_input.as_bytes()),
            expected_decoded_output.len()
        );
    }
}

#[test]
fn single_characters_decode_correctly() {
    for (expected_decoded_output, encoded_input) in single_char::test_cases() {
        let mut output_buffer = vec![0u8; max_decoded_size(encoded_input.len())];
        let decode_len = decode(encoded_input.as_bytes(), &mut output_buffer).unwrap();
        assert_eq!(&output_buffer[0..decode_len], expected_decoded_output);
    }
}

#[test]
fn random_data_decodes_correctly() {
    for (expected_decoded_output, encoded_input) in random_data::test_cases() {
        let mut output_buffer = vec![0u8; max_decoded_size(encoded_input.len())];
        let decode_len = decode(encoded_input.as_bytes(), &mut output_buffer).unwrap();
        assert_eq!(&output_buffer[0..decode_len], expected_decoded_output);
    }
}

#[test]
fn examples_from_rfc4648_section_2_decode_correctly() {
    let mut output_buffer = vec![0u8; 6];

    assert_eq!(decode(b"", &mut output_buffer), Ok(0));

    let len = decode(b"Zg==", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"f");

    let len = decode(b"Zm8=", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"fo");

    let len = decode(b"Zm9v", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"foo");

    let len = decode(b"Zm9vYg==", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"foob");

    let len = decode(b"Zm9vYmE=", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"fooba");

    let len = decode(b"Zm9vYmFy", &mut output_buffer).unwrap();
    assert_eq!(&output_buffer[0..len], b"foobar");
}

#[test]
fn invalid_data_returns_error() {
    let mut output_buffer = vec![0u8; 10];

    // Invalid length (not multiple of 4)
    assert_eq!(
        decode(b"Zg=", &mut output_buffer),
        Err(Error::InvalidArgument)
    );

    // Invalid characters
    assert_eq!(
        decode(b"Zg*=", &mut output_buffer),
        Err(Error::InvalidArgument)
    );

    // Invalid padding
    assert_eq!(
        decode(b"Zg===", &mut output_buffer),
        Err(Error::InvalidArgument)
    );
}

#[test]
fn too_small_output_buffer_returns_error_for_decode() {
    let mut output_buffer = vec![0u8; 2];
    let input = b"Zm9v"; // "foo" -> 3 bytes

    assert_eq!(decode(input, &mut output_buffer), Err(Error::OutOfRange));
}
