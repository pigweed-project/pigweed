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

#include "pw_bluetooth_proxy/internal/basic_mode_tx_engine.h"

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

Result<H4PacketWithH4> BasicModeTxEngine::GenerateNextPacket(
    const multibuf::MultiBuf& payload, bool& keep_payload) {
  keep_payload = true;
  const uint16_t data_length = static_cast<uint16_t>(payload.size());

  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + data_length;
  const size_t h4_packet_size = H4SizeForL2capData(data_length);

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

  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl.payload().BackingStorage().data(), acl.payload().SizeInBytes());
  bframe.pdu_length().Write(data_length);
  bframe.channel_id().Write(remote_cid_);
  PW_CHECK(bframe.IsComplete());

  auto bframe_payload = bframe.payload();
  span<uint8_t> backing_storage(bframe_payload.BackingStorage().data(),
                                bframe_payload.SizeInBytes());
  auto bytes_copied = payload.CopyTo(as_writable_bytes(backing_storage));
  PW_CHECK_UINT_EQ(bytes_copied.size(), backing_storage.size());
  PW_CHECK(acl.Ok());
  PW_CHECK(bframe.Ok());

  // All content has been copied from the front payload, so release it.
  keep_payload = false;
  return h4_packet;
}

Status BasicModeTxEngine::CheckWriteParameter(
    const multibuf::MultiBuf& payload) {
  std::optional<uint16_t> max_l2cap_length = delegate_.MaxL2capPayloadSize();
  if (!max_l2cap_length) {
    return Status::FailedPrecondition();
  }
  if (payload.size() > max_l2cap_length) {
    PW_LOG_WARN("Payload (%zu bytes) is too large. So will not process.",
                payload.size());
    return Status::InvalidArgument();
  }
  return OkStatus();
}

BasicModeTxEngine::HandlePduFromHostReturnValue
BasicModeTxEngine::HandlePduFromHost(pw::span<uint8_t> frame) {
  Result<emboss::BFrameWriter> bframe_view =
      MakeEmbossWriter<emboss::BFrameWriter>(frame);

  if (!bframe_view.ok()) {
    // TODO: https://pwbug.dev/360929142 - Stop channel on error.
    PW_LOG_ERROR("Host transmitted invalid B-frame to 0x%X. So will drop.",
                 remote_cid_);
    return {.forward_to_controller = false, .send_to_client = std::nullopt};
  }

  pw::span payload(bframe_view->payload().BackingStorage().data(),
                   bframe_view->payload().SizeInBytes());

  return {.forward_to_controller = true, .send_to_client = payload};
}
}  // namespace pw::bluetooth::proxy::internal
