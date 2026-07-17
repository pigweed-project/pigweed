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

#include "pw_bluetooth_sapphire/internal/host/l2cap/recombiner.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt::l2cap {
namespace {

const BasicHeader& GetBasicHeader(const hci::ACLDataPacket& fragment) {
  PW_DCHECK(fragment.packet_boundary_flag() !=
            hci_spec::ACLPacketBoundaryFlag::kContinuingFragment);
  return fragment.view().payload<BasicHeader>();
}

}  // namespace

Recombiner::Recombiner(
    hci_spec::ConnectionHandle handle,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : handle_(handle), wake_lease_provider_(wake_lease_provider) {}

Recombiner::Result Recombiner::ConsumeFragment(hci::ACLDataPacketPtr fragment) {
  PW_DCHECK(fragment);
  PW_DCHECK(fragment->connection_handle() == handle_);
  TRACE_DURATION("bluetooth", "Recombiner::AddFragment");

  if (!recombination_) {
    return ProcessFirstFragment(std::move(fragment));
  }

  // If we received a new initial packet without completing the recombination,
  // then drop the entire last sequence.
  if (fragment->packet_boundary_flag() !=
      hci_spec::ACLPacketBoundaryFlag::kContinuingFragment) {
    bt_log(
        WARN, "l2cap", "expected continuing fragment! (handle: %.4x)", handle_);
    ClearRecombination();

    // Try to initiate a new starting sequence with |fragment|.
    auto result = ProcessFirstFragment(std::move(fragment));

    // Report an error for the dropped frame, even if there was no error
    // processing |fragment| itself.
    result.frames_dropped = true;
    return result;
  }

  // Reject zero-progress continuations and cap the per-PDU fragment count so a
  // malicious controller / remote peer cannot grow PDU::fragments_ without
  // bound (or to ~65535 nodes via 1-byte fragments) while pinning wake_lease.
  if (fragment->view().payload_size() == 0u ||
      recombination_->pdu.fragment_count() >= PDU::kMaxFragmentsPerPdu) {
    bt_log(WARN,
           "l2cap",
           "dropping recombination: zero-byte or excess fragment "
           "(count: %zu, handle: %.4x)",
           recombination_->pdu.fragment_count(),
           handle_);
    ClearRecombination();
    return {.pdu = {}, .frames_dropped = true};
  }

  recombination_->accumulated_length += fragment->view().payload_size();
  recombination_->pdu.AppendFragment(std::move(fragment));
  BeginTrace();

  if (recombination_->accumulated_length >
      recombination_->expected_frame_length) {
    bt_log(
        WARN, "l2cap", "continuing fragment too long! (handle: %.4x)", handle_);
    ClearRecombination();

    // Drop |fragment| since a continuing fragment cannot begin a sequence.
    return {.pdu = {}, .frames_dropped = true};
  }

  if (recombination_->accumulated_length ==
      recombination_->expected_frame_length) {
    // The frame is complete!
    auto pdu = std::move(recombination_->pdu);
    ClearRecombination();
    return {.pdu = {std::move(pdu)}, .frames_dropped = false};
  }

  // The frame is not complete yet.
  return {.pdu = {}, .frames_dropped = false};
}

Recombiner::Result Recombiner::ProcessFirstFragment(
    hci::ACLDataPacketPtr fragment) {
  PW_DCHECK(fragment);
  PW_DCHECK(!recombination_);

  // The first fragment needs to at least contain the Basic L2CAP header and
  // should not be a continuation fragment.
  size_t current_length = fragment->view().payload_size();
  if (fragment->packet_boundary_flag() ==
          hci_spec::ACLPacketBoundaryFlag::kContinuingFragment ||
      current_length < sizeof(BasicHeader)) {
    bt_log(DEBUG, "l2cap", "bad first fragment (size: %zu)", current_length);
    return {.pdu = {}, .frames_dropped = true};
  }

  // TODO(armansito): Also validate that the controller honors the HCI packet
  // boundary flag contract for the controller-to-host flow direction.

  size_t expected_frame_length =
      static_cast<uint16_t>(pw::bytes::ConvertOrderFrom(
          cpp20::endian::little, GetBasicHeader(*fragment).length)) +
      sizeof(BasicHeader);

  if (current_length > expected_frame_length) {
    bt_log(DEBUG,
           "l2cap",
           "fragment malformed: payload too long (expected length: %zu, "
           "fragment length: %zu)",
           expected_frame_length,
           current_length);
    return {.pdu = {}, .frames_dropped = true};
  }

  // We can start building a PDU.
  PDU pdu;
  pdu.AppendFragment(std::move(fragment));

  if (current_length == expected_frame_length) {
    // The PDU is complete.
    return {.pdu = {std::move(pdu)}, .frames_dropped = false};
  }

  pw::bluetooth_sapphire::Lease lease =
      PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_, "Recombiner")
          .value_or(pw::bluetooth_sapphire::Lease());

  // We need to recombine multiple fragments to obtain a complete PDU.
  BeginTrace();
  recombination_ = {.pdu = std::move(pdu),
                    .expected_frame_length = expected_frame_length,
                    .accumulated_length = current_length,
                    .wake_lease = std::move(lease)};
  return {.pdu = {}, .frames_dropped = false};
}

void Recombiner::ClearRecombination() {
  PW_DCHECK(recombination_);
  if (recombination_->pdu.is_valid()) {
    bt_log(DEBUG,
           "l2cap",
           "recombiner dropped packet (fragments: %zu, expected length: %zu, "
           "accumulated length: "
           "%zu, handle: %.4x)",
           recombination_->pdu.fragment_count(),
           recombination_->expected_frame_length,
           recombination_->accumulated_length,
           handle_);
  }
  recombination_.reset();
  EndTraces();
}

void Recombiner::BeginTrace() {
  if (!TRACE_ENABLED()) {
    return;
  }
  trace_flow_id_t flow_id = TRACE_NONCE();
  TRACE_FLOW_BEGIN(
      "bluetooth", "Recombiner buffered ACL data fragment", flow_id);
  trace_ids_.push_back(flow_id);
}

void Recombiner::EndTraces() {
  if (!TRACE_ENABLED()) {
    return;
  }
  for ([[maybe_unused]] auto flow_id : trace_ids_) {
    TRACE_FLOW_END(
        "bluetooth", "Recombiner buffered ACL data fragment", flow_id);
  }
  trace_ids_.clear();
}

}  // namespace bt::l2cap
