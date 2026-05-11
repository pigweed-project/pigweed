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

use pw_status::{Error, Result};
use zerocopy::{FromBytes, Immutable, IntoBytes, TryFromBytes};

/// The type of request sent over the stream IPC channel.
#[non_exhaustive]
#[repr(u32)]
#[derive(Copy, Clone, Debug, IntoBytes, TryFromBytes, Immutable)]
pub enum RequestType {
    /// Read data from the stream.
    Read = 1,
    /// Write data to the stream.
    Write = 2,
}

/// Header prepended to all stream requests.
#[non_exhaustive]
#[repr(C)]
#[derive(IntoBytes, TryFromBytes, Immutable)]
pub struct RequestHeader {
    /// The request command type.
    pub request_type: RequestType,
}

impl RequestHeader {
    /// Decodes a `RequestHeader` from a raw byte slice.
    pub fn try_from_bytes(bytes: &[u8]) -> Result<Self> {
        Self::try_from(bytes)
    }
}

impl TryFrom<&[u8]> for RequestHeader {
    type Error = Error;

    fn try_from(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < core::mem::size_of::<RequestHeader>() {
            return Err(Error::OutOfRange);
        }
        Self::try_read_from_bytes(&bytes[..core::mem::size_of::<RequestHeader>()])
            .map_err(|_| Error::InvalidArgument)
    }
}

/// Header prepended to all stream responses.
#[non_exhaustive]
#[repr(C)]
#[derive(IntoBytes, FromBytes, Immutable)]
pub struct ResponseHeader {
    /// Status of the completed operation. `0` indicates success (OK).
    /// A non-zero value represents a raw Pigweed error code.
    pub status: u32,
    /// For Read requests: the length of the payload following this header.
    /// For Write requests: the number of bytes successfully written (supports partial writes).
    pub length: u32,
}

impl TryFrom<&[u8]> for ResponseHeader {
    type Error = Error;

    fn try_from(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < core::mem::size_of::<ResponseHeader>() {
            return Err(Error::OutOfRange);
        }
        Self::read_from_bytes(&bytes[..core::mem::size_of::<ResponseHeader>()])
            .map_err(|_| Error::InvalidArgument)
    }
}

impl ResponseHeader {
    /// Decodes a `ResponseHeader` from a raw byte slice.
    pub fn try_from_bytes(bytes: &[u8]) -> Result<Self> {
        Self::try_from(bytes)
    }

    /// Evaluates the status code in the response header, returning `Ok(())` on success,
    /// or the corresponding Pigweed error.
    pub fn result(&self) -> Result<()> {
        pw_status::status_to_result(self.status)
    }
}
