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

use std::path::Path;

use anyhow::{Context, Result};
use pw_gdb_protocol::Client;
use pw_kernel_annotations::ImageInfo;
use tokio::net::TcpStream;
use tokio_util::compat::TokioAsyncReadCompatExt;

pub async fn run(path: &Path, gdb_addr: &str) -> Result<()> {
    let info = ImageInfo::new(path)?;

    // Connect to GDB server
    let stream = TcpStream::connect(gdb_addr)
        .await
        .context(format!("Failed to connect to GDB server at {}", gdb_addr))?;

    let compat_stream = stream.compat();
    let mut client = Client::new(compat_stream);

    println!(
        "{:<30} {:<12} {:<10} {:<10} {:<10}",
        "Name", "Address", "Size", "Used", "Usage %"
    );
    println!(
        "{:-<30} {:-<12} {:-<10} {:-<10} {:-<10}",
        "", "", "", "", ""
    );

    for stack in info.stacks {
        let memory = client
            .read_memory(stack.stack_addr, stack.stack_size)
            .await
            .context(format!("Failed to read stack memory for {}", stack.name))?;

        // Stacks are initialized with the repeating pattern 0xdecafbad by the
        // kernel. If this pattern is unmodified at a given, there is high
        // probability that that part of the stack has not been used.  From this
        // a high water mark can be detected.

        let pattern = magic_values::UNUSED_STACK_PATTERN;
        let pattern_bytes = match info.endian {
            object::Endianness::Little => pattern.to_le_bytes(),
            object::Endianness::Big => pattern.to_be_bytes(),
        };

        let unused_bytes = memory
            .chunks(4)
            .take_while(|chunk| chunk == &pattern_bytes)
            .count()
            * 4;
        let used_bytes = stack.stack_size - unused_bytes as u64;

        // Precisions loss will not effect percentage display.
        #[allow(clippy::cast_precision_loss)]
        let usage_percent = (used_bytes as f64 / stack.stack_size as f64) * 100.0;

        println!(
            "{:<30} 0x{:08x}   {:<10} {:<10} {:.1}%",
            stack.name, stack.stack_addr, stack.stack_size, used_bytes, usage_percent
        );
    }

    Ok(())
}
