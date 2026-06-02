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
use ring_buffer::RingBuffer;
use userspace::syscall;
use zerocopy::IntoBytes;

use crate::defs::{RequestHeader, RequestType, ResponseHeader};

/// A `stream_protocol` server.
///
/// Owns and manages an read transaction buffer of size `BUFFER_SIZE`.
pub struct StreamServer<const BUFFER_SIZE: usize> {
    ipc_handle: u32,
    receive_buffer: RingBuffer<BUFFER_SIZE>,
}

impl<const BUFFER_SIZE: usize> StreamServer<BUFFER_SIZE> {
    /// Creates a new `StreamServer` instance for the given channel handle.
    #[must_use]
    pub const fn new(ipc_handle: u32) -> Self {
        Self {
            ipc_handle,
            receive_buffer: RingBuffer::<BUFFER_SIZE>::new(),
        }
    }

    /// Pushes incoming hardware byte stream data into the internal ring buffer,
    /// automatically setting the peer USER signal to notify clients.
    pub fn push_data(&mut self, data: &[u8]) -> Result<usize> {
        let pushed_bytes = self.receive_buffer.push_slice(data);
        if pushed_bytes > 0 {
            self.set_data_available(true)?;
        }
        Ok(pushed_bytes)
    }

    /// Handle handles an IPC request.
    ///
    /// Handles as single incoming IPC request, if pending.  When handling a
    /// write transaction, calls the `write_fn` callback with the written data.
    pub fn handle_ipc(&mut self, write_fn: impl FnMut(&[u8])) -> Result<()> {
        let mut request_buffer = [0u8; 128];
        let bytes_read = syscall::channel_read(self.ipc_handle, 0, &mut request_buffer)?;

        let header = RequestHeader::try_from_bytes(&request_buffer[..bytes_read])?;

        match header.request_type {
            RequestType::Write => self.handle_write(&request_buffer, bytes_read, write_fn)?,
            RequestType::Read => self.handle_read()?,
        }
        Ok(())
    }

    /// Handles a stream Write request from a raw request buffer.
    fn handle_write(
        &self,
        request_buffer: &[u8],
        bytes_read: usize,
        mut write_fn: impl FnMut(&[u8]),
    ) -> Result<()> {
        let payload_len = bytes_read - mem::size_of::<RequestHeader>();
        let start = mem::size_of::<RequestHeader>();
        let payload = &request_buffer[start..start + payload_len];

        write_fn(payload);

        let response = ResponseHeader {
            status: pw_status::OK,
            #[allow(clippy::cast_possible_truncation)]
            length: payload_len as u32,
        };
        syscall::channel_respond(self.ipc_handle, response.as_bytes())
    }

    /// Handles a stream Read request, servicing it out of the internal ring buffer.
    fn handle_read(&mut self) -> Result<()> {
        let mut response_buffer = [0u8; 128];
        let response_length = self.prepare_read_response(&mut response_buffer)?;

        // Clear peer USER signal before responding to avoid a period between the
        // transaction completing and the client still having the signal that there's
        // data available.
        if self.receive_buffer.is_empty() {
            self.set_data_available(false)?;
        }

        syscall::channel_respond(self.ipc_handle, &response_buffer[..response_length])?;
        Ok(())
    }

    /// Prepares a read transaction response buffer containing the response header and payload.
    ///
    /// TODO: https://pwbug.dev/509941221 - Use vectorized IO to avoid copying
    /// headers and data into a temporary buffer.
    fn prepare_read_response(&mut self, response_buffer: &mut [u8]) -> Result<usize> {
        let header_size = mem::size_of::<ResponseHeader>();

        if header_size > response_buffer.len() {
            return Err(Error::OutOfRange);
        }

        let max_payload_length = response_buffer.len() - header_size;

        let payload_length = self.receive_buffer.pop_slice(max_payload_length, |slice| {
            response_buffer[header_size..header_size + slice.len()].copy_from_slice(slice);
            slice.len()
        });

        let response_header = ResponseHeader {
            status: pw_status::OK,
            #[allow(clippy::cast_possible_truncation)]
            length: payload_length as u32,
        };

        let header_bytes = response_header.as_bytes();
        response_buffer[..header_size].copy_from_slice(header_bytes);

        Ok(header_size + payload_length)
    }

    /// Toggles the peer user signal (Signals::USER) on the client.
    fn set_data_available(&self, available: bool) -> Result<()> {
        syscall::object_set_peer_user_signal(self.ipc_handle, available)?;
        Ok(())
    }
}
