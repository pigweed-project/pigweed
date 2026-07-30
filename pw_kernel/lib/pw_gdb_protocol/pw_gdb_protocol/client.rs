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
    qemu_physical_memory_mode: bool,
    has_arm_trustzone: bool,
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
            qemu_physical_memory_mode: false,
            has_arm_trustzone: false,
        }
    }

    /// Sets the maximum packet size in bytes.
    pub fn set_max_packet_size(&mut self, size: usize) {
        self.max_packet_size = size;
    }

    /// Returns whether QEMU physical memory mode is currently enabled.
    pub fn qemu_physical_memory_mode(&self) -> bool {
        self.qemu_physical_memory_mode
    }

    /// Returns whether ARM TrustZone memory aliasing support is enabled.
    pub fn has_arm_trustzone(&self) -> bool {
        self.has_arm_trustzone
    }

    /// Sets whether ARM TrustZone memory aliasing support is enabled.
    pub fn set_has_arm_trustzone(&mut self, has_arm_trustzone: bool) {
        self.has_arm_trustzone = has_arm_trustzone;
    }

    /// Rewrites a physical memory address if QEMU physical memory mode is enabled
    /// and ARM TrustZone address transformation is enabled.
    ///
    /// # ARMv8-M / TrustZone Memory Aliasing & Bit 28 (`0x10000000`)
    /// On ARMv8-M processors (e.g. Cortex-M33 on `mps2-an505`), memory is divided into
    /// Non-Secure and Secure alias regions. The only difference between a Non-Secure memory
    /// address and its Secure alias counterpart is Bit 28 (`0x10000000`):
    /// - SRAM (SSRAM1): Non-Secure `0x28000000–0x29FFFFFF`, Secure Alias `0x38000000–0x39FFFFFF`
    /// - Code / Flash: Non-Secure `0x00000000–0x0FFFFFFF`, Secure Alias `0x10000000–0x1FFFFFFF`
    /// - Peripherals: Non-Secure `0x40000000–0x4FFFFFFF`, Secure Alias `0x50000000–0x5FFFFFFF`
    ///
    /// # QEMU Physical Memory Mode (`Qqemu.PhyMemMode:1`) Behavior
    /// In QEMU's system memory map (`address_space_memory`), physical RAM is registered at its
    /// canonical physical base address (e.g. `0x28000000`), leaving Secure alias regions
    /// (e.g. `0x38000000`) as unmapped gaps in QEMU's system physical address space.
    ///
    /// - In Normal Debug Mode (`Qqemu.PhyMemMode:0`), memory queries route through
    ///   `cpu_memory_rw_debug()`, which translates Secure Alias `0x38010000` -> `0x28010000` via
    ///   `arm_cpu_get_phys_page_attrs_debug()`. However, if the CPU is paused in unprivileged
    ///   user mode (`ARMMMUIdx_MUser`), MPU/SAU security checks reject the unprivileged request and
    ///   return GDB error `"E14"` (GDB Error 20).
    /// - In Physical Memory Mode (`Qqemu.PhyMemMode:1`), memory queries bypass CPU MPU/MMU
    ///   translation and invoke `cpu_physical_memory_read()` directly on `address_space_memory`.
    ///   Querying unmapped Secure alias addresses like `0x38000000` directly hits unmapped address
    ///   space in QEMU, returning zeroes (`0x00`).
    ///
    /// # Hardware SWD Parity & Address Transformation
    /// Real hardware debug probes (over SWD via CoreSight MEM-AP) set the Secure bit in control
    /// registers (`CSW.PROT`) and automatically strip Bit 28 when placing physical address transfers
    /// on the AHB bus.
    ///
    /// When `qemu_physical_memory_mode` and `has_arm_trustzone` are enabled,
    /// this function strips Bit 28 (`addr & !0x10000000`) from the requested address to map
    /// Secure alias addresses to canonical physical RAM addresses. Otherwise,
    /// addresses are returned unchanged.
    fn rewrite_physical_address(&self, addr: u64) -> u64 {
        if self.qemu_physical_memory_mode && self.has_arm_trustzone {
            addr & !0x10000000
        } else {
            addr
        }
    }

    /// Reads memory from the target at the specified address and length.
    ///
    /// Sends one or more `m` packets and waits for the responses.
    pub async fn read_memory(&mut self, addr: u64, length: u64) -> io::Result<Vec<u8>> {
        let length = usize::try_from(length)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "Length is too large"))?;

        let max_chunk_size = Packet::max_payload_size(self.max_packet_size)?;

        let mut data = Vec::with_capacity(length);
        let mut current_addr = self.rewrite_physical_address(addr);
        let mut remaining_length = length;

        while remaining_length > 0 {
            let chunk_length = core::cmp::min(remaining_length, max_chunk_size);
            self.send_packet(&Packet::ReadMemory {
                addr: current_addr,
                length: chunk_length as u64,
            })
            .await?;

            let response = self.receive_packet().await?;
            let chunk_data = match response {
                Packet::ReadMemoryResponse(data) => data,
                Packet::Error(code) => {
                    return Err(io::Error::other(format!(
                        "GDB server returned error code: {}",
                        code
                    )));
                }
                _ => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("Unexpected response to read memory: {:?}", response),
                    ));
                }
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

    /// Writes memory to the target at the specified address.
    ///
    /// Sends one or more `M` packets and waits for `OK` responses.
    pub async fn write_memory(&mut self, addr: u64, data: &[u8]) -> io::Result<()> {
        if data.is_empty() {
            return Ok(());
        }

        let mut current_addr = self.rewrite_physical_address(addr);
        let mut remaining_data = data;

        while !remaining_data.is_empty() {
            let chunk_length = Packet::max_write_memory_chunk_size(
                self.max_packet_size,
                current_addr,
                remaining_data.len(),
            )?;

            let (chunk, rest) = remaining_data.split_at(chunk_length);

            // Send the M packet for the current chunk.
            self.send_packet(&Packet::WriteMemory {
                addr: current_addr,
                data: chunk.to_vec(),
            })
            .await?;

            // Wait for GDB server response (OK, Error, or Empty if unsupported).
            let response = self.receive_packet().await?;
            match response {
                Packet::Ok => {}
                Packet::Error(code) => {
                    return Err(io::Error::other(format!(
                        "GDB server returned error code: {}",
                        code
                    )));
                }
                Packet::Empty => {
                    return Err(io::Error::new(
                        io::ErrorKind::Unsupported,
                        "GDB server does not support writing memory",
                    ));
                }
                _ => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "Unexpected response to write memory",
                    ));
                }
            }

            current_addr += chunk_length as u64;
            remaining_data = rest;
        }

        Ok(())
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

    /// Continues target execution.
    pub async fn continue_execution(&mut self) -> io::Result<()> {
        self.send_packet(&Packet::Continue).await
    }

    /// Waits for the target to stop and returns the [`StopReply`].
    pub async fn wait_for_stop_reply(&mut self) -> io::Result<StopReply> {
        let response = self.receive_packet().await?;
        match response {
            Packet::StopReply(reply) => Ok(reply),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unexpected response while waiting for stop reply",
            )),
        }
    }

    /// Sends a Control-C (0x03) interrupt character to the target to stop execution,
    /// queries the halt reason (`?`), consumes up to two stop replies, and returns a stop reply.
    ///
    /// This method is idempotent and can be safely called whether the target is currently running
    /// or already stopped.
    pub async fn interrupt(&mut self) -> io::Result<StopReply> {
        let packet = Packet::Interrupt;
        self.stream.write_all(packet.encode().as_bytes()).await?;
        self.stream.flush().await?;

        self.send_packet(&Packet::QueryHaltReason).await?;

        let response = self.receive_packet().await?;
        let reply = match response {
            Packet::StopReply(reply) => reply,
            _ => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Unexpected response to interrupt",
                ));
            }
        };

        if !self.stream.buffer().is_empty() {
            let _ = self.receive_packet().await;
        }

        Ok(reply)
    }

    /// Reads all registers from the target.
    ///
    /// Sends a `g` packet and returns the register bytes.
    pub async fn read_registers(&mut self) -> io::Result<Vec<u8>> {
        self.send_packet(&Packet::ReadRegisters).await?;
        let response = self.receive_packet().await?;
        match response {
            Packet::ReadMemoryResponse(data) => Ok(data),
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unexpected response to read registers: {:?}", response),
            )),
        }
    }

    /// Reads a single register by index from the target.
    ///
    /// Sends a `p<reg>` packet and returns the register bytes.
    pub async fn read_register(&mut self, reg: u32) -> io::Result<Vec<u8>> {
        self.send_packet(&Packet::ReadRegister { reg }).await?;
        let response = self.receive_packet().await?;
        match response {
            Packet::ReadMemoryResponse(data) => Ok(data),
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unexpected response to read register: {:?}", response),
            )),
        }
    }

    /// Writes a single register by index on the target.
    ///
    /// Sends a `P<reg>=<val>` packet and expects `OK`.
    pub async fn write_register(&mut self, reg: u32, val: &[u8]) -> io::Result<()> {
        self.send_packet(&Packet::WriteRegister {
            reg,
            val: val.to_vec(),
        })
        .await?;
        let response = self.receive_packet().await?;
        match response {
            Packet::Ok => Ok(()),
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unexpected response to write register: {:?}", response),
            )),
        }
    }

    /// Enables or disables physical memory mode in QEMU's GDB server (`Qqemu.PhyMemMode`).
    ///
    /// When physical memory mode is enabled (`true`), memory accesses bypass CPU MMU/MPU/PMP
    /// address translation and access controls.
    pub async fn set_qemu_physical_memory_mode(&mut self, enable: bool) -> io::Result<()> {
        self.send_packet(&Packet::QemuPhyMemMode(enable)).await?;
        let response = self.receive_packet().await?;
        match response {
            Packet::Ok => {
                self.qemu_physical_memory_mode = enable;
                Ok(())
            }
            Packet::Error(code) => Err(io::Error::other(format!(
                "GDB server returned error code: {}",
                code
            ))),
            Packet::Empty => Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "GDB server does not support Qqemu.PhyMemMode",
            )),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unexpected response to Qqemu.PhyMemMode",
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
    async fn test_write_memory() {
        const TEST_PAYLOAD_BYTES: &[u8] = &[0xde, 0xca, 0xfb, 0xad];

        let mut input = vec![b'+'];
        let response_payload = b"OK";
        let checksum = Packet::calculate_checksum(response_payload);
        input.push(b'$');
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);

        client
            .write_memory(0x1000, TEST_PAYLOAD_BYTES)
            .await
            .unwrap();

        let expected_sent = b"$M1000,4:decafbad#c2+"; // + is ACK for response
        assert_eq!(stream.write_data, expected_sent);
    }

    #[tokio::test]
    async fn test_write_memory_chunking() {
        let mut input = Vec::new();

        // Chunk 1 response
        input.push(b'+'); // ACK for 1st command
        input.push(b'$');
        input.extend_from_slice(b"OK");
        input.push(b'#');
        let ok_sum = Packet::calculate_checksum(b"OK");
        input.extend_from_slice(format!("{:02x}", ok_sum).as_bytes());

        // Chunk 2 response
        input.push(b'+'); // ACK for 2nd command
        input.push(b'$');
        input.extend_from_slice(b"OK");
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", ok_sum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);
        client.set_max_packet_size(14);

        client.write_memory(0x1000, &[0xaa, 0xbb]).await.unwrap();

        let p1 = Packet::WriteMemory {
            addr: 0x1000,
            data: vec![0xaa],
        };
        let p2 = Packet::WriteMemory {
            addr: 0x1001,
            data: vec![0xbb],
        };

        let mut expected = Vec::new();
        expected.extend_from_slice(p1.encode().as_bytes());
        expected.push(b'+');
        expected.extend_from_slice(p2.encode().as_bytes());
        expected.push(b'+');

        assert_eq!(stream.write_data, expected);
    }

    #[tokio::test]
    async fn test_write_memory_large_buffer_small_packet_size() {
        // Total data length is 16 bytes (0x10, requiring 2 hex digits for total length).
        // Max packet size is set to 16 bytes. Address 0x1000 has 4 hex digits.
        // Fixed overhead = 7 + 4 = 11 bytes.
        // With max_packet_size = 16 bytes, each 2-byte chunk packet ($M1000,2:aaaa#xx) takes 16 bytes (1 hex digit for length 2).
        // Transmitting 16 bytes requires 8 chunks of 2 bytes each.

        let data = [0xaa; 16];
        let mut input = Vec::new();
        let ok_sum = Packet::calculate_checksum(b"OK");

        for _ in 0..8 {
            input.push(b'+');
            input.push(b'$');
            input.extend_from_slice(b"OK");
            input.push(b'#');
            input.extend_from_slice(format!("{:02x}", ok_sum).as_bytes());
        }

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);
        client.set_max_packet_size(16);

        client.write_memory(0x1000, &data).await.unwrap();
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
        let response = b"+";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);

        client.continue_execution().await.unwrap();

        assert_eq!(stream.write_data, b"$c#63");
    }

    #[tokio::test]
    async fn test_wait_for_stop_reply() {
        let response = b"$S02#b5";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);

        let stop_reason = client.wait_for_stop_reply().await.unwrap();

        assert_eq!(stop_reason, StopReply::Signal(2));
        assert_eq!(stream.write_data, b"+"); // ACK sent for stop reply
    }

    #[tokio::test]
    async fn test_continue_execution_then_interrupt() {
        let mut response = vec![b'+']; // ACK for continue 'c'
        response.push(b'+'); // ACK for HaltReason '?'
        let stop_reply = b"$S02#b5";
        response.extend_from_slice(stop_reply);

        let mut stream = MockStream::new(response);
        let mut client = Client::new(&mut stream);

        client.continue_execution().await.unwrap();

        let stop_reason = client.interrupt().await.unwrap();
        assert_eq!(stop_reason, StopReply::Signal(2));

        assert_eq!(stream.write_data, b"$c#63\x03$?#3f+");
    }

    #[tokio::test]
    async fn test_interrupt_when_stopped() {
        let mut response = vec![b'+']; // ACK for HaltReason '?'
        let stop_reply = b"$S02#b5";
        response.extend_from_slice(stop_reply);
        let mut stream = MockStream::new(response);
        let mut client = Client::new(&mut stream);

        let stop_reason = client.interrupt().await.unwrap();

        assert_eq!(stop_reason, StopReply::Signal(2));
        assert_eq!(stream.write_data, b"\x03$?#3f+");
    }

    #[tokio::test]
    async fn test_interrupt_when_running_two_stop_replies() {
        let stop_reply = b"$S02#b5";
        let mut response = Vec::new();
        response.push(b'+'); // ACK for HaltReason '?'
        response.extend_from_slice(stop_reply); // Stop reply 1 (e.g. for interrupt \x03)
        response.extend_from_slice(stop_reply); // Stop reply 2 (for HaltReason '?')

        let mut stream = MockStream::new(response);
        let mut client = Client::new(&mut stream);

        let stop_reason = client.interrupt().await.unwrap();

        assert_eq!(stop_reason, StopReply::Signal(2));
        assert_eq!(stream.write_data, b"\x03$?#3f++");
    }

    #[tokio::test]
    async fn test_read_registers() {
        let response_payload = b"01020304";
        let checksum = Packet::calculate_checksum(response_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);

        let regs = client.read_registers().await.unwrap();
        assert_eq!(regs, &[0x01, 0x02, 0x03, 0x04]);
        assert_eq!(stream.write_data, b"$g#67+");
    }

    #[tokio::test]
    async fn test_read_register() {
        let response_payload = b"e2000010";
        let checksum = Packet::calculate_checksum(response_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);

        let reg_val = client.read_register(15).await.unwrap();
        assert_eq!(reg_val, &[0xe2, 0x00, 0x00, 0x10]);
        assert_eq!(stream.write_data, b"$pf#d6+");
    }

    #[tokio::test]
    async fn test_write_register() {
        let response = b"+$OK#9a";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);

        client
            .write_register(14, &[0x00, 0x00, 0x00, 0x00])
            .await
            .unwrap();
        assert_eq!(stream.write_data, b"$Pe=00000000#72+");
    }

    #[tokio::test]
    async fn test_set_qemu_physical_memory_mode() {
        let response = b"+$OK#9a";
        let mut stream = MockStream::new(response.to_vec());
        let mut client = Client::new(&mut stream);

        assert!(!client.qemu_physical_memory_mode());
        client.set_qemu_physical_memory_mode(true).await.unwrap();
        assert!(client.qemu_physical_memory_mode());
        assert_eq!(stream.write_data, b"$Qqemu.PhyMemMode:1#77+");
    }

    #[tokio::test]
    async fn test_read_write_memory_qemu_physical_memory_mode() {
        let response_payload = b"decafbad";
        let checksum = Packet::calculate_checksum(response_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);
        client.set_has_arm_trustzone(true);
        client.qemu_physical_memory_mode = true;

        // Address 0x10002000 has 0x10000000 bit set. In physical memory mode with TrustZone enabled, it should be stripped to 0x2000.
        let result = client.read_memory(0x10002000, 4).await.unwrap();
        assert_eq!(result, &[0xde, 0xca, 0xfb, 0xad]);
        assert_eq!(stream.write_data, b"$m2000,4#8f+");
    }

    #[tokio::test]
    async fn test_read_write_memory_qemu_physical_memory_mode_without_trustzone() {
        let response_payload = b"decafbad";
        let checksum = Packet::calculate_checksum(response_payload);
        let mut input = vec![b'+', b'$'];
        input.extend_from_slice(response_payload);
        input.push(b'#');
        input.extend_from_slice(format!("{:02x}", checksum).as_bytes());

        let mut stream = MockStream::new(input);
        let mut client = Client::new(&mut stream);
        client.set_has_arm_trustzone(false);
        client.qemu_physical_memory_mode = true;

        // Address 0x10002000 has 0x10000000 bit set. Without TrustZone enabled, address should NOT be rewritten.
        let result = client.read_memory(0x10002000, 4).await.unwrap();
        assert_eq!(result, &[0xde, 0xca, 0xfb, 0xad]);
        assert_eq!(
            stream.write_data,
            format!(
                "$m10002000,4#{:02x}+",
                Packet::calculate_checksum(b"m10002000,4")
            )
            .as_bytes()
        );
    }
}
