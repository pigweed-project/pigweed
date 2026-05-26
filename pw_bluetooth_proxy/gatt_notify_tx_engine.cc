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

#include "pw_bluetooth_proxy/internal/gatt_notify_tx_engine.h"

#include "pw_assert/check.h"
#include "pw_bluetooth/att.emb.h"
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

Result<H4PacketWithH4> GattNotifyTxEngine::GenerateNextPacket(
    const multibuf::MultiBuf& attribute_value, bool& keep_payload) {
  keep_payload = true;
  std::optional<uint16_t> max_l2cap_payload_size =
      delegate_.MaxL2capPayloadSize();
  // This should have been caught during Write.
  PW_CHECK(max_l2cap_payload_size);
  const uint16_t max_attribute_size =
      *max_l2cap_payload_size - emboss::AttHandleValueNtf::MinSizeInBytes();
  // This should have been caught during Write.
  PW_CHECK(attribute_value.size() < max_attribute_size);

  uint16_t att_frame_size = static_cast<uint16_t>(
      emboss::AttHandleValueNtf::MinSizeInBytes() + attribute_value.size());

  const size_t l2cap_packet_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + att_frame_size;
  const size_t h4_packet_size = H4SizeForL2capData(att_frame_size);

  PW_TRY_ASSIGN(H4PacketWithH4 h4_packet,
                delegate_.AllocateH4(static_cast<uint16_t>(h4_packet_size)));
  h4_packet.SetH4Type(emboss::H4PacketType::ACL_DATA);

  // Write ACL header
  PW_TRY_ASSIGN(
      auto acl,
      MakeEmbossWriter<emboss::AclDataFrameWriter>(h4_packet.GetHciSpan()));
  acl.header().handle().Write(connection_handle_);
  acl.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  acl.header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  acl.data_total_length().Write(l2cap_packet_size);
  PW_CHECK(acl.Ok());

  // Write L2CAP B-Frame header
  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl.payload().BackingStorage().data(), acl.payload().SizeInBytes());
  bframe.pdu_length().Write(att_frame_size);
  bframe.channel_id().Write(remote_cid_);
  PW_CHECK(bframe.Ok());

  // Write ATT notify header and value
  PW_CHECK(att_frame_size == bframe.payload().BackingStorage().SizeInBytes());
  Result<emboss::AttHandleValueNtfWriter> att_notify =
      MakeEmbossWriter<emboss::AttHandleValueNtfWriter>(
          static_cast<int32_t>(attribute_value.size()),
          bframe.payload().BackingStorage().data(),
          att_frame_size);
  PW_CHECK_OK(att_notify);
  att_notify->attribute_opcode().Write(emboss::AttOpcode::ATT_HANDLE_VALUE_NTF);
  att_notify->attribute_handle().Write(attribute_handle_);

  auto attr_val = att_notify->attribute_value();
  span<uint8_t> backing_storage(attr_val.BackingStorage().data(),
                                attr_val.SizeInBytes());
  auto bytes_copied =
      attribute_value.CopyTo(as_writable_bytes(backing_storage));
  PW_CHECK_UINT_EQ(bytes_copied.size(), backing_storage.size());
  PW_CHECK(att_notify->Ok());

  // All content has been copied from the front payload, so release it.
  keep_payload = false;
  return h4_packet;
}

Status GattNotifyTxEngine::CheckWriteParameter(
    const multibuf::MultiBuf& payload) {
  std::optional<uint16_t> max_l2cap_payload_size =
      delegate_.MaxL2capPayloadSize();
  if (!max_l2cap_payload_size) {
    PW_LOG_WARN("Tried to write before LE_Read_Buffer_Size processed.");
    return Status::FailedPrecondition();
  }
  if (*max_l2cap_payload_size <= emboss::AttHandleValueNtf::MinSizeInBytes()) {
    PW_LOG_ERROR("LE ACL data packet size limit does not support writing.");
    return Status::FailedPrecondition();
  }
  const uint16_t max_attribute_size =
      *max_l2cap_payload_size - emboss::AttHandleValueNtf::MinSizeInBytes();
  if (payload.size() > max_attribute_size) {
    PW_LOG_WARN("Attribute too large (%zu > %d). So will not process.",
                payload.size(),
                max_attribute_size);
    return pw::Status::InvalidArgument();
  }

  return pw::OkStatus();
}

}  // namespace pw::bluetooth::proxy::internal
