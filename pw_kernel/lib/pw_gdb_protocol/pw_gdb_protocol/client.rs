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

use std::io;

use futures::io::{AsyncBufReadExt, AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, BufReader};

use crate::packet::{BreakpointType, Packet, StopReply};

/// A client for interacting with a GDB server.
pub struct Client<S> {
    stream: BufReader<S>,
    max_packet_size: usize,
}

impl<S: AsyncRead + AsyncWrite + Unpin> Client<S> {
    /// Creates a new `Client` with the given stream.
    ///
    /// Max packet size defaults to 400 bytes per the max packet size mentioned
    /// at https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html.
    pub fn new(stream: S) -> Self {
        Self {
            stream: BufReader::new(stream),
            max_packet_size: 400,
        }
    }

    /// Sets the maximum packet size in bytes.
    pub fn set_max_packet_size(&mut self, size: usize) {
        self.max_packet_size = size;
    }

    /// Reads memory from the target at the specified address and length.
    ///
    /// Sends one or more `m` packets and waits for the responses.
    pub async fn read_memory(&mut self, addr: u64, length: u64) -> io::Result<Vec<u8>> {
        let length = usize::try_from(length)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "Length is too large"))?;

        let max_chunk_size = Packet::max_payload_size(self.max_packet_size)?;

        let mut data = Vec::with_capacity(length);
        let mut current_addr = addr;
        let mut remaining_length = length;

        while remaining_length > 0 {
            let chunk_length = core::cmp::min(remaining_length, max_chunk_size);
            self.send_packet(&Packet::ReadMemory {
                addr: current_addr,
                length: chunk_length as u64,
            })
            .await?;

            let response = self.receive_packet().await?;
            let Packet::ReadMemoryResponse(chunk_data) = response else {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Invalid response",
                ));
            };

            if chunk_data.len() != chunk_length {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "ReadMemoryResponse has incorrect length",
                ));
            }

            data.extend(chunk_data);
            current_addr += chunk_length as u64;
            remaining_length -= chunk_length;
        }
        Ok(data)
    }

    /// Sets a software breakpoint at the specified address.
    ///
    /// The `kind` parameter is target-specific and architecture-dependent. In most
    /// implementations, it represents the size of the breakpoint in bytes (e.g.,
    /// the length of the instruction to be replaced).
    ///
    /// For example:
    /// - ARM: 2 for 16-bit Thumb mode, 3 for 32-bit Thumb-2 mode, or 4 for 32-bit ARM mode.
    /// - MIPS: 2 for MIPS16 mode, or 4 for 32-bit MIPS mode.
    /// - RISC-V: 2 for 16-bit compressed instruction mode, or 4 for 32-bit instruction mode.
    pub async fn insert_software_breakpoint(&mut self, addr: u64, kind: u64) -> io::Result<()> {
        self.send_packet(&Packet::InsertBreakpoint {
            t_type: BreakpointType::Software,
            addr,
            kind,
        })
        .await?;

        let response = self.receive_packet().await?;
        match response {
            Packet::Ok => Ok(()),
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            Packet::Empty => Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "GDB server does not support software breakpoints",
            )),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unexpected response to insert breakpoint",
            )),
        }
    }

    /// Removes a software breakpoint at the specified address.
    ///
    /// The `kind` parameter must match the `kind` used when the breakpoint was inserted.
    /// It is target-specific and architecture-dependent, typically representing the size
    /// of the breakpoint instruction in bytes.
    pub async fn remove_software_breakpoint(&mut self, addr: u64, kind: u64) -> io::Result<()> {
        self.send_packet(&Packet::RemoveBreakpoint {
            t_type: BreakpointType::Software,
            addr,
            kind,
        })
        .await?;

        let response = self.receive_packet().await?;
        match response {
            Packet::Ok => Ok(()),
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            Packet::Empty => Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "GDB server does not support software breakpoints",
            )),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unexpected response to remove breakpoint",
            )),
        }
    }

    /// Continues target execution and waits for a stop reply.
    pub async fn continue_execution(&mut self) -> io::Result<StopReply> {
        self.send_packet(&Packet::Continue).await?;

        let response = self.receive_packet().await?;
        match response {
            Packet::StopReply(reply) => Ok(reply),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unexpected response to continue execution",
            )),
        }
    }

    async fn send_packet(&mut self, packet: &Packet) -> io::Result<()> {
        let frame = packet.encode();
        self.stream.write_all(frame.as_bytes()).await?;
        self.stream.flush().await?;
        self.wait_for_ack().await
    }

    async fn wait_for_ack(&mut self) -> io::Result<()> {
        let mut byte = [0u8; 1];
        loop {
            self.stream.read_exact(&mut byte).await?;
            match byte[0] {
                b'+' => return Ok(()),
                b'-' => {
                    return Err(io::Error::other("Received NACK from server"));
                }
                _ => continue, // Ignore garbage
            }
        }
    }

    async fn receive_packet(&mut self) -> io::Result<Packet> {
        // Skip non-framed data until '$'
        let mut buffer = Vec::new();
        loop {
            let bytes_read = self.stream.read_until(b'$', &mut buffer).await?;
            if bytes_read == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "Connection closed",
                ));
            }
            if buffer.last() == Some(&b'$') {
                break;
            }
        }

        // Read payload until '#'
        let mut payload_bytes = Vec::new();
        let bytes_read = self.stream.read_until(b'#', &mut payload_bytes).await?;
        if bytes_read == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "Connection closed while reading payload",
            ));
        }
        if payload_bytes.last() != Some(&b'#') {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Missing packet terminator",
            ));
        }
        payload_bytes.pop(); // Remove '#'

        // Read checksum (2 bytes)
        let mut checksum_buf = [0u8; 2];
        self.stream.read_exact(&mut checksum_buf).await?;

        // Verify checksum
        let received_checksum_str = core::str::from_utf8(&checksum_buf)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        let received_checksum = u8::from_str_radix(received_checksum_str, 16)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;

        let calculated_checksum = Packet::calculate_checksum(&payload_bytes);
        if received_checksum != calculated_checksum {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Checksum mismatch",
            ));
        }

        // Send ACK '+'
        self.stream.write_all(b"+").await?;
        self.stream.flush().await?;

        let payload_str = String::from_utf8(payload_bytes)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;

        let (_, packet) = Packet::decode_payload(&payload_str)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;

        Ok(packet)
    }
}

#[cfg(test)]
mod tests {
    use core::pin::Pin;
    use std::collections::VecDeque;

    use futures::task::{Context, Poll};

    use super::*;

    // Mock stream for testing
    struct MockStream {
        read_data: VecDeque<u8>,
        write_data: Vec<u8>,
    }

    impl MockStream {
        fn new(read_data: Vec<u8>) -> Self {
            Self {
                read_data: read_data.into(),
                write_data: Vec::new(),
            }
        }
    }

    impl AsyncRead for MockStream {
        fn poll_read(
            mut self: Pin<&mut Self>,
            _cx: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            if self.read_data.is_empty() {
                return Poll::Ready(Ok(0));
            }
            let n = core::cmp::min(buf.len(), self.read_data.len());
            for item in buf.iter_mut().take(n) {
                *item = self.read_data.pop_front().unwrap();
            }
            Poll::Ready(Ok(n))
        }
    }

    impl AsyncWrite for MockStream {
        fn poll_write(
            mut self: Pin<&mut Self>,
            _cx: &mut Context<'_>,
            buf: &[u8],
        ) -> Poll<io::Result<usize>> {
            self.write_data.extend_from_slice(buf);
            Poll::Ready(Ok(buf.len()))
        }

        fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }

        fn poll_close(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    #[tokio::test]
    async fn test_read_memory() {
        const TEST_PAYLOAD_HEX: &[u8] = b"decafbad";
        const TEST_PAYLOAD_BYTES: &[u8] = &[0xde, 0xca, 0xfb, 0xad];

        let response_payload = TEST_PAYLOAD_HEX;
        let checksum = Packet::calculate_checksum(response_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);

        let result = client.read_memory(0x1000, 4).await.unwrap();
        assert_eq!(result, TEST_PAYLOAD_BYTES);

        let expected_sent = b"$m1000,4#8e+"; // + is ACK for response
        assert_eq!(stream.write_data, expected_sent);
    }

    #[tokio::test]
    async fn test_read_memory_chunking() {
        // limit = 6 bytes -> max data len = (6-4)/2 = 1 byte
        // We want to read 2 bytes: [0xaa, 0xbb]

        let chunk1_payload = b"aa"; // Hex encoding of 0xaa
        let chunk2_payload = b"bb"; // Hex encoding of 0xbb

        // Packet 1 response
        let c1_sum = Packet::calculate_checksum(chunk1_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(chunk1_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", c1_sum).as_bytes());

        // Packet 2 response
        let c2_sum = Packet::calculate_checksum(chunk2_payload);
        input.push(b'+'); // ACK for 2nd command
        input.push(b'$');
        input.extend_from_slice(chunk2_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", c2_sum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);
        client.set_max_packet_size(6);

        let result = client.read_memory(0x1000, 2).await.unwrap();
        assert_eq!(result, &[0xaa, 0xbb]);

        // Verify sent packets
        // Packet 1: $m1000,1#...
        // Packet 2: $m1001,1#...

        let p1 = Packet::ReadMemory {
            addr: 0x1000,
            length: 1,
        };
        let p2 = Packet::ReadMemory {
            addr: 0x1001,
            length: 1,
        };

        let mut expected = Vec::new();
        // Packet 1
        expected.extend_from_slice(p1.encode().as_bytes());
        // ACK for Response 1
        expected.push(b'+');
        // Packet 2
        expected.extend_from_slice(p2.encode().as_bytes());
        // ACK for Response 2
        expected.push(b'+');

        assert_eq!(stream.write_data, expected);
    }

    #[tokio::test]
    async fn test_insert_remove_software_breakpoint() {
        // Test insert
        let response = b"+$OK#9a";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);
        client.insert_software_breakpoint(0x1000, 4).await.unwrap();
        assert_eq!(stream.write_data, b"$Z0,1000,4#d7+");

        // Test remove
        let response = b"+$OK#9a";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);
        client.remove_software_breakpoint(0x1000, 4).await.unwrap();
        assert_eq!(stream.write_data, b"$z0,1000,4#f7+");
    }

    #[tokio::test]
    async fn test_continue_execution() {
        let response = b"+$S02#b5";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);

        // Suppose the system under test is paused
        // we continue, and then it stops due to a SIGINT
        let stop_reason = client.continue_execution().await.unwrap();

        assert_eq!(stop_reason, StopReply::Signal(2));
        assert_eq!(stream.write_data, b"$c#63+");
    }
}
