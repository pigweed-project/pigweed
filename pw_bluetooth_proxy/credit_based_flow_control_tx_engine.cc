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

#include "pw_bluetooth_proxy/internal/credit_based_flow_control_tx_engine.h"

#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_log/log.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy::internal {

namespace {
constexpr size_t H4SizeForL2capData(uint16_t data_length) {
  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + data_length;
  const size_t acl_packet_size =
      emboss::AclDataFrameHeader::IntrinsicSizeInBytes() + l2cap_packet_size;
  return sizeof(emboss::H4PacketType) + acl_packet_size;
}

}  // namespace

CreditBasedFlowControlTxEngine::CreditBasedFlowControlTxEngine(
    ConnectionOrientedChannelConfig& config,
    uint16_t connection_handle,
    uint16_t local_cid,
    Delegate& delegate)
    : delegate_(delegate),
      mtu_(config.mtu),
      mps_(config.mps),
      connection_handle_(connection_handle),
      remote_cid_(config.cid),
      local_cid_(local_cid),
      credits_(config.credits) {}

Result<H4PacketWithH4> CreditBasedFlowControlTxEngine::GenerateNextPacket(
    const multibuf::MultiBuf& sdu, bool& keep_payload) {
  keep_payload = true;
  constexpr uint8_t kSduLengthFieldSize =
      emboss::FirstKFrame::MinSizeInBytes() -
      emboss::BasicL2capHeader::IntrinsicSizeInBytes();
  std::optional<uint16_t> max_basic_payload_size = MaxBasicL2capPayloadSize();
  if (credits_ == 0 || !max_basic_payload_size ||
      *max_basic_payload_size <= kSduLengthFieldSize) {
    return Status::Unavailable();
  }

  // Number of client SDU bytes to be encoded in this segment.
  uint16_t sdu_bytes_in_segment;
  // Size of PDU payload for this L2CAP frame.
  uint16_t pdu_data_size;
  if (!is_continuing_segment_) {
    // Generating the first (or only) PDU of an SDU.
    size_t sdu_bytes_max_allowable =
        *max_basic_payload_size - kSduLengthFieldSize;
    sdu_bytes_in_segment =
        static_cast<uint16_t>(std::min(sdu.size(), sdu_bytes_max_allowable));
    pdu_data_size = sdu_bytes_in_segment + kSduLengthFieldSize;
  } else {
    // Generating a continuing PDU in an SDU.
    size_t sdu_bytes_max_allowable = *max_basic_payload_size;
    sdu_bytes_in_segment = static_cast<uint16_t>(
        std::min(sdu.size() - sdu_offset_, sdu_bytes_max_allowable));
    pdu_data_size = sdu_bytes_in_segment;
  }

  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + pdu_data_size;
  const size_t h4_packet_size = H4SizeForL2capData(pdu_data_size);
  PW_TRY_ASSIGN(H4PacketWithH4 h4_packet,
                delegate_.AllocateH4(static_cast<uint16_t>(h4_packet_size)));
  h4_packet.SetH4Type(emboss::H4PacketType::ACL_DATA);

  PW_TRY_ASSIGN(
      auto acl,
      MakeEmbossWriter<emboss::AclDataFrameWriter>(h4_packet.GetHciSpan()));
  acl.header().handle().Write(connection_handle_);
  acl.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  acl.header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  acl.data_total_length().Write(l2cap_packet_size);

  span<uint8_t> backing_storage;
  if (!is_continuing_segment_) {
    Result<emboss::FirstKFrameWriter> first_kframe_writer =
        MakeEmbossWriter<emboss::FirstKFrameWriter>(
            acl.payload().BackingStorage().data(), acl.payload().SizeInBytes());
    PW_CHECK(first_kframe_writer.ok());
    first_kframe_writer->pdu_length().Write(pdu_data_size);
    first_kframe_writer->channel_id().Write(remote_cid_);
    first_kframe_writer->sdu_length().Write(sdu.size());
    // Emboss's Ok() checks the payload array, which would trigger MSan
    // use-of-uninitialized-value if checked before the payload is copied.
    // So we only check IsComplete() bounds check here.
    PW_CHECK(first_kframe_writer->IsComplete());

    auto payload = first_kframe_writer->payload();
    backing_storage =
        span<uint8_t>(payload.BackingStorage().data(), payload.SizeInBytes());
  } else {
    Result<emboss::SubsequentKFrameWriter> subsequent_kframe_writer =
        MakeEmbossWriter<emboss::SubsequentKFrameWriter>(
            acl.payload().BackingStorage().data(), acl.payload().SizeInBytes());
    PW_CHECK(subsequent_kframe_writer.ok());
    subsequent_kframe_writer->pdu_length().Write(pdu_data_size);
    subsequent_kframe_writer->channel_id().Write(remote_cid_);
    PW_CHECK(subsequent_kframe_writer->IsComplete());

    auto payload = subsequent_kframe_writer->payload();
    backing_storage =
        span<uint8_t>(payload.BackingStorage().data(), payload.SizeInBytes());
  }

  backing_storage = backing_storage.subspan(0, sdu_bytes_in_segment);
  auto bytes_copied =
      sdu.CopyTo(as_writable_bytes(backing_storage), sdu_offset_);
  PW_CHECK_UINT_EQ(bytes_copied.size(), sdu_bytes_in_segment);
  sdu_offset_ += sdu_bytes_in_segment;

  if (sdu_offset_ == sdu.size()) {
    // This segment was the final (or only) PDU of the SDU payload. So all
    // content has been copied from the front payload so it can be released.
    keep_payload = false;
    sdu_offset_ = 0;
    is_continuing_segment_ = false;
  } else {
    is_continuing_segment_ = true;
  }

  --credits_;
  return h4_packet;
}

Status CreditBasedFlowControlTxEngine::CheckWriteParameter(
    const multibuf::MultiBuf& payload) {
  if (payload.size() > mtu_) {
    PW_LOG_ERROR(
        "Payload (%zu bytes) exceeds MTU (%d bytes). So will not process. "
        "local_cid: %#x, remote_cid: %#x",
        payload.size(),
        mtu_,
        local_cid_,
        remote_cid_);
    return Status::InvalidArgument();
  }
  return pw::OkStatus();
}

Result<bool> CreditBasedFlowControlTxEngine::AddCredits(uint16_t credits) {
  // Core Spec v6.0 Vol 3, Part A, 10.1: "The device receiving the credit
  // packet shall disconnect the L2CAP channel if the credit count exceeds
  // 65535."
  if (credits >
      emboss::L2capLeCreditBasedConnectionReq::max_credit_value() - credits_) {
    PW_LOG_ERROR(
        "btproxy: Received additional tx credits %u which put tx_credits_ %u "
        "beyond max credit value of %ld. So stopping channel and reporting "
        "it needs to be closed. local_cid: %u, remote_cid: %u",
        credits,
        credits_,
        long{emboss::L2capLeCreditBasedConnectionReq::max_credit_value()},
        local_cid_,
        remote_cid_);
    return Status::InvalidArgument();
  }

  bool credits_previously_zero = credits_ == 0;
  credits_ += credits;
  return credits_previously_zero;
}

TxEngine::HandlePduFromHostReturnValue
CreditBasedFlowControlTxEngine::HandlePduFromHost(pw::span<uint8_t>) {
  // Always forward data from host to controller
  return {.forward_to_controller = true, .send_to_client = std::nullopt};
}

std::optional<uint16_t>
CreditBasedFlowControlTxEngine::MaxBasicL2capPayloadSize() const {
  std::optional<uint16_t> max_basic_l2cap_payload_size =
      delegate_.MaxL2capPayloadSize();
  if (!max_basic_l2cap_payload_size) {
    return std::nullopt;
  }
  return std::min(*max_basic_l2cap_payload_size, mps_);
}

}  // namespace pw::bluetooth::proxy::internal
