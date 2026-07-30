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

use clap::{Parser, Subcommand};
use pw_gdb_protocol::Client;
use tokio::net::TcpStream;
use tokio_util::compat::TokioAsyncReadCompatExt;

#[derive(Parser)]
#[command(author, version, about, long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,

    /// GDB server address (default: localhost:1234)
    #[arg(short, long, default_value = "localhost:1234")]
    addr: String,
}

#[derive(Subcommand)]
enum Commands {
    /// Read memory from the target
    ReadMemory {
        /// Start address in hex format (e.g., 1000 or 0x1000)
        #[arg(value_parser = parse_hex_u64)]
        address: u64,

        /// Length in bytes
        #[arg(value_parser = parse_int)]
        length: u64,
    },
    /// Write memory to the target
    WriteMemory {
        /// Start address in hex format (e.g., 1000 or 0x1000)
        #[arg(value_parser = parse_hex_u64)]
        address: u64,

        /// Data to write in hex format in big-endian (e.g., 12abcdef or 0x12abcdef).
        /// Must contain a whole number of bytes (an even number of digits).
        data: String,
    },
    /// Interrupt/stop target execution
    #[command(alias = "control-c")]
    Interrupt,
    /// Continue target execution
    Continue,
}

/// Strips an optional `0x` or `0X` prefix from a hex string.
///
/// If `require_even` is true, also verifies that the resulting hex string has an even
/// number of digits.
///
/// Returns a tuple containing the stripped slice and a boolean indicating whether a prefix
/// was stripped.
fn strip_hex_prefix(s: &str, require_even: bool) -> Result<(&str, bool), String> {
    let (hex_str, has_prefix) =
        if let Some(stripped) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
            (stripped, true)
        } else {
            (s, false)
        };

    if require_even && !hex_str.len().is_multiple_of(2) {
        return Err("Hex string must have an even number of digits".to_string());
    }

    Ok((hex_str, has_prefix))
}

/// Parses a hex string into a `u64`.
///
/// Accepts an optional `0x` or `0X` prefix.
fn parse_hex_u64(s: &str) -> Result<u64, String> {
    let (hex_str, _) = strip_hex_prefix(s, false)?;
    u64::from_str_radix(hex_str, 16).map_err(|e| e.to_string())
}

/// Parses an integer string into a `u64`.
///
/// Accepts either a decimal string or a hex string prefixed with `0x` or `0X`.
fn parse_int(s: &str) -> Result<u64, String> {
    let (str_to_parse, is_hex) = strip_hex_prefix(s, false)?;
    if is_hex {
        u64::from_str_radix(str_to_parse, 16).map_err(|e| e.to_string())
    } else {
        str_to_parse.parse::<u64>().map_err(|e| e.to_string())
    }
}

/// Parses a hex string into a byte vector.
///
/// Accepts optional `0x` or `0X` prefix. The hex string must contain an even
/// number of digits.
///
/// Parsing is performed in big-endian byte order, where the
/// first pair of hex digits corresponds to the first byte in the output vector
/// (e.g., `0102` -> `[0x01, 0x02]`).
fn parse_hex_bytes(s: &str) -> Result<Vec<u8>, String> {
    let (hex_str, _) = strip_hex_prefix(s, true)?;
    hex::decode(hex_str).map_err(|e| e.to_string())
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn core::error::Error>> {
    let cli = Cli::parse();

    let stream = TcpStream::connect(&cli.addr).await?;
    let compat_stream = stream.compat();
    let mut client = Client::new(compat_stream);

    match cli.command {
        Commands::ReadMemory { address, length } => {
            let data = client.read_memory(address, length).await?;
            println!("{}", hex::encode(data));
        }
        Commands::WriteMemory { address, data } => {
            let bytes = parse_hex_bytes(&data)?;
            client.write_memory(address, &bytes).await?;
            println!(
                "Successfully wrote {} bytes to 0x{:x}",
                bytes.len(),
                address
            );
        }
        Commands::Interrupt => {
            let stop_reply = client.interrupt().await?;
            println!("Target stopped: {}", stop_reply);
        }
        Commands::Continue => {
            client.continue_execution().await?;
            println!("Target resumed");
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_hex_u64() {
        assert_eq!(parse_hex_u64("10").unwrap(), 16);
        assert_eq!(parse_hex_u64("0x10").unwrap(), 16);
        assert_eq!(parse_hex_u64("0X10").unwrap(), 16);
        assert_eq!(parse_hex_u64("deadbeef").unwrap(), 0xdeadbeef);
        assert_eq!(parse_hex_u64("0xDEADBEEF").unwrap(), 0xdeadbeef);
        parse_hex_u64("invalid").unwrap_err();
        parse_hex_u64("0x12g4").unwrap_err();
    }

    #[test]
    fn test_parse_int() {
        assert_eq!(parse_int("10").unwrap(), 10);
        assert_eq!(parse_int("0x10").unwrap(), 16);
        assert_eq!(parse_int("0X10").unwrap(), 16);
        parse_int("invalid").unwrap_err();
    }

    #[test]
    fn test_parse_hex_bytes() {
        assert_eq!(parse_hex_bytes("1234").unwrap(), vec![0x12, 0x34]);
        assert_eq!(parse_hex_bytes("0x1234").unwrap(), vec![0x12, 0x34]);
        assert_eq!(parse_hex_bytes("0X1234").unwrap(), vec![0x12, 0x34]);

        // Alphanumeric hex digits (uppercase, lowercase, mixed)
        assert_eq!(parse_hex_bytes("abcd").unwrap(), vec![0xab, 0xcd]);
        assert_eq!(parse_hex_bytes("0xABCD").unwrap(), vec![0xab, 0xcd]);
        assert_eq!(
            parse_hex_bytes("0X1a2B3c4D").unwrap(),
            vec![0x1a, 0x2b, 0x3c, 0x4d]
        );
        assert_eq!(
            parse_hex_bytes("deadbeef").unwrap(),
            vec![0xde, 0xad, 0xbe, 0xef]
        );

        // Odd number of digits should fail
        parse_hex_bytes("1").unwrap_err();
        parse_hex_bytes("123").unwrap_err();
        parse_hex_bytes("0x123").unwrap_err();
        parse_hex_bytes("0Xabc").unwrap_err();

        // Invalid hex characters should fail
        parse_hex_bytes("invalid").unwrap_err();
        parse_hex_bytes("0x12g4").unwrap_err();
    }
}
