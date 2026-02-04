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
#pragma once

#include <cstddef>
#include <cstdint>

namespace pw::multibuf::examples {

// Note: This file includes explicit constants for the packed length of each
// struct. `sizeof(...)` includes unwanted padding bytes for alignment, and
// trying to use `PW_PACKED(struct)` can lead to undefined behavior when
// accessing unaligned fields. Additionally, they must be marked "inline" to
// avoid clang-tidy warnings on darwin.

// DOCSTAG: [pw_multibuf-examples-protocol-link_frame]
// Protocol DemoLink has frames up to 1014 bytes in length with 6-byte headers
// (2 bytes each for src addr, dst addr, and len) and a 4 byte crc32 checksum.
struct DemoLinkHeader {
  uint16_t src_addr;
  uint16_t dst_addr;
  uint16_t length;
};

struct DemoLinkFooter {
  uint32_t crc32;
};
// DOCSTAG: [pw_multibuf-examples-protocol-link_frame]

inline constexpr size_t kDemoLinkHeaderLen = sizeof(uint16_t) * 3;
inline constexpr size_t kDemoLinkFooterLen = sizeof(uint32_t) * 3;
inline constexpr size_t kMaxDemoLinkFrameLength = 1 << 10;

// DOCSTAG: [pw_multibuf-examples-protocol-network_packet]
// Protocol DemoNetwork have packets that fit entirely within a DemoLink frame.
// They have 20-byte headers (8 byte src and dst address, and a 4 byte packet
// length).
struct DemoNetworkHeader {
  uint64_t src_addr;
  uint64_t dst_addr;
  uint32_t length;
};
// DOCSTAG: [pw_multibuf-examples-protocol-network_packet]

inline constexpr size_t kDemoNetworkHeaderLen =
    sizeof(uint64_t) * 2 + sizeof(uint32_t);

// DOCSTAG: [pw_multibuf-examples-protocol-transport_segment]
// Protocol DemoTransport has segments up to ~4 GiB spanning multiple packets.
// Each fragment of a segment includes a 16 byte header that includes a
// segment ID, offset and length. The first fragment has and additional 4 byte
// field for the total segment length.
struct DemoTransportHeader {
  uint64_t segment_id;
  uint32_t offset;
  uint32_t length;
};

// DOCSTAG: [pw_multibuf-examples-protocol-transport_segment]

inline constexpr size_t kDemoTransportHeaderLen =
    sizeof(uint64_t) + sizeof(uint32_t) * 2;

struct DemoTransportFirstHeader : DemoTransportHeader {
  uint32_t total_length;
};

}  // namespace pw::multibuf::examples
