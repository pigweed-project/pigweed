// Copyright 2024 The Pigweed Authors
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
//

#include "pw_bluetooth_sapphire/internal/host/iso/iso_inbound_packet_assembler.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_data.emb.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt::iso {

void IsoInboundPacketAssembler::ProcessNext(pw::span<const std::byte> packet) {
  auto packet_view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      packet.data(), packet.size());

  // This should have been checked by the caller.
  PW_CHECK(packet_view.Ok());

  pw::bluetooth::emboss::IsoDataPbFlag pb_flag =
      packet_view.header().pb_flag().Read();
  if ((pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU) ||
      (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT)) {
    // If this is the start of an SDU, we shouldn't have anything in the buffer
    if (!assembly_buffer_.empty()) {
      bt_log(ERROR, "iso", "Incomplete ISO packet received - discarding");
      assembly_buffer_.clear();
    }
  }

  if (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU) {
    PW_CHECK(complete_packet_handler_);
    complete_packet_handler_(packet);
    return;
  }

  // When we encounter the first fragment of an SDU, we just copy everything
  // into the temporary buffer.
  if (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT) {
    // Make sure our buffer has sufficient space to hold the entire assembled
    // ISO SDU frame
    size_t assembled_frame_size =
        static_cast<size_t>(packet_view.sdu_fragment_offset().Read()) +
        static_cast<size_t>(packet_view.iso_sdu_length().Read());
    if (assembled_frame_size > assembly_buffer_.capacity()) {
      assembly_buffer_.reserve(assembled_frame_size);
    }

    assembly_buffer_.resize(packet.size());
    std::copy(packet.begin(), packet.end(), assembly_buffer_.begin());
    return;
  }

  PW_CHECK((pb_flag ==
            pw::bluetooth::emboss::IsoDataPbFlag::INTERMEDIATE_FRAGMENT) ||
           (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT));
  if (!AppendFragment(packet)) {
    return;
  }

  if (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT) {
    pw::span<const std::byte> assembly_buffer_span(assembly_buffer_.data(),
                                                   assembly_buffer_.size());
    complete_packet_handler_(assembly_buffer_span);
  }
}

bool IsoInboundPacketAssembler::AppendFragment(
    pw::span<const std::byte> packet) {
  // Make sure we have previously received fragments
  if (assembly_buffer_.empty()) {
    bt_log(
        ERROR, "iso", "Out-of-order ISO packet fragment received - discarding");
    return false;
  }

  auto assembly_buffer_view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      assembly_buffer_.data(), assembly_buffer_.size());
  PW_DCHECK(assembly_buffer_view.Ok());

  auto fragment_view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      packet.data(), packet.size());
  PW_DCHECK(fragment_view.Ok());

  // A failure here would indicate that packets are being incorrectly routed to
  // the appropriate stream (since we should be using the connection handle to
  // figure out where to send the packet).
  PW_CHECK(assembly_buffer_view.header().connection_handle().Read() ==
           fragment_view.header().connection_handle().Read());

  size_t total_sdu_bytes_received =
      static_cast<size_t>(assembly_buffer_view.sdu_fragment_size().Read()) +
      static_cast<size_t>(fragment_view.sdu_fragment_size().Read());
  size_t complete_sdu_length =
      static_cast<size_t>(assembly_buffer_view.iso_sdu_length().Read());

  // Verify that the total amount of SDU data received does not exceed that
  // specified in the header from the FIRST_FRAGMENT.
  if (total_sdu_bytes_received > complete_sdu_length) {
    bt_log(ERROR,
           "iso",
           "Invalid data fragments received, exceed total SDU length - "
           "discarding");
    assembly_buffer_.clear();
    return false;
  }

  bool is_last_fragment = fragment_view.header().pb_flag().Read() ==
                          pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT;
  if (is_last_fragment && (total_sdu_bytes_received < complete_sdu_length)) {
    bt_log(ERROR,
           "iso",
           "Insufficient data fragments received (%zu bytes received, expected "
           "%zu) - discarding",
           total_sdu_bytes_received,
           complete_sdu_length);
    assembly_buffer_.clear();
    return false;
  }

  // Append the SDU data
  assembly_buffer_.insert(
      assembly_buffer_.end(),
      fragment_view.iso_sdu_fragment().BackingStorage().begin(),
      fragment_view.iso_sdu_fragment().BackingStorage().end());

  // Update the header fields so they are as consistent as possible
  assembly_buffer_view.header().data_total_length().Write(
      assembly_buffer_.size() - assembly_buffer_view.hdr_size().Read());
  if (is_last_fragment) {
    assembly_buffer_view.header().pb_flag().Write(
        pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU);
  }
  return true;
}

}  // namespace bt::iso
