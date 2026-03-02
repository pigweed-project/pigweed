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

//! A library for writing Perfetto traces.
//!
//! # Example
//!
//! ```
//! use pw_perfetto_writer::{PerfettoWriter, ThreadState};
//!
//! let mut buffer = Vec::new();
//! {
//!     let mut writer = PerfettoWriter::new(&mut buffer);
//!
//!     writer.add_process_track(1000, 1234, 1, "example process");
//!     writer.add_thread_track(1000, 2345, 1234, 1, 2, "main thread");
//!     writer.add_slice_begin_event(1010, 2345, "foo");
//!     writer.add_slice_end_event(1020, 2345);
//! }
//! ```

use std::io::Write;

use prost::Message;
use trace_proto::perfetto::protos::generic_kernel_task_state_event::TaskStateEnum;
use trace_proto::perfetto::protos::track_descriptor::StaticOrDynamicName;
use trace_proto::perfetto::protos::{
    CounterDescriptor, GenericKernelTaskStateEvent, ProcessDescriptor, ThreadDescriptor, Trace,
    TracePacket, TrackDescriptor, TrackEvent, trace_packet, track_event,
};

pub type ThreadState = TaskStateEnum;

/// A helper for writing Perfetto trace packets.
///
/// `PerfettoWriter` wraps a `Write` implementation and provides methods to write
/// various Perfetto trace events, such as process/thread checks, counter events,
/// and slice events.
pub struct PerfettoWriter<W: Write> {
    writer: W,
    trace: Trace,
    sequence_id: trace_packet::OptionalTrustedPacketSequenceId,
}

impl<W: Write> Drop for PerfettoWriter<W> {
    fn drop(&mut self) {
        let data = self.trace.encode_to_vec();
        self.writer.write_all(&data).unwrap();
    }
}

impl<W: Write> PerfettoWriter<W> {
    /// Creates a new `PerfettoWriter` that writes to the given writer.
    pub fn new(writer: W) -> Self {
        Self {
            writer,
            trace: Default::default(),
            sequence_id: trace_packet::OptionalTrustedPacketSequenceId::TrustedPacketSequenceId(1),
        }
    }

    fn add_packet(&mut self, timestamp: u64, data: trace_packet::Data) {
        let mut packet = TracePacket {
            timestamp: Some(timestamp),
            data: Some(data),
            optional_trusted_packet_sequence_id: Some(self.sequence_id),
            ..Default::default()
        };

        if self.trace.packet.is_empty() {
            packet.previous_packet_dropped = Some(true);
            packet.first_packet_on_sequence = Some(true);
            packet.sequence_flags = Some(
                trace_packet::SequenceFlags::SeqNeedsIncrementalState as u32
                    | trace_packet::SequenceFlags::SeqIncrementalStateCleared as u32,
            );
        }
        self.trace.packet.push(packet);
    }

    /// Adds a process track descriptor.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `uuid` - The unique ID for this track.
    /// * `pid` - The process ID.
    /// * `name` - The name of the process.
    pub fn add_process_track(&mut self, timestamp: u64, uuid: u64, pid: i32, name: &str) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackDescriptor(TrackDescriptor {
                static_or_dynamic_name: Some(StaticOrDynamicName::Name(name.into())),
                uuid: Some(uuid),
                process: Some(ProcessDescriptor {
                    pid: Some(pid),
                    process_name: Some(name.into()),
                    ..Default::default()
                }),
                ..Default::default()
            }),
        )
    }

    /// Adds a thread track descriptor.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `uuid` - The unique ID for this track.
    /// * `parent_uuid` - The UUID of the parent process track.
    /// * `pid` - The process ID.
    /// * `tid` - The thread ID.
    /// * `name` - The name of the thread.
    pub fn add_thread_track(
        &mut self,
        timestamp: u64,
        uuid: u64,
        parent_uuid: u64,
        pid: i32,
        tid: i32,
        name: &str,
    ) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackDescriptor(TrackDescriptor {
                static_or_dynamic_name: Some(StaticOrDynamicName::Name(name.into())),
                uuid: Some(uuid),
                parent_uuid: Some(parent_uuid),
                thread: Some(ThreadDescriptor {
                    pid: Some(pid),
                    tid: Some(tid),
                    thread_name: Some(name.into()),
                    ..Default::default()
                }),
                ..Default::default()
            }),
        )
    }

    /// Adds a counter track descriptor.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `uuid` - The unique ID for this track.
    /// * `parent_uuid` - The UUID of the parent process/thread track.
    /// * `name` - The name of the counter.
    pub fn add_counter_track(&mut self, timestamp: u64, uuid: u64, parent_uuid: u64, name: &str) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackDescriptor(TrackDescriptor {
                static_or_dynamic_name: Some(StaticOrDynamicName::Name(name.into())),
                uuid: Some(uuid),
                parent_uuid: Some(parent_uuid),
                counter: Some(CounterDescriptor {
                    unit_name: Some(name.into()),
                    ..Default::default()
                }),
                ..Default::default()
            }),
        )
    }

    /// Adds a counter event.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `counter_uuid` - The UUID of the counter track.
    /// * `val` - The value of the counter.
    pub fn add_counter_event(&mut self, timestamp: u64, counter_uuid: u64, val: i64) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackEvent(TrackEvent {
                r#type: Some(track_event::Type::Counter.into()),
                track_uuid: Some(counter_uuid),
                counter_value_field: Some(track_event::CounterValueField::CounterValue(val)),
                ..Default::default()
            }),
        )
    }

    /// Adds a slice begin event.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `thread_uuid` - The UUID of the thread track.
    /// * `name` - The name of the slice.
    pub fn add_slice_begin_event(&mut self, timestamp: u64, thread_uuid: u64, name: &str) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackEvent(TrackEvent {
                r#type: Some(track_event::Type::SliceBegin.into()),
                name_field: Some(track_event::NameField::Name(name.into())),
                track_uuid: Some(thread_uuid),
                ..Default::default()
            }),
        )
    }

    /// Adds a slice end event.
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `thread_uuid` - The UUID of the thread track.
    pub fn add_slice_end_event(&mut self, timestamp: u64, thread_uuid: u64) {
        self.add_packet(
            timestamp,
            trace_packet::Data::TrackEvent(TrackEvent {
                r#type: Some(track_event::Type::SliceEnd.into()),
                track_uuid: Some(thread_uuid),
                ..Default::default()
            }),
        )
    }

    /// Adds a thread state event (for kernel task state changes).
    ///
    /// # Arguments
    ///
    /// * `timestamp` - The timestamp of the event.
    /// * `tid` - The thread ID.
    /// * `state` - The new state of the thread.
    pub fn add_thread_state_event(&mut self, timestamp: u64, tid: i32, state: ThreadState) {
        self.add_packet(
            timestamp,
            trace_packet::Data::GenericKernelTaskStateEvent(GenericKernelTaskStateEvent {
                cpu: Some(0),
                tid: Some(tid.into()),
                state: Some(state.into()),
                ..Default::default()
            }),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_perfetto_writer() {
        let mut buffer = Vec::new();
        {
            let mut writer = PerfettoWriter::new(&mut buffer);
            writer.add_process_track(1000, 1234, 1, "example process");
            writer.add_counter_track(1000, 1235, 1234, "counter");
            writer.add_thread_track(1000, 2345, 1234, 1, 2, "main thread");

            writer.add_counter_event(1100, 1235, 2);
            writer.add_counter_event(1200, 1235, 3);
            writer.add_counter_event(1300, 1235, 4);

            writer.add_slice_begin_event(1010, 2345, "a");
            writer.add_slice_begin_event(1040, 2345, "b");

            writer.add_slice_end_event(1180, 2345);
            writer.add_slice_end_event(1220, 2345);
            writer.add_thread_state_event(1230, 2, ThreadState::TaskStateRunning);
        }

        let trace = Trace::decode(buffer.as_slice()).expect("Failed to decode trace");
        assert_eq!(trace.packet.len(), 11);

        // Packet 0: Process Track
        let packet = &trace.packet[0];
        assert_eq!(packet.timestamp, Some(1000));
        assert!(packet.first_packet_on_sequence.unwrap_or(false));
        let trace_packet::Data::TrackDescriptor(td) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackDescriptor");
        };
        assert_eq!(td.uuid, Some(1234));
        assert_eq!(
            td.static_or_dynamic_name,
            Some(StaticOrDynamicName::Name("example process".to_string()))
        );
        let process = td.process.as_ref().unwrap();
        assert_eq!(process.pid, Some(1));
        assert_eq!(process.process_name, Some("example process".to_string()));

        // Packet 1: Counter Track
        let packet = &trace.packet[1];
        assert_eq!(packet.timestamp, Some(1000));
        let trace_packet::Data::TrackDescriptor(td) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackDescriptor");
        };
        assert_eq!(td.uuid, Some(1235));
        assert_eq!(td.parent_uuid, Some(1234));
        assert_eq!(
            td.static_or_dynamic_name,
            Some(StaticOrDynamicName::Name("counter".to_string()))
        );
        let counter = td.counter.as_ref().unwrap();
        assert_eq!(counter.unit_name, Some("counter".to_string()));

        // Packet 2: Thread Track
        let packet = &trace.packet[2];
        assert_eq!(packet.timestamp, Some(1000));
        let trace_packet::Data::TrackDescriptor(td) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackDescriptor");
        };
        assert_eq!(td.uuid, Some(2345));
        assert_eq!(td.parent_uuid, Some(1234));
        assert_eq!(
            td.static_or_dynamic_name,
            Some(StaticOrDynamicName::Name("main thread".to_string()))
        );
        let thread = td.thread.as_ref().unwrap();
        assert_eq!(thread.pid, Some(1));
        assert_eq!(thread.tid, Some(2));
        assert_eq!(thread.thread_name, Some("main thread".to_string()));

        // Packet 3: Counter Event (val=2)
        let packet = &trace.packet[3];
        assert_eq!(packet.timestamp, Some(1100));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::Counter.into()));
        assert_eq!(te.track_uuid, Some(1235));
        assert_eq!(
            te.counter_value_field,
            Some(track_event::CounterValueField::CounterValue(2))
        );

        // Packet 4: Counter Event (val=3)
        let packet = &trace.packet[4];
        assert_eq!(packet.timestamp, Some(1200));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::Counter.into()));
        assert_eq!(te.track_uuid, Some(1235));
        assert_eq!(
            te.counter_value_field,
            Some(track_event::CounterValueField::CounterValue(3))
        );

        // Packet 5: Counter Event (val=4)
        let packet = &trace.packet[5];
        assert_eq!(packet.timestamp, Some(1300));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::Counter.into()));
        assert_eq!(te.track_uuid, Some(1235));
        assert_eq!(
            te.counter_value_field,
            Some(track_event::CounterValueField::CounterValue(4))
        );

        // Packet 6: Slice Begin "a"
        let packet = &trace.packet[6];
        assert_eq!(packet.timestamp, Some(1010));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::SliceBegin.into()));
        assert_eq!(te.track_uuid, Some(2345));
        assert_eq!(
            te.name_field,
            Some(track_event::NameField::Name("a".to_string()))
        );

        // Packet 7: Slice Begin "b"
        let packet = &trace.packet[7];
        assert_eq!(packet.timestamp, Some(1040));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::SliceBegin.into()));
        assert_eq!(te.track_uuid, Some(2345));
        assert_eq!(
            te.name_field,
            Some(track_event::NameField::Name("b".to_string()))
        );

        // Packet 8: Slice End
        let packet = &trace.packet[8];
        assert_eq!(packet.timestamp, Some(1180));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::SliceEnd.into()));
        assert_eq!(te.track_uuid, Some(2345));

        // Packet 9: Slice End
        let packet = &trace.packet[9];
        assert_eq!(packet.timestamp, Some(1220));
        let trace_packet::Data::TrackEvent(te) = packet.data.as_ref().unwrap() else {
            panic!("Expected TrackEvent");
        };
        assert_eq!(te.r#type, Some(track_event::Type::SliceEnd.into()));
        assert_eq!(te.track_uuid, Some(2345));

        // Packet 10: Thread State
        let packet = &trace.packet[10];
        assert_eq!(packet.timestamp, Some(1230));
        let trace_packet::Data::GenericKernelTaskStateEvent(se) = packet.data.as_ref().unwrap()
        else {
            panic!("Expected GenericKernelTaskStateEvent");
        };
        assert_eq!(se.tid, Some(2));
        assert_eq!(se.state, Some(ThreadState::TaskStateRunning.into()));
    }
}
