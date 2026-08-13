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

//! Host-side client library for interacting with Pigweed Kernel debug mailboxes.

#![forbid(unsafe_code)]

use core::marker::PhantomData;
use core::time::Duration;

use anyhow::{Context, Result, anyhow};
use byteorder::{ByteOrder, LittleEndian};
use futures::io::{AsyncRead, AsyncWrite};
use pw_gdb_protocol::Client;
use pw_kernel_annotations::{DebugMailboxInfo, ImageInfo};
use pw_kernel_debug_mailbox_protocol::{ReadyFlag, UnreadFlag};
use zerocopy::{FromBytes, Immutable, IntoBytes};

/// Host-side handle for manipulating a debug mailbox on a target system.
#[derive(Debug, Clone)]
pub struct DebugMailboxClient<E: ByteOrder = LittleEndian> {
    pub name: String,
    pub addr: u64,
    pub size: u64,
    _phantom: PhantomData<E>,
}

impl<E: ByteOrder> DebugMailboxClient<E> {
    /// Byte offset of the `ready` atomic flag in the mailbox layout.
    pub const READY_OFFSET: u64 = 0;
    /// Byte offset of the `unread` atomic flag in the mailbox layout.
    pub const UNREAD_OFFSET: u64 = 4;
    /// Byte offset of the `value` payload in the mailbox layout.
    pub const VALUE_OFFSET: u64 = 8;

    /// Creates a new `DebugMailboxClient` from a `DebugMailboxInfo`.
    #[must_use]
    pub fn new(info: &DebugMailboxInfo) -> Self {
        Self {
            name: info.name.clone(),
            addr: info.addr,
            size: info.size,
            _phantom: PhantomData,
        }
    }

    /// Looks up a debug mailbox by name in an ELF `ImageInfo`.
    pub fn lookup(image: &ImageInfo, name: &str) -> Result<Self> {
        for mailbox in &image.mailboxes {
            if mailbox.name == name {
                return Ok(Self::new(mailbox));
            }
        }

        let available = image
            .mailboxes
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>()
            .join(", ");
        Err(anyhow!(
            "Mailbox '{name}' not found. Available mailboxes: {available}"
        ))
    }

    /// Reads a typed value from target memory at the specified mailbox offset.
    async fn read_field<V: FromBytes, S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        client: &mut Client<S>,
        offset: u64,
    ) -> Result<V> {
        let read_addr = self.addr + offset;
        client
            .interrupt()
            .await
            .context("Failed to interrupt target")?;

        let size = core::mem::size_of::<V>();
        let bytes = client
            .read_memory(read_addr, u64::try_from(size)?)
            .await
            .context(format!("Failed to read memory at 0x{:08x}", read_addr))?;

        let (val, _) = V::read_from_prefix(&bytes)
            .map_err(|_| anyhow!("Failed to parse value of size {} from memory", size))?;

        Ok(val)
    }

    /// Writes a typed value to target memory at the specified mailbox offset.
    async fn write_field<V: IntoBytes + Immutable, S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        client: &mut Client<S>,
        offset: u64,
        value: V,
    ) -> Result<()> {
        let write_addr = self.addr + offset;
        client
            .interrupt()
            .await
            .context("Failed to interrupt target")?;

        client
            .write_memory(write_addr, value.as_bytes())
            .await
            .context(format!("Failed to write memory at 0x{:08x}", write_addr))?;
        Ok(())
    }

    /// Waits until the target mailbox `ready` flag is non-zero.
    ///
    /// Post-condition: the target will be stopped upon successful exit.
    pub async fn wait_until_ready<S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        client: &mut Client<S>,
    ) -> Result<()> {
        loop {
            let bytes: [u8; 4] = self.read_field(client, Self::READY_OFFSET).await?;
            let ready = E::read_u32(&bytes);
            if ready == ReadyFlag::Ready as u32 {
                break;
            }

            client
                .continue_execution()
                .await
                .context("Failed to resume target execution")?;
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        Ok(())
    }

    /// Sends a value to the target debug mailbox and kicks execution.
    pub async fn send<V: IntoBytes + Immutable, S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        client: &mut Client<S>,
        value: V,
    ) -> Result<()> {
        self.wait_until_ready(client).await?;

        // Write the value
        self.write_field(client, Self::VALUE_OFFSET, value).await?;

        // Set unread flag to Unread
        let mut unread_bytes = [0u8; 4];
        E::write_u32(&mut unread_bytes, UnreadFlag::Unread as u32);
        self.write_field(client, Self::UNREAD_OFFSET, unread_bytes)
            .await?;

        client
            .continue_execution()
            .await
            .context("Failed to resume target execution")?;

        Ok(())
    }

    /// Reads the current payload value from the debug mailbox.
    pub async fn read_value<V: FromBytes, S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        client: &mut Client<S>,
    ) -> Result<V> {
        let val = self.read_field(client, Self::VALUE_OFFSET).await?;

        client
            .continue_execution()
            .await
            .context("Failed to resume target execution")?;

        Ok(val)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_offsets() {
        assert_eq!(DebugMailboxClient::<LittleEndian>::READY_OFFSET, 0);
        assert_eq!(DebugMailboxClient::<LittleEndian>::UNREAD_OFFSET, 4);
        assert_eq!(DebugMailboxClient::<LittleEndian>::VALUE_OFFSET, 8);
    }
}
