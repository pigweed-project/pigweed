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

use std::collections::HashMap;
use std::io::BufWriter;
use std::path::Path;

use anyhow::{Context, Result, anyhow};
use byteorder::{BigEndian, LittleEndian};
use object::Endianness;
use pw_gdb_protocol::Client;
use pw_kernel_tracing::{EventPayload, Record};
use pw_perfetto_writer::{PerfettoWriter, ThreadState};
use tokio::net::TcpStream;
use tokio_util::compat::TokioAsyncReadCompatExt;

use crate::image_info::ImageInfo;

// TODO: konkers - Move this to a kernel types crate.
#[derive(Copy, Clone, PartialEq)]
#[repr(u8)]
pub enum NativeThreadState {
    New = 0,
    Initial = 1,
    Ready = 2,
    Running = 3,
    Terminated = 4,
    Joined = 5,
    WaitingInterruptible = 6,
    WaitingNonInterruptible = 7,
}

pub fn thread_state(raw_thread_state: u8) -> ThreadState {
    if raw_thread_state == NativeThreadState::New as u8
        || raw_thread_state == NativeThreadState::Initial as u8
    {
        ThreadState::TaskStateCreated
    } else if raw_thread_state == NativeThreadState::Ready as u8 {
        ThreadState::TaskStateRunnable
    } else if raw_thread_state == NativeThreadState::Running as u8 {
        ThreadState::TaskStateRunning
    } else if raw_thread_state == NativeThreadState::Terminated as u8 {
        ThreadState::TaskStateDead
    } else if raw_thread_state == NativeThreadState::Joined as u8 {
        ThreadState::TaskStateDestroyed
    } else if raw_thread_state == NativeThreadState::WaitingInterruptible as u8 {
        ThreadState::TaskStateInterruptibleSleep
    } else if raw_thread_state == NativeThreadState::WaitingNonInterruptible as u8 {
        ThreadState::TaskStateUninterruptibleSleep
    } else {
        ThreadState::TaskStateUnknown
    }
}

pub async fn run(path: &Path, gdb_addr: &str) -> Result<()> {
    let info = ImageInfo::new(path)?;

    if info.trace_buffers.len() != 1 {
        return Err(anyhow!(
            "Only a single trace buffer is support, found {}",
            info.trace_buffers.len()
        ));
    }

    // Connect to GDB server
    let stream = TcpStream::connect(gdb_addr)
        .await
        .context(format!("Failed to connect to GDB server at {}", gdb_addr))?;

    let compat_stream = stream.compat();
    let mut client = Client::new(compat_stream);

    let trace_buffer = &info.trace_buffers[0];
    let memory = client
        .read_memory(trace_buffer.addr, trace_buffer.size)
        .await
        .context(format!(
            "Failed to read trace buffer memory for {}",
            trace_buffer.name
        ))?;

    let records: Vec<Record> = memory
        .chunks(16)
        .filter_map(|data| {
            if info.endian == Endianness::Little {
                Record::read_from_buffer::<LittleEndian>(data)
            } else {
                Record::read_from_buffer::<BigEndian>(data)
            }
        })
        .filter(|record| record.time_stamp != 0)
        .collect();

    let f = std::fs::File::create("trace.pb").unwrap();
    let writer = BufWriter::new(f);
    let mut perfetto = PerfettoWriter::new(writer);

    // pw_kernel does not have classic process and thread ids. Instead it has
    // opaque `usize` ids that may be larger than perfetto's i32 pid and tids.
    // Perfetto requires pids and tids so fake ones are created.
    //
    // Pid 0 is reserved for the kernel.
    // Tids start at 0x1000 to avoid colliding with pid 0.
    let mut process_pids: HashMap<u64, i32> = HashMap::new();
    let mut thread_tids = HashMap::new();

    // Add synthetic kernel entry as pid 0.
    process_pids.insert(0, 0);
    perfetto.add_process_track(0, 0, 0, "kernel");

    let first_timestamp = u64::from(
        records
            .iter()
            .map(|v| v.time_stamp)
            .reduce(core::cmp::min)
            .unwrap(),
    );

    for (i, process) in info.processes.iter().enumerate() {
        // Pids start at 1 to avoid colliding with kernel's pid 0.
        let pid = i32::try_from(i).unwrap() + 1;
        process_pids.insert(process.id, pid);
        perfetto.add_process_track(first_timestamp, process.id, pid, &process.name);
    }

    for (i, thread) in info.threads.iter().enumerate() {
        let pid = process_pids.get(&thread.parent_id).unwrap();
        // Tids start at 0x1000 to avoid colliding with pids.
        let tid = i32::try_from(i).unwrap() + 0x1000;
        thread_tids.insert(u32::try_from(thread.id).unwrap(), tid);
        perfetto.add_thread_track(
            first_timestamp,
            thread.id,
            thread.parent_id,
            *pid,
            tid,
            &thread.name,
        );
    }

    for record in records.iter() {
        let event = record.payload().unwrap();
        match event {
            EventPayload::ContextSwitch(event) => {
                if event.old_thread_id != 0 {
                    let old_tid = thread_tids.get(&event.old_thread_id).unwrap();
                    perfetto.add_thread_state_event(
                        u64::from(record.time_stamp),
                        *old_tid,
                        thread_state(event.old_thread_state),
                    );
                }

                let new_tid = thread_tids.get(&event.new_thread_id).unwrap();
                perfetto.add_thread_state_event(
                    u64::from(record.time_stamp),
                    *new_tid,
                    ThreadState::TaskStateRunning,
                );
            }
        }
    }

    println!("wrote trace to trace.pb");
    Ok(())
}
