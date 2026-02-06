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

#include "pw_multibuf/size_report/sender.h"

namespace pw::multibuf::size_report {

void BasicSender::Send(const char* message) {
  message_ = as_bytes(span(std::string_view(message)));
  demo_transport_header_.segment_id++;
  demo_transport_header_.total_length =
      static_cast<uint32_t>(message_.size_bytes());
  waker_.Wake();
}

ConstByteSpan BasicSender::GetDemoLinkHeader(size_t payload_len) {
  demo_link_header_.length = static_cast<uint16_t>(payload_len);
  return as_bytes(span(&demo_link_header_, 1));
}

ConstByteSpan BasicSender::GetDemoLinkFooter(uint32_t checksum) {
  demo_link_footer_.crc32 = checksum;
  return as_bytes(span(&demo_link_footer_, 1));
}

ConstByteSpan BasicSender::GetDemoNetworkHeader(size_t payload_len) {
  demo_network_header_.length = static_cast<uint32_t>(payload_len);
  return as_bytes(span(&demo_network_header_, 1));
}

ConstByteSpan BasicSender::GetDemoTransportHeader(size_t payload_len) {
  demo_transport_header_.length = static_cast<uint32_t>(payload_len);
  if (offset() == 0) {
    return as_bytes(span(&demo_transport_header_, 1));
  }

  examples::DemoTransportHeader* demo_transport_header =
      &demo_transport_header_;
  return as_bytes(span(demo_transport_header, 1));
}

ConstByteSpan BasicSender::GetMessageFragment(size_t segment_size) {
  segment_size = std::min(segment_size, remaining());
  return message_.first(segment_size);
}

void BasicSender::AdvanceOffset(size_t off) {
  message_ = message_.subspan(off);
  demo_transport_header_.offset += off;
}

}  // namespace pw::multibuf::size_report
