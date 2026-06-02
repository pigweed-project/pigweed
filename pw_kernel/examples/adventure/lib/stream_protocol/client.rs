// Copyright 2026 The Pigweed Authors
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

use core::mem;

use pw_status::{Error, Result};
use userspace::syscall::{self, Signals};
use userspace::time::Instant;
use zerocopy::IntoBytes;

use crate::defs::{RequestHeader, RequestType, ResponseHeader};

/// A `stream_protocol` client.
pub struct StreamClient {
    ipc_handle: u32,
}

impl StreamClient {
    /// Creates a new `StreamClient` instance for the given channel handle.
    #[must_use]
    pub const fn new(ipc_handle: u32) -> Self {
        Self { ipc_handle }
    }

    /// Prepares an `ipc_buffer` for a write transaction
    ///
    /// TODO: https://pwbug.dev/509941221 - Use vectorized IO to avoid copying
    /// headers and data into a temporary buffer.
    fn prepare_write_transaction(write_data: &[u8], ipc_buffer: &mut [u8]) -> Result<usize> {
        let header = RequestHeader {
            request_type: RequestType::Write,
        };

        let header_bytes = header.as_bytes();
        if header_bytes.len() + write_data.len() > ipc_buffer.len() {
            return Err(Error::OutOfRange);
        }

        ipc_buffer[..header_bytes.len()].copy_from_slice(header_bytes);

        let start = header_bytes.len();
        let end = start + write_data.len();
        ipc_buffer[start..end].copy_from_slice(write_data);

        Ok(header_bytes.len() + write_data.len())
    }

    /// Verifies and extracts the read payload from the raw response buffer.
    ///
    /// TODO: https://pwbug.dev/509941221 - Use vectorized IO to avoid copying
    /// headers and data into a temporary buffer.
    fn extract_read_payload(
        response_header: &ResponseHeader,
        response_buffer: &[u8],
        response_length: usize,
        destination_buffer: &mut [u8],
    ) -> Result<usize> {
        let payload_length = response_length - mem::size_of::<ResponseHeader>();

        if payload_length != response_header.length as usize {
            return Err(Error::InvalidArgument);
        }

        if payload_length > destination_buffer.len() {
            return Err(Error::OutOfRange);
        }

        let start = mem::size_of::<ResponseHeader>();
        let end = start + payload_length;

        destination_buffer[..payload_length].copy_from_slice(&response_buffer[start..end]);

        Ok(payload_length)
    }

    /// Writes a byte slice to the IPC stream channel with a given deadline.
    pub fn write(&self, data: &[u8], deadline: Instant) -> Result<usize> {
        let mut request_buffer = [0u8; 128];
        let request_length = Self::prepare_write_transaction(data, &mut request_buffer)?;
        let mut response_buffer = [0u8; mem::size_of::<ResponseHeader>()];

        let response_length = syscall::channel_transact(
            self.ipc_handle,
            &request_buffer[..request_length],
            &mut response_buffer,
            deadline,
        )?;

        let response_header = ResponseHeader::try_from_bytes(&response_buffer[..response_length])?;
        response_header.result()?;

        Ok(response_header.length as usize)
    }

    /// Writes a string slice to the IPC stream channel with a given deadline.
    pub fn write_str(&self, text: &str, deadline: Instant) -> Result<usize> {
        self.write(text.as_bytes(), deadline)
    }

    /// Reads data from the IPC stream channel into the destination buffer with a given deadline.
    pub fn read(&self, destination_buffer: &mut [u8], deadline: Instant) -> Result<usize> {
        syscall::object_wait(self.ipc_handle, Signals::USER, deadline)?;

        let header = RequestHeader {
            request_type: RequestType::Read,
        };
        let mut response_buffer = [0u8; 128];
        let response_length = syscall::channel_transact(
            self.ipc_handle,
            header.as_bytes(),
            &mut response_buffer,
            deadline,
        )?;

        let response_header = ResponseHeader::try_from_bytes(&response_buffer[..response_length])?;
        response_header.result()?;
        Self::extract_read_payload(
            &response_header,
            &response_buffer,
            response_length,
            destination_buffer,
        )
    }
}
