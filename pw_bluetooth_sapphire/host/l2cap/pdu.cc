// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/l2cap/pdu.h"

#include <pw_assert/check.h>

#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"

namespace bt::l2cap {

// NOTE: The order in which these are initialized matters, as
// other.ReleaseFragments() resets |other.fragment_count_|.
PDU::PDU(PDU&& other)
    : fragments_(other.ReleaseFragments()),
      trace_id_(std::exchange(other.trace_id_, 0)) {}

PDU& PDU::operator=(PDU&& other) {
  // NOTE: The order in which these are initialized matters, as
  // other.ReleaseFragments() resets |other.fragment_count_|.
  fragments_ = other.ReleaseFragments();
  trace_id_ = std::exchange(other.trace_id_, 0);
  return *this;
}

size_t PDU::Copy(MutableByteBuffer* out_buffer, size_t pos, size_t size) const {
  PW_DCHECK(out_buffer);
  PW_DCHECK(pos <= length());
  PW_DCHECK(is_valid());

  size_t remaining = std::min(size, length() - pos);
  PW_DCHECK(out_buffer->size() >= remaining);
  if (!remaining) {
    return 0;
  }

  bool found = false;
  size_t offset = 0u;
  for (auto iter = fragments_.begin(); iter != fragments_.end() && remaining;
       ++iter) {
    auto payload = (*iter)->view().payload_data();

    // Skip the Basic L2CAP header for the first fragment.
    if (iter == fragments_.begin()) {
      payload = payload.view(sizeof(BasicHeader));
    }

    // We first find the beginning fragment based on |pos|.
    if (!found) {
      size_t fragment_size = payload.size();
      if (pos >= fragment_size) {
        pos -= fragment_size;
        continue;
      }

      // The beginning fragment has been found.
      found = true;
    }

    // Calculate how much to read from the current fragment
    size_t write_size = std::min(payload.size() - pos, remaining);

    // Read the fragment into out_buffer->mutable_data() + offset.
    out_buffer->Write(payload.data() + pos, write_size, offset);

    // Clear |pos| after using it on the first fragment as all successive
    // fragments are read from the beginning.
    if (pos)
      pos = 0u;

    offset += write_size;
    remaining -= write_size;
  }

  return offset;
}

PDU::FragmentList PDU::ReleaseFragments() {
  auto out_list = std::move(fragments_);

  PW_DCHECK(!is_valid());
  return out_list;
}

const BasicHeader& PDU::basic_header() const {
  PW_DCHECK(!fragments_.empty());
  const auto& fragment = *fragments_.begin();

  PW_DCHECK(fragment->packet_boundary_flag() !=
            hci_spec::ACLPacketBoundaryFlag::kContinuingFragment);
  return fragment->view().payload<BasicHeader>();
}

void PDU::AppendFragment(hci::ACLDataPacketPtr fragment) {
  PW_DCHECK(fragment);
  PW_DCHECK(!is_valid() || (*fragments_.begin())->connection_handle() ==
                               fragment->connection_handle());
  fragments_.push_back(std::move(fragment));
}

}  // namespace bt::l2cap
