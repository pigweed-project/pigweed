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

#[cfg(feature = "std")]
use byteorder::ByteOrder;

/// A trace record.
///
/// Trace records are fixed-size, 16-byte structures consisting of a header (timestamp,
/// and event type) and event specific data.
///
/// # Structure
/// | Bits       | Field      | Description                 |
/// |------------|------------|-----------------------------|
/// | \[0:25\]   | time_stamp | Time stamp of the event     |
/// | \[26:31\]  | event_type | [`EventType`] of the record |
/// | \[32:127\] | event_data | Event specific data         |
#[derive(Debug, Clone, Copy)]
pub struct Record {
    pub event_type: EventType,
    pub time_stamp: u32,
    pub event_data: [u32; 3],
}

impl Record {
    /// Reads a record from a byte buffer.
    ///
    /// The buffer must be at least 16 bytes long.
    /// The endianness `T` determines how the bytes are interpreted.
    ///
    /// # Arguments
    /// * `buffer`: A slice of at least 16 bytes.
    ///
    /// # Returns
    /// * `Some(Record)` if the buffer is large enough and the event type is valid.
    /// * `None` otherwise.
    ///
    /// # Example
    /// ```
    /// use pw_kernel_tracing::{Record, EventType};
    /// use byteorder::LittleEndian;
    ///
    /// let bytes = [
    ///     0x00, 0x00, 0x00, 0x00, // Header (Timestamp 0, EventType ContextSwitch)
    ///     0x01, 0x00, 0x00, 0x00, // Data 0
    ///     0x02, 0x00, 0x00, 0x00, // Data 1
    ///     0x03, 0x00, 0x00, 0x00, // Data 2
    /// ];
    /// let record = Record::read_from_buffer::<LittleEndian>(&bytes).unwrap();
    /// assert_eq!(record.event_type, EventType::ContextSwitch);
    /// ```
    #[cfg(feature = "std")]
    #[must_use]
    pub fn read_from_buffer<T: ByteOrder>(buffer: &[u8]) -> Option<Self> {
        if buffer.len() < 16 {
            return None;
        }

        let header = T::read_u32(&buffer[0..4]);
        let time_stamp = header & 0x03ff_ffff;
        let event_type_val = (header >> 26) & 0x3f;

        let event_type = EventType::try_from(event_type_val as u8).ok()?;

        let event_data = [
            T::read_u32(&buffer[4..8]),
            T::read_u32(&buffer[8..12]),
            T::read_u32(&buffer[12..16]),
        ];

        Some(Self {
            event_type,
            time_stamp,
            event_data,
        })
    }

    /// Encodes the record into a `[u32; 4]` array.
    ///
    /// The encoding is endian-agnostic in the sense that the `u32` words contain the
    /// architectural values. When writing these to bytes, care must be taken to maintain
    /// consistency.
    ///
    /// # Returns
    /// A `[u32; 4]` buffer containing the encoded record.
    #[must_use]
    pub const fn encode(&self) -> [u32; 4] {
        let header =
            (self.time_stamp & 0x03ff_ffff) | (((self.event_type as u8 & 0x3f) as u32) << 26);

        // Though these use from_le_bytes, the bytes are explicitly added in little
        // endian order so that the endianness of the target does not matter.
        // This is done to avoid unsafe transmutes and optimizes out 32 bit word
        // assignments.
        [
            header,
            self.event_data[0],
            self.event_data[1],
            self.event_data[2],
        ]
    }

    /// Parses the event data into a strongly-typed [`EventPayload`].
    ///
    /// # Returns
    /// * `Some(EventPayload)` if the event data can be parsed for the record's `event_type`.
    /// * `None` if parsing fails.
    #[must_use]
    pub fn payload(&self) -> Option<EventPayload> {
        match self.event_type {
            EventType::ContextSwitch => Some(EventPayload::ContextSwitch(
                ContextSwitchEvent::read_from_buffer(&self.event_data)?,
            )),
            EventType::SpanStart => Some(EventPayload::Span(TraceSpanEvent::read_from_buffer(
                &self.event_data,
            )?)),
            EventType::SpanEnd => Some(EventPayload::Span(TraceSpanEvent::read_from_buffer(
                &self.event_data,
            )?)),
        }
    }
}

/// Strongly-typed payload for defined event types.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventPayload {
    /// A context switch event.
    ContextSwitch(ContextSwitchEvent),
    Span(TraceSpanEvent),
}

/// Supported trace event types.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum EventType {
    // NOTE: The values here must be less than 64 to fit in the 6 bit field in the header.
    /// Context Switch Event
    ContextSwitch = 0,
    SpanStart = 1,
    SpanEnd = 2,
}

impl TryFrom<u8> for EventType {
    type Error = ();

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(EventType::ContextSwitch),
            1 => Ok(EventType::SpanStart),
            2 => Ok(EventType::SpanEnd),
            _ => Err(()),
        }
    }
}

/// Context switch event data
///
/// | Bits      | Field            | Description                                                    |
/// |-----------|------------------|----------------------------------------------------------------|
/// | \[0:31\]  | old_thread_id    | ID of the thread that was running before the context switch    |
/// | \[32:63\] | new_thread_id    | ID of the thread that is running after the context switch      |
/// | \[64:71\] | old_thread_state | State of the thread that was running before the context switch |
/// | \[72:95\] | reserved         | Read Ignored Write Zero                                        |
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ContextSwitchEvent {
    pub old_thread_id: u32,
    pub new_thread_id: u32,
    pub old_thread_state: u8,
}

impl ContextSwitchEvent {
    /// Encodes the event into a `u32` buffer
    #[must_use]
    pub const fn encode(&self) -> [u32; 3] {
        [
            self.old_thread_id,
            self.new_thread_id,
            self.old_thread_state as u32,
        ]
    }

    /// Reads a context switch event from a `u32` buffer.
    ///
    /// # Arguments
    /// * `buffer`: A slice of at least 3 `u32` words.
    #[must_use]
    pub fn read_from_buffer(buffer: &[u32]) -> Option<Self> {
        if buffer.len() < 3 {
            return None;
        }

        let old_thread_id = buffer[0];
        let new_thread_id = buffer[1];
        let old_thread_state = (buffer[2] & 0xff) as u8;

        Some(Self {
            old_thread_id,
            new_thread_id,
            old_thread_state,
        })
    }
}

/// Trace span event data
///
/// | Bits      | Field            | Description                                                    |
/// |-----------|------------------|----------------------------------------------------------------|
/// | \[0:96\] | message          | The message associated with this span, encoded with pw_tokenizer|
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TraceSpanEvent {
    pub message: [u8; 12],
}

impl TraceSpanEvent {
    /// Encodes the event into a `u32` buffer
    #[must_use]
    pub const fn encode(&self) -> [u32; 3] {
        let (first, second, third): ([u8; 4], [u8; 4], [u8; 4]) = match self.message {
            [a, b, c, d, e, f, g, h, i, j, k, l] => ([a, b, c, d], [e, f, g, h], [i, j, k, l]),
        };

        // Always keep the buffer in little endian, so it doesn't get reordered by the target
        // endianness or the host endianness
        [
            u32::from_le_bytes(first).to_le(),
            u32::from_le_bytes(second).to_le(),
            u32::from_le_bytes(third).to_le(),
        ]
    }

    /// Reads a context switch event from a `u32` buffer.
    ///
    /// # Arguments
    /// * `buffer`: A slice of at least 3 `u32` words.
    #[must_use]
    pub fn read_from_buffer(buffer: &[u32]) -> Option<Self> {
        if buffer.len() < 3 {
            return None;
        }

        let (first, second, third) = (
            buffer[0].to_le_bytes(),
            buffer[1].to_le_bytes(),
            buffer[2].to_le_bytes(),
        );
        let message: [u8; 12] = {
            let [a, b, c, d] = first;
            let [e, f, g, h] = second;
            let [i, j, k, l] = third;

            [a, b, c, d, e, f, g, h, i, j, k, l]
        };

        Some(Self { message })
    }
}

#[cfg(test)]
#[allow(unused_imports)]
mod tests {
    #[cfg(feature = "std")]
    use byteorder::{BigEndian, LittleEndian};
    use unittest::test;

    use super::{ContextSwitchEvent, EventPayload, EventType, Record, TraceSpanEvent};

    #[test]
    fn context_switch_read_from_buffer_reads_correct_values() -> unittest::Result<()> {
        let buffer = [
            1, // old_thread_id = 1
            2, // new_thread_id = 2
            3, // old_thread_state = 3
        ];

        let event = ContextSwitchEvent::read_from_buffer(&buffer).unwrap();

        unittest::assert_eq!(event.old_thread_id, 1);
        unittest::assert_eq!(event.new_thread_id, 2);
        unittest::assert_eq!(event.old_thread_state, 3);

        Ok(())
    }

    #[test]
    fn context_switch_encode_generates_correct_values() -> unittest::Result<()> {
        let event = ContextSwitchEvent {
            old_thread_id: 0x01020304,
            new_thread_id: 0x05060708,
            old_thread_state: 0x09,
        };

        let buffer = event.encode();

        unittest::assert_eq!(
            buffer,
            [
                0x01020304, // old_thread_id
                0x05060708, // new_thread_id
                0x00000009, // old (09) | reserved (0) << 8
            ]
        );

        Ok(())
    }

    #[cfg(feature = "std")]
    #[test]
    fn record_read_little_endian_reads_correct_values() -> unittest::Result<()> {
        let buffer = [
            // Header:
            // bits 0-25 (time): 0x123456
            // bits 26-31 (type): 0 (ContextSwitch)
            // Value: 0x123456 | (0x00 << 26)
            // 0x00123456
            // LE bytes: 0x56, 0x34, 0x12, 0x00
            0x56, 0x34, 0x12, 0x00, // Event data (12 bytes)
            0x00, 0x01, 0x02, 0x03, // word 0 = 0x03020100
            0x04, 0x05, 0x06, 0x07, // word 1 = 0x07060504
            0x08, 0x09, 0x0a, 0x0b, // word 2 = 0x0b0a0908
        ];

        let record = Record::read_from_buffer::<LittleEndian>(&buffer).unwrap();

        unittest::assert_eq!(record.event_type, EventType::ContextSwitch);
        unittest::assert_eq!(record.time_stamp, 0x123456);
        unittest::assert_eq!(record.event_data, [0x03020100, 0x07060504, 0x0b0a0908]);

        Ok(())
    }

    #[cfg(feature = "std")]
    #[test]
    fn record_read_big_endian_returns_correct_values() -> unittest::Result<()> {
        let buffer = [
            // Header:
            // bits 0-25 (time): 0x123456
            // bits 26-31 (type): 0 (ContextSwitch)
            // Value: 0x123456 | (0x00 << 26)
            // 0x00123456
            // BE:
            // Byte 0-3: 0x00, 0x12, 0x34, 0x56
            0x00, 0x12, 0x34, 0x56, // Event data (12 bytes)
            0x00, 0x01, 0x02, 0x03, // word 0 = 0x00010203
            0x04, 0x05, 0x06, 0x07, // word 1 = 0x04050607
            0x08, 0x09, 0x0a, 0x0b, // word 2 = 0x08090a0b
        ];

        let record = Record::read_from_buffer::<BigEndian>(&buffer).unwrap();

        unittest::assert_eq!(record.event_type, EventType::ContextSwitch);
        unittest::assert_eq!(record.time_stamp, 0x123456);
        unittest::assert_eq!(record.event_data, [0x00010203, 0x04050607, 0x08090a0b]);

        Ok(())
    }

    #[test]
    fn record_encode_generates_correct_values() -> unittest::Result<()> {
        let record = Record {
            event_type: EventType::ContextSwitch,
            time_stamp: 0x123456,
            event_data: [0x03020100, 0x07060504, 0x0b0a0908],
        };
        let buffer = record.encode();

        // buffer[0] = 0x00123456 because:
        // time (0x123456) | type (0<<26)
        unittest::assert_eq!(buffer[0], 0x00123456);
        unittest::assert_eq!(buffer[1], 0x03020100);
        unittest::assert_eq!(buffer[2], 0x07060504);
        unittest::assert_eq!(buffer[3], 0x0b0a0908);

        Ok(())
    }

    #[test]
    fn record_payload_returns_correct_context_switch_event() -> unittest::Result<()> {
        let event = ContextSwitchEvent {
            old_thread_id: 1,
            new_thread_id: 2,
            old_thread_state: 3,
        };
        let encoded_event = event.encode();

        let record = Record {
            event_type: EventType::ContextSwitch,
            time_stamp: 0,
            event_data: encoded_event,
        };

        let decoded_event = unittest::unwrap!(match record.payload().unwrap() {
            EventPayload::ContextSwitch(e) => Ok(e),
            _ => Err(()),
        });
        unittest::assert_eq!(decoded_event.old_thread_id, 1);
        unittest::assert_eq!(decoded_event.new_thread_id, 2);
        unittest::assert_eq!(decoded_event.old_thread_state, 3);

        Ok(())
    }

    #[test]
    fn record_payload_returns_correct_trace_span_event() -> unittest::Result<()> {
        let event = TraceSpanEvent {
            message: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
        };
        let encoded_event = event.encode();

        let record = Record {
            event_type: EventType::SpanStart,
            time_stamp: 0,
            event_data: encoded_event,
        };

        let decoded_event = unittest::unwrap!(match record.payload().unwrap() {
            EventPayload::Span(e) => Ok(e),
            _ => Err(()),
        });
        unittest::assert_eq!(
            decoded_event.message,
            [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
        );

        Ok(())
    }

    #[test]
    fn trace_span_read_from_buffer_reads_correct_values() -> unittest::Result<()> {
        let buffer = [
            0x04030201, // message[0..3] = [1, 2, 3, 4]
            0x08070605, // message[4..8] = [5, 6, 7, 8]
            0x0c0b0a09, // message[9..12] = [9, 10, 11, 12]
        ];

        let event = TraceSpanEvent::read_from_buffer(&buffer).unwrap();

        unittest::assert_eq!(event.message, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);

        Ok(())
    }

    #[test]
    fn trace_span_encode_generates_correct_values() -> unittest::Result<()> {
        let event = TraceSpanEvent {
            message: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
        };

        let buffer = event.encode();

        unittest::assert_eq!(
            buffer,
            [
                0x04030201, // message[0..3] = [1, 2, 3, 4]
                0x08070605, // message[4..8] = [5, 6, 7, 8]
                0x0c0b0a09, // message[9..12] = [9, 10, 11, 12]
            ]
        );

        Ok(())
    }

    #[cfg(feature = "std")]
    #[test]
    fn trace_span_preserves_buffer_order_with_different_endianness_round_trip()
    -> unittest::Result<()> {
        let event = TraceSpanEvent {
            message: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
        };
        let encoded_event = event.encode();

        let record = Record {
            event_type: EventType::SpanStart,
            time_stamp: 0,
            event_data: encoded_event,
        };

        let buffer = record.encode();

        // Encode and decode the record in big endian
        let big_endian_record = {
            let buffer = buffer
                .into_iter()
                .flat_map(|x| x.to_be_bytes())
                .collect::<Vec<u8>>();
            Record::read_from_buffer::<BigEndian>(&buffer).unwrap()
        };

        unittest::assert_eq!(big_endian_record.event_type, EventType::SpanStart);
        unittest::assert_eq!(big_endian_record.time_stamp, 0);

        let decoded_event = unittest::unwrap!(match big_endian_record.payload().unwrap() {
            EventPayload::Span(e) => Ok(e),
            _ => Err(()),
        });
        unittest::assert_eq!(
            decoded_event.message,
            [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
        );

        // Encode and decode the record in little endian
        let little_endian_record = {
            let buffer = buffer
                .into_iter()
                .flat_map(|x| x.to_le_bytes())
                .collect::<Vec<u8>>();
            Record::read_from_buffer::<LittleEndian>(&buffer).unwrap()
        };

        unittest::assert_eq!(little_endian_record.event_type, EventType::SpanStart);
        unittest::assert_eq!(little_endian_record.time_stamp, 0);

        let decoded_event = unittest::unwrap!(match little_endian_record.payload().unwrap() {
            EventPayload::Span(e) => Ok(e),
            _ => Err(()),
        });
        unittest::assert_eq!(
            decoded_event.message,
            [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
        );

        Ok(())
    }
}
