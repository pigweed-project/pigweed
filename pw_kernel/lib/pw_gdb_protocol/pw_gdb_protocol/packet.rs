// Copyright 2025 The Pigweed Authors
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

use nom::character::complete::{char, hex_digit1};
use nom::combinator::map_res;
use nom::sequence::{preceded, separated_pair};
use nom::{IResult, Parser};

/// Represents a GDB remote protocol packet.
#[derive(Debug, PartialEq, Eq)]
pub enum Packet {
    /// A command to read memory from the target.
    ///
    /// Format: `m<addr>,<length>`
    ReadMemory { addr: u64, length: u64 },
    /// A response containing the memory contents.
    ///
    /// Format: `<hex_data>`
    ReadMemoryResponse(Vec<u8>),
}

impl Packet {
    /// Encodes the packet into its string representation (without framing).
    fn encode_payload(&self) -> String {
        match self {
            Packet::ReadMemory { addr, length } => format!("m{:x},{:x}", addr, length),
            Packet::ReadMemoryResponse(data) => hex::encode(data),
        }
    }

    /// Encodes the packet with GDB framing (start character, checksum, etc.).
    ///
    /// Format: `$<payload>#<checksum>`
    pub fn encode(&self) -> String {
        let payload = self.encode_payload();
        let checksum = Self::calculate_checksum(payload.as_bytes());
        format!("${}#{:02x}", payload, checksum)
    }

    /// Calculates the GDB checksum for the given data.
    ///
    /// The checksum is the sum of all bytes modulo 256.
    pub fn calculate_checksum(data: &[u8]) -> u8 {
        data.iter().fold(0, |acc, &x| acc.wrapping_add(x))
    }

    /// Decodes a packet from its string representation (without framing).
    pub fn decode_payload(input: &str) -> IResult<&str, Packet> {
        if input.starts_with('m') {
            let (rem, (addr, length)) = parse_read_memory(input)?;
            Ok((rem, Packet::ReadMemory { addr, length }))
        } else {
            // Assume response is hex data
            // We use map_res to convert the hex string to Vec<u8>
            // nom's hex_digit1 will consume all hex characters
            let (rem, hex_data) = hex_digit1(input)?;
            let data = hex::decode(hex_data).map_err(|_| {
                nom::Err::Failure(nom::error::Error::new(input, nom::error::ErrorKind::Fail))
            })?;
            Ok((rem, Packet::ReadMemoryResponse(data)))
        }
    }

    /// Calculates the maximum data length for a given total packet size.
    ///
    /// The total packet size includes the overhead of the GDB protocol framing:
    /// - 1 byte for '$'
    /// - 1 byte for '#'
    /// - 2 bytes for checksum
    /// - 2 characters of hex encoding for each byte of data
    ///
    /// Returns an error if total_size is too small to hold any data.
    pub fn max_payload_size(max_packet_size: usize) -> std::io::Result<usize> {
        if max_packet_size <= 4 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "Total packet size is too small to hold any data",
            ));
        }
        Ok((max_packet_size - 4) / 2)
    }
}

fn parse_read_memory(input: &str) -> IResult<&str, (u64, u64)> {
    preceded(
        char('m'),
        separated_pair(
            map_res(hex_digit1, |s| u64::from_str_radix(s, 16)),
            char(','),
            map_res(hex_digit1, |s| u64::from_str_radix(s, 16)),
        ),
    )
    .parse(input)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_encode_read_memory() {
        let packet = Packet::ReadMemory {
            addr: 0x1234,
            length: 0x10,
        };
        assert_eq!(packet.encode_payload(), "m1234,10");
    }

    #[test]
    fn test_decode_read_memory() {
        let input = "m1234,10";
        let (_, packet) = Packet::decode_payload(input).unwrap();
        assert_eq!(
            packet,
            Packet::ReadMemory {
                addr: 0x1234,
                length: 0x10
            }
        );
    }

    const TEST_PAYLOAD: &[u8] = &[0xde, 0xca, 0xfb, 0xad];
    const TEST_PAYLOAD_STR: &str = "decafbad";

    #[test]
    fn test_encode_read_memory_response() {
        let packet = Packet::ReadMemoryResponse(TEST_PAYLOAD.to_vec());
        assert_eq!(packet.encode_payload(), TEST_PAYLOAD_STR);
    }

    #[test]
    fn test_decode_read_memory_response() {
        let input = TEST_PAYLOAD_STR;
        let (_, packet) = Packet::decode_payload(input).unwrap();
        assert_eq!(packet, Packet::ReadMemoryResponse(TEST_PAYLOAD.to_vec()));
    }
}
