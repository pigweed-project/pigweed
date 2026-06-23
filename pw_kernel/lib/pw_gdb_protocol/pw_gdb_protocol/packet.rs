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

use nom::bytes::complete::take_while_m_n;
use nom::character::complete::{char, hex_digit1, one_of};
use nom::combinator::map_res;
use nom::sequence::{preceded, separated_pair};
use nom::{IResult, Parser};

/// Represents a GDB stop reply.
#[derive(Debug, PartialEq, Eq, Clone)]
pub enum StopReply {
    /// The target stopped due to a signal.
    ///
    /// Corresponds to 'T AA' or 'S AA' packets.
    Signal(u8),
    /// The target exited with a status.
    ///
    /// Corresponds to 'W AA' packet.
    Exited(u8),
    /// The target terminated due to a signal.
    ///
    /// Corresponds to 'X AA' packet.
    Terminated(u8),
}

/// Represents the type of breakpoint or watchpoint.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum BreakpointType {
    /// Software breakpoint.
    Software = 0,
    /// Hardware breakpoint.
    Hardware = 1,
    /// Write watchpoint.
    WriteWatchpoint = 2,
    /// Read watchpoint.
    ReadWatchpoint = 3,
    /// Access watchpoint.
    AccessWatchpoint = 4,
}

impl TryFrom<u8> for BreakpointType {
    type Error = &'static str;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(BreakpointType::Software),
            1 => Ok(BreakpointType::Hardware),
            2 => Ok(BreakpointType::WriteWatchpoint),
            3 => Ok(BreakpointType::ReadWatchpoint),
            4 => Ok(BreakpointType::AccessWatchpoint),
            _ => Err("Invalid breakpoint type"),
        }
    }
}

impl core::fmt::Display for StopReply {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            StopReply::Signal(sig) => write!(f, "T{:02x}", sig),
            StopReply::Exited(status) => write!(f, "W{:02x}", status),
            StopReply::Terminated(sig) => write!(f, "X{:02x}", sig),
        }
    }
}

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
    /// A command to continue target execution.
    ///
    /// Format: `c`
    Continue,
    /// A command to insert a breakpoint.
    ///
    /// Format: `Z<type>,<addr>,<kind>`
    InsertBreakpoint {
        t_type: BreakpointType,
        addr: u64,
        kind: u64,
    },
    /// A command to remove a breakpoint.
    ///
    /// Format: `z<type>,<addr>,<kind>`
    RemoveBreakpoint {
        t_type: BreakpointType,
        addr: u64,
        kind: u64,
    },
    /// OK response from GDB server.
    Ok,
    /// Error response from GDB server.
    Error(u8),
    /// Empty response (unsupported command).
    Empty,
    /// Stop reply packet.
    StopReply(StopReply),
}

impl Packet {
    /// Encodes the packet into its string representation (without framing).
    fn encode_payload(&self) -> String {
        match self {
            Packet::ReadMemory { addr, length } => format!("m{:x},{:x}", addr, length),
            Packet::ReadMemoryResponse(data) => hex::encode(data),
            Packet::Continue => "c".to_string(),
            Packet::InsertBreakpoint { t_type, addr, kind } => {
                format!("Z{},{:x},{:x}", *t_type as u8, addr, kind)
            }
            Packet::RemoveBreakpoint { t_type, addr, kind } => {
                format!("z{},{:x},{:x}", *t_type as u8, addr, kind)
            }
            Packet::Ok => "OK".to_string(),
            Packet::Error(code) => format!("E{:02x}", code),
            Packet::Empty => "".to_string(),
            Packet::StopReply(reply) => reply.to_string(),
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
        if input.is_empty() {
            Ok(("", Packet::Empty))
        } else if input == "OK" {
            Ok(("", Packet::Ok))
        } else if input.starts_with('E') && input.len() == 3 {
            if let Ok(code) = u8::from_str_radix(&input[1..3], 16) {
                Ok((&input[3..], Packet::Error(code)))
            } else {
                Err(nom::Err::Error(nom::error::Error::new(
                    input,
                    nom::error::ErrorKind::Fail,
                )))
            }
        } else if let Ok((rem, reply)) = parse_stop_reply(input) {
            Ok((rem, Packet::StopReply(reply)))
        } else if input.starts_with('m') {
            let (rem, (addr, length)) = parse_read_memory(input)?;
            Ok((rem, Packet::ReadMemory { addr, length }))
        } else if input.starts_with('Z') {
            let (rem, (t_type, addr, kind)) = parse_breakpoint(input, 'Z')?;
            Ok((rem, Packet::InsertBreakpoint { t_type, addr, kind }))
        } else if input.starts_with('z') {
            let (rem, (t_type, addr, kind)) = parse_breakpoint(input, 'z')?;
            Ok((rem, Packet::RemoveBreakpoint { t_type, addr, kind }))
        } else if input == "c" {
            Ok(("", Packet::Continue))
        } else {
            // Assume response is hex data
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
    #[allow(clippy::std_instead_of_core)]
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

fn parse_stop_reply(input: &str) -> IResult<&str, StopReply> {
    let (input, kind) = one_of("TSWX")(input)?;
    let (input, val) = map_res(
        take_while_m_n(2usize, 2usize, |c: char| c.is_ascii_hexdigit()),
        |s| u8::from_str_radix(s, 16),
    )(input)?;
    let reply = match kind {
        'T' | 'S' => StopReply::Signal(val),
        'W' => StopReply::Exited(val),
        'X' => StopReply::Terminated(val),
        _ => unreachable!(),
    };
    Ok((input, reply))
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

fn parse_breakpoint(input: &str, prefix: char) -> IResult<&str, (BreakpointType, u64, u64)> {
    use nom::sequence::tuple;
    preceded(
        char(prefix),
        tuple((
            map_res(one_of("01234"), |c| {
                c.to_digit(10)
                    .and_then(|d| u8::try_from(d).ok())
                    .and_then(|d| BreakpointType::try_from(d).ok())
                    .ok_or("invalid type")
            }),
            preceded(
                char(','),
                map_res(hex_digit1, |s| u64::from_str_radix(s, 16)),
            ),
            preceded(
                char(','),
                map_res(hex_digit1, |s| u64::from_str_radix(s, 16)),
            ),
        )),
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

    #[test]
    fn test_encode_continue() {
        let packet = Packet::Continue;
        assert_eq!(packet.encode_payload(), "c");
    }

    #[test]
    fn test_decode_continue() {
        let input = "c";
        let (_, packet) = Packet::decode_payload(input).unwrap();
        assert_eq!(packet, Packet::Continue);
    }

    #[test]
    fn test_encode_insert_breakpoint() {
        let packet = Packet::InsertBreakpoint {
            t_type: BreakpointType::Software,
            addr: 0x1234,
            kind: 4,
        };
        assert_eq!(packet.encode_payload(), "Z0,1234,4");
    }

    #[test]
    fn test_decode_insert_breakpoint() {
        let input = "Z0,1234,4";
        let (_, packet) = Packet::decode_payload(input).unwrap();
        assert_eq!(
            packet,
            Packet::InsertBreakpoint {
                t_type: BreakpointType::Software,
                addr: 0x1234,
                kind: 4,
            }
        );
    }

    #[test]
    fn test_decode_stop_reply() {
        let input = "T05thread:1;";
        let (_, packet) = Packet::decode_payload(input).unwrap();
        assert_eq!(packet, Packet::StopReply(StopReply::Signal(5)));
    }

    #[test]
    fn test_decode_stop_reply_variants() {
        assert_eq!(
            Packet::decode_payload("S05").unwrap().1,
            Packet::StopReply(StopReply::Signal(5))
        );
        assert_eq!(
            Packet::decode_payload("W00").unwrap().1,
            Packet::StopReply(StopReply::Exited(0))
        );
        assert_eq!(
            Packet::decode_payload("X09").unwrap().1,
            Packet::StopReply(StopReply::Terminated(9))
        );
        assert_eq!(
            Packet::decode_payload("T00tnotrun:0;").unwrap(),
            ("tnotrun:0;", Packet::StopReply(StopReply::Signal(0)))
        );
    }
}
