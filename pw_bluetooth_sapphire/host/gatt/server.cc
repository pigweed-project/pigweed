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

#include "pw_bluetooth_sapphire/internal/host/gatt/server.h"

#include <lib/fit/function.h>
#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/att/database.h"
#include "pw_bluetooth_sapphire/internal/host/att/permissions.h"
#include "pw_bluetooth_sapphire/internal/host/common/slab_allocator.h"
#include "pw_bluetooth_sapphire/internal/host/common/trace.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

namespace bt::gatt {

class AttBasedServer final : public Server {
 public:
  AttBasedServer(PeerId peer_id,
                 LocalServiceManager::WeakPtr local_services,
                 att::Bearer::WeakPtr bearer)
      : peer_id_(peer_id),
        local_services_(std::move(local_services)),
        att_(std::move(bearer)),
        weak_self_(this) {
    PW_CHECK(local_services_.is_alive());
    PW_DCHECK(att_.is_alive());

    exchange_mtu_id_ = att_->RegisterHandler(
        att::kExchangeMTURequest,
        fit::bind_member<&AttBasedServer::OnExchangeMTU>(this));
    find_information_id_ = att_->RegisterHandler(
        att::kFindInformationRequest,
        fit::bind_member<&AttBasedServer::OnFindInformation>(this));
    read_by_group_type_id_ = att_->RegisterHandler(
        att::kReadByGroupTypeRequest,
        fit::bind_member<&AttBasedServer::OnReadByGroupType>(this));
    read_by_type_id_ = att_->RegisterHandler(
        att::kReadByTypeRequest,
        fit::bind_member<&AttBasedServer::OnReadByType>(this));
    read_req_id_ = att_->RegisterHandler(
        att::kReadRequest,
        fit::bind_member<&AttBasedServer::OnReadRequest>(this));
    write_req_id_ = att_->RegisterHandler(
        att::kWriteRequest,
        fit::bind_member<&AttBasedServer::OnWriteRequest>(this));
    write_cmd_id_ = att_->RegisterHandler(
        att::kWriteCommand,
        fit::bind_member<&AttBasedServer::OnWriteCommand>(this));
    read_blob_req_id_ = att_->RegisterHandler(
        att::kReadBlobRequest,
        fit::bind_member<&AttBasedServer::OnReadBlobRequest>(this));
    find_by_type_value_id_ = att_->RegisterHandler(
        att::kFindByTypeValueRequest,
        fit::bind_member<&AttBasedServer::OnFindByTypeValueRequest>(this));
    prepare_write_id_ = att_->RegisterHandler(
        att::kPrepareWriteRequest,
        fit::bind_member<&AttBasedServer::OnPrepareWriteRequest>(this));
    exec_write_id_ = att_->RegisterHandler(
        att::kExecuteWriteRequest,
        fit::bind_member<&AttBasedServer::OnExecuteWriteRequest>(this));
  }

  ~AttBasedServer() override {
    att_->UnregisterHandler(exec_write_id_);
    att_->UnregisterHandler(prepare_write_id_);
    att_->UnregisterHandler(find_by_type_value_id_);
    att_->UnregisterHandler(read_blob_req_id_);
    att_->UnregisterHandler(write_cmd_id_);
    att_->UnregisterHandler(write_req_id_);
    att_->UnregisterHandler(read_req_id_);
    att_->UnregisterHandler(read_by_type_id_);
    att_->UnregisterHandler(read_by_group_type_id_);
    att_->UnregisterHandler(find_information_id_);
    att_->UnregisterHandler(exchange_mtu_id_);
  }

 private:
  // Convenience "alias"
  inline att::Database::WeakPtr db() { return local_services_->database(); }

  // Server overrides:
  void SendUpdate(IdType service_id,
                  IdType chrc_id,
                  BufferView value,
                  IndicationCallback indicate_callback) override {
    constexpr size_t kHeaderSize = sizeof(att::Header) + sizeof(att::Handle);
    const size_t max_payload_size =
        std::max<size_t>(att_->mtu(), kHeaderSize) - kHeaderSize;
    if (value.size() > max_payload_size) {
      value = value.view(0, max_payload_size);
    }
    auto buffer = NewBuffer(kHeaderSize + value.size());
    PW_CHECK(buffer);

    LocalServiceManager::ClientCharacteristicConfig config;
    if (!local_services_->GetCharacteristicConfig(
            service_id, chrc_id, peer_id_, &config)) {
      bt_log(TRACE,
             "gatt",
             "peer has not configured characteristic: %s",
             bt_str(peer_id_));
      if (indicate_callback) {
        indicate_callback(ToResult(HostError::kNotSupported));
      }
      return;
    }

    // Make sure that the client has subscribed to the requested protocol
    // method.
    if ((indicate_callback && !config.indicate) ||
        (!indicate_callback && !config.notify)) {
      bt_log(TRACE,
             "gatt",
             "peer has not enabled (%s): %s",
             (indicate_callback ? "indications" : "notifications"),
             bt_str(peer_id_));
      if (indicate_callback) {
        indicate_callback(ToResult(HostError::kNotSupported));
      }
      return;
    }

    att::PacketWriter writer(
        indicate_callback ? att::kIndication : att::kNotification,
        buffer.get());
    auto rsp_params = writer.mutable_payload<att::AttributeData>();
    rsp_params->handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, config.handle);
    writer.mutable_payload_data().Write(value, sizeof(att::AttributeData));

    if (!indicate_callback) {
      [[maybe_unused]] bool _ = att_->SendWithoutResponse(std::move(buffer));
      return;
    }
    auto transaction_cb = [indicate_cb = std::move(indicate_callback)](
                              att::Bearer::TransactionResult result) mutable {
      if (result.is_ok()) {
        bt_log(DEBUG, "gatt", "got indication ACK");
        indicate_cb(fit::ok());
      } else {
        const auto& [error, handle] = result.error_value();
        bt_log(WARN,
               "gatt",
               "indication failed (error %s, handle: %#.4x)",
               bt_str(error),
               handle);
        indicate_cb(fit::error(error));
      }
    };
    att_->StartTransaction(std::move(buffer), std::move(transaction_cb));
  }

  void ShutDown() override { att_->ShutDown(); }

  // ATT protocol request handlers:
  void OnExchangeMTU(att::Bearer::TransactionId tid,
                     const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kExchangeMTURequest);

    if (packet.payload_size() != sizeof(att::ExchangeMTURequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::ExchangeMTURequestParams>();
    uint16_t client_mtu = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                      params.client_rx_mtu);
    uint16_t server_mtu = att_->preferred_mtu();

    auto buffer =
        NewBuffer(sizeof(att::Header) + sizeof(att::ExchangeMTUResponseParams));
    PW_CHECK(buffer);

    att::PacketWriter writer(att::kExchangeMTUResponse, buffer.get());
    auto rsp_params = writer.mutable_payload<att::ExchangeMTUResponseParams>();
    rsp_params->server_rx_mtu =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, server_mtu);

    att_->Reply(tid, std::move(buffer));

    // If the minimum value is less than the default MTU, then go with the
    // default MTU (Vol 3, Part F, 3.4.2.2).
    // TODO(armansito): This needs to use on kBREDRMinATTMTU for BR/EDR. Make
    // the default MTU configurable.
    att_->set_mtu(std::max(att::kLEMinMTU, std::min(client_mtu, server_mtu)));
  }

  void OnFindInformation(att::Bearer::TransactionId tid,
                         const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kFindInformationRequest);
    TRACE_DURATION("bluetooth", "gatt::Server::OnFindInformation");

    if (packet.payload_size() != sizeof(att::FindInformationRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::FindInformationRequestParams>();
    att::Handle start =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.start_handle);
    att::Handle end =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);

    constexpr size_t kRspStructSize =
        sizeof(att::FindInformationResponseParams);
    constexpr size_t kHeaderSize = sizeof(att::Header) + kRspStructSize;
    PW_DCHECK(kHeaderSize <= att_->mtu());

    if (start == att::kInvalidHandle || start > end) {
      att_->ReplyWithError(tid, start, att::ErrorCode::kInvalidHandle);
      return;
    }

    // Find all attributes within range with the same compact UUID size that can
    // fit within the current MTU.
    size_t max_payload_size = att_->mtu() - kHeaderSize;
    size_t uuid_size;
    size_t entry_size;
    std::list<const att::Attribute*> results;
    for (auto it = db()->GetIterator(start, end); !it.AtEnd(); it.Advance()) {
      const auto* attr = it.get();
      PW_DCHECK(attr);

      // GATT does not allow 32-bit UUIDs
      size_t compact_size = attr->type().CompactSize(/*allow_32bit=*/false);
      if (results.empty()) {
        // |uuid_size| is determined by the first attribute.
        uuid_size = compact_size;
        entry_size =
            std::min(uuid_size + sizeof(att::Handle), max_payload_size);
      } else if (compact_size != uuid_size || entry_size > max_payload_size) {
        break;
      }

      results.push_back(attr);
      max_payload_size -= entry_size;
    }

    if (results.empty()) {
      att_->ReplyWithError(tid, start, att::ErrorCode::kAttributeNotFound);
      return;
    }

    PW_DCHECK(!results.empty());

    size_t pdu_size = kHeaderSize + entry_size * results.size();

    auto buffer = NewBuffer(pdu_size);
    PW_CHECK(buffer);

    att::PacketWriter writer(att::kFindInformationResponse, buffer.get());
    auto rsp_params =
        writer.mutable_payload<att::FindInformationResponseParams>();
    rsp_params->format =
        (entry_size == 4) ? att::UUIDType::k16Bit : att::UUIDType::k128Bit;

    // |out_entries| initially references |params->information_data|. The loop
    // below modifies it as entries are written into the list.
    auto out_entries =
        writer.mutable_payload_data().mutable_view(kRspStructSize);
    for (const auto& attr : results) {
      *reinterpret_cast<att::Handle*>(out_entries.mutable_data()) =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, attr->handle());
      auto uuid_view = out_entries.mutable_view(sizeof(att::Handle));
      attr->type().ToBytes(&uuid_view, /*allow_32bit=*/false);

      // advance
      out_entries = out_entries.mutable_view(entry_size);
    }

    att_->Reply(tid, std::move(buffer));
  }

  void OnFindByTypeValueRequest(att::Bearer::TransactionId tid,
                                const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kFindByTypeValueRequest);

    if (packet.payload_size() < sizeof(att::FindByTypeValueRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::FindByTypeValueRequestParams>();
    att::Handle start =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.start_handle);
    att::Handle end =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);
    UUID type(params.type);
    constexpr size_t kParamsSize = sizeof(att::FindByTypeValueRequestParams);

    BufferView value = packet.payload_data().view(
        kParamsSize, packet.payload_size() - kParamsSize);

    if (start == att::kInvalidHandle || start > end) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidHandle);
      return;
    }

    auto iter = db()->GetIterator(start, end, &type, /*groups_only=*/false);
    if (iter.AtEnd()) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kAttributeNotFound);
      return;
    }

    std::list<const att::Attribute*> results;

    // Filter for identical values
    for (; !iter.AtEnd(); iter.Advance()) {
      const auto* attr = iter.get();
      PW_DCHECK(attr);

      // Only support static values for this Request type
      if (attr->value()) {
        if (*attr->value() == value) {
          results.push_back(attr);
        }
      }
    }

    // No attributes match the value
    if (results.size() == 0) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kAttributeNotFound);
      return;
    }

    constexpr size_t kRspStructSize = sizeof(att::HandlesInformationList);
    size_t pdu_size = sizeof(att::Header) + kRspStructSize * results.size();
    auto buffer = NewBuffer(pdu_size);
    PW_CHECK(buffer);

    att::PacketWriter writer(att::kFindByTypeValueResponse, buffer.get());

    // Points to the next entry in the target PDU.
    auto next_entry = writer.mutable_payload_data();
    for (const auto& attr : results) {
      auto* entry = reinterpret_cast<att::HandlesInformationList*>(
          next_entry.mutable_data());
      entry->handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, attr->handle());
      if (attr->group().active()) {
        entry->group_end_handle = pw::bytes::ConvertOrderTo(
            cpp20::endian::little, attr->group().end_handle());
      } else {
        entry->group_end_handle =
            pw::bytes::ConvertOrderTo(cpp20::endian::little, attr->handle());
      }
      next_entry = next_entry.mutable_view(kRspStructSize);
    }

    att_->Reply(tid, std::move(buffer));
  }

  void OnReadByGroupType(att::Bearer::TransactionId tid,
                         const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kReadByGroupTypeRequest);
    TRACE_DURATION("bluetooth", "gatt::Server::OnReadByGroupType");

    att::Handle start, end;
    UUID group_type;

    // The group type is represented as either a 16-bit or 128-bit UUID.
    if (packet.payload_size() == sizeof(att::ReadByTypeRequestParams16)) {
      const auto& params = packet.payload<att::ReadByTypeRequestParams16>();
      start = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          params.start_handle);
      end =
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);
      group_type = UUID(static_cast<uint16_t>(
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.type)));
    } else if (packet.payload_size() ==
               sizeof(att::ReadByTypeRequestParams128)) {
      const auto& params = packet.payload<att::ReadByTypeRequestParams128>();
      start = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          params.start_handle);
      end =
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);
      group_type = UUID(params.type);
    } else {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    if (group_type != types::kPrimaryService &&
        group_type != types::kSecondaryService) {
      att_->ReplyWithError(tid, start, att::ErrorCode::kUnsupportedGroupType);
      return;
    }

    constexpr size_t kRspStructSize =
        sizeof(att::ReadByGroupTypeResponseParams);
    constexpr size_t kHeaderSize = sizeof(att::Header) + kRspStructSize;
    PW_DCHECK(kHeaderSize <= att_->mtu());

    size_t value_size;
    std::list<const att::Attribute*> results;
    fit::result<att::ErrorCode> status =
        ReadByTypeHelper(start,
                         end,
                         group_type,
                         /*group_type=*/true,
                         att_->mtu() - kHeaderSize,
                         att::kMaxReadByGroupTypeValueLength,
                         sizeof(att::AttributeGroupDataEntry),
                         &value_size,
                         &results);
    if (status.is_error()) {
      att_->ReplyWithError(tid, start, status.error_value());
      return;
    }

    PW_DCHECK(!results.empty());

    size_t entry_size = value_size + sizeof(att::AttributeGroupDataEntry);
    size_t pdu_size = kHeaderSize + entry_size * results.size();
    PW_DCHECK(pdu_size <= att_->mtu());

    auto buffer = NewBuffer(pdu_size);
    PW_CHECK(buffer);

    att::PacketWriter writer(att::kReadByGroupTypeResponse, buffer.get());
    auto params = writer.mutable_payload<att::ReadByGroupTypeResponseParams>();

    PW_DCHECK(entry_size <= std::numeric_limits<uint8_t>::max());
    params->length = static_cast<uint8_t>(entry_size);

    // Points to the next entry in the target PDU.
    auto next_entry =
        writer.mutable_payload_data().mutable_view(kRspStructSize);
    for (const auto& attr : results) {
      auto* entry = reinterpret_cast<att::AttributeGroupDataEntry*>(
          next_entry.mutable_data());
      entry->start_handle = pw::bytes::ConvertOrderTo(
          cpp20::endian::little, attr->group().start_handle());
      entry->group_end_handle = pw::bytes::ConvertOrderTo(
          cpp20::endian::little, attr->group().end_handle());
      next_entry.Write(attr->group().decl_value().view(0, value_size),
                       sizeof(att::AttributeGroupDataEntry));

      next_entry = next_entry.mutable_view(entry_size);
    }

    att_->Reply(tid, std::move(buffer));
  }

  void OnReadByType(att::Bearer::TransactionId tid,
                    const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kReadByTypeRequest);
    TRACE_DURATION("bluetooth", "gatt::Server::OnReadByType");

    att::Handle start, end;
    UUID type;

    // The attribute type is represented as either a 16-bit or 128-bit UUID.
    if (packet.payload_size() == sizeof(att::ReadByTypeRequestParams16)) {
      const auto& params = packet.payload<att::ReadByTypeRequestParams16>();
      start = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          params.start_handle);
      end =
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);
      type = UUID(static_cast<uint16_t>(
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.type)));
    } else if (packet.payload_size() ==
               sizeof(att::ReadByTypeRequestParams128)) {
      const auto& params = packet.payload<att::ReadByTypeRequestParams128>();
      start = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          params.start_handle);
      end =
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.end_handle);
      type = UUID(params.type);
    } else {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    constexpr size_t kRspStructSize = sizeof(att::ReadByTypeResponseParams);
    constexpr size_t kHeaderSize = sizeof(att::Header) + kRspStructSize;
    PW_DCHECK(kHeaderSize <= att_->mtu());

    size_t out_value_size;
    std::list<const att::Attribute*> results;
    fit::result<att::ErrorCode> status =
        ReadByTypeHelper(start,
                         end,
                         type,
                         /*group_type=*/false,
                         att_->mtu() - kHeaderSize,
                         att::kMaxReadByTypeValueLength,
                         sizeof(att::AttributeData),
                         &out_value_size,
                         &results);
    if (status.is_error()) {
      att_->ReplyWithError(tid, start, status.error_value());
      return;
    }

    PW_DCHECK(!results.empty());

    // If the value is dynamic, then delegate the read to any registered
    // handler.
    if (!results.front()->value()) {
      PW_DCHECK(results.size() == 1u);

      const size_t kMaxValueSize =
          std::min(att_->mtu() - kHeaderSize - sizeof(att::AttributeData),
                   static_cast<size_t>(att::kMaxReadByTypeValueLength));

      att::Handle handle = results.front()->handle();
      auto self = weak_self_.GetWeakPtr();
      auto result_cb = [self, tid, handle, kMaxValueSize](
                           fit::result<att::ErrorCode> read_result,
                           const auto& value) {
        if (!self.is_alive())
          return;

        if (read_result.is_error()) {
          self->att_->ReplyWithError(tid, handle, read_result.error_value());
          return;
        }

        // Respond with just a single entry.
        size_t value_size = std::min(value.size(), kMaxValueSize);
        size_t entry_size = value_size + sizeof(att::AttributeData);
        auto buffer = NewBuffer(entry_size + kHeaderSize);
        att::PacketWriter writer(att::kReadByTypeResponse, buffer.get());

        auto params = writer.mutable_payload<att::ReadByTypeResponseParams>();
        params->length = static_cast<uint8_t>(entry_size);
        params->attribute_data_list->handle =
            pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);
        writer.mutable_payload_data().Write(
            value.data(), value_size, sizeof(params->length) + sizeof(handle));

        self->att_->Reply(tid, std::move(buffer));
      };

      // Respond with an error if no read handler was registered.
      if (!results.front()->ReadAsync(peer_id_, 0, result_cb)) {
        att_->ReplyWithError(tid, handle, att::ErrorCode::kReadNotPermitted);
      }
      return;
    }

    size_t entry_size = sizeof(att::AttributeData) + out_value_size;
    PW_DCHECK(entry_size <= std::numeric_limits<uint8_t>::max());

    size_t pdu_size = kHeaderSize + entry_size * results.size();
    PW_DCHECK(pdu_size <= att_->mtu());

    auto buffer = NewBuffer(pdu_size);
    PW_CHECK(buffer);

    att::PacketWriter writer(att::kReadByTypeResponse, buffer.get());
    auto params = writer.mutable_payload<att::ReadByTypeResponseParams>();
    params->length = static_cast<uint8_t>(entry_size);

    // Points to the next entry in the target PDU.
    auto next_entry =
        writer.mutable_payload_data().mutable_view(kRspStructSize);
    for (const auto& attr : results) {
      auto* entry =
          reinterpret_cast<att::AttributeData*>(next_entry.mutable_data());
      entry->handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, attr->handle());
      next_entry.Write(attr->value()->view(0, out_value_size),
                       sizeof(entry->handle));

      next_entry = next_entry.mutable_view(entry_size);
    }

    att_->Reply(tid, std::move(buffer));
  }

  void OnReadBlobRequest(att::Bearer::TransactionId tid,
                         const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kReadBlobRequest);

    if (packet.payload_size() != sizeof(att::ReadBlobRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::ReadBlobRequestParams>();
    att::Handle handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);
    uint16_t offset =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.offset);

    const auto* attr = db()->FindAttribute(handle);
    if (!attr) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kInvalidHandle);
      return;
    }

    fit::result<att::ErrorCode> permissions_result =
        att::CheckReadPermissions(attr->read_reqs(), att_->security());
    if (permissions_result.is_error()) {
      att_->ReplyWithError(tid, handle, permissions_result.error_value());
      return;
    }

    constexpr size_t kHeaderSize = sizeof(att::Header);

    auto self = weak_self_.GetWeakPtr();
    auto callback = [self, tid, handle](fit::result<att::ErrorCode> status,
                                        const auto& value) {
      if (!self.is_alive())
        return;

      if (status.is_error()) {
        self->att_->ReplyWithError(tid, handle, status.error_value());
        return;
      }

      size_t value_size =
          std::min(value.size(), self->att_->mtu() - kHeaderSize);
      auto buffer = NewBuffer(value_size + kHeaderSize);
      PW_CHECK(buffer);

      att::PacketWriter writer(att::kReadBlobResponse, buffer.get());
      writer.mutable_payload_data().Write(value.view(0, value_size));

      self->att_->Reply(tid, std::move(buffer));
    };

    // Use the cached value if there is one.
    if (attr->value()) {
      if (offset >= attr->value()->size()) {
        att_->ReplyWithError(tid, handle, att::ErrorCode::kInvalidOffset);
        return;
      }
      size_t value_size =
          std::min(attr->value()->size(), self->att_->mtu() - kHeaderSize);
      callback(fit::ok(), attr->value()->view(offset, value_size));
      return;
    }

    // TODO(fxbug.dev/42142121): Add a timeout to this
    if (!attr->ReadAsync(peer_id_, offset, callback)) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kReadNotPermitted);
    }
  }

  void OnReadRequest(att::Bearer::TransactionId tid,
                     const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kReadRequest);

    if (packet.payload_size() != sizeof(att::ReadRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::WriteRequestParams>();
    att::Handle handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);

    const auto* attr = db()->FindAttribute(handle);
    if (!attr) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kInvalidHandle);
      return;
    }

    fit::result<att::ErrorCode> permissions_result =
        att::CheckReadPermissions(attr->read_reqs(), att_->security());
    if (permissions_result.is_error()) {
      att_->ReplyWithError(tid, handle, permissions_result.error_value());
      return;
    }

    constexpr size_t kHeaderSize = sizeof(att::Header);

    auto self = weak_self_.GetWeakPtr();
    auto callback = [self, tid, handle](fit::result<att::ErrorCode> status,
                                        const auto& value) {
      if (!self.is_alive())
        return;

      if (status.is_error()) {
        self->att_->ReplyWithError(tid, handle, status.error_value());
        return;
      }

      size_t value_size =
          std::min(value.size(), self->att_->mtu() - kHeaderSize);
      auto buffer = NewBuffer(value_size + kHeaderSize);
      PW_CHECK(buffer);

      att::PacketWriter writer(att::kReadResponse, buffer.get());
      writer.mutable_payload_data().Write(value.view(0, value_size));

      self->att_->Reply(tid, std::move(buffer));
    };

    // Use the cached value if there is one.
    if (attr->value()) {
      callback(fit::ok(), *attr->value());
      return;
    }

    if (!attr->ReadAsync(peer_id_, 0, callback)) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kReadNotPermitted);
    }
  }

  void OnWriteCommand(att::Bearer::TransactionId,
                      const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kWriteCommand);

    if (packet.payload_size() < sizeof(att::WriteRequestParams)) {
      // Ignore if wrong size, no response allowed
      return;
    }

    const auto& params = packet.payload<att::WriteRequestParams>();
    att::Handle handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);
    const auto* attr = db()->FindAttribute(handle);

    // Attributes can be invalid if the handle is invalid
    if (!attr) {
      return;
    }

    fit::result<att::ErrorCode> status =
        att::CheckWritePermissions(attr->write_reqs(), att_->security());
    if (status.is_error()) {
      return;
    }

    // Attributes with a static value cannot be written.
    if (attr->value()) {
      return;
    }

    auto value_view = packet.payload_data().view(sizeof(params.handle));
    if (value_view.size() > att::kMaxAttributeValueLength) {
      return;
    }

    // No response allowed for commands, ignore the cb
    attr->WriteAsync(peer_id_, 0, value_view, nullptr);
  }

  void OnWriteRequest(att::Bearer::TransactionId tid,
                      const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kWriteRequest);

    if (packet.payload_size() < sizeof(att::WriteRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::WriteRequestParams>();
    att::Handle handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);

    const auto* attr = db()->FindAttribute(handle);
    if (!attr) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kInvalidHandle);
      return;
    }

    fit::result<att::ErrorCode> permissions_result =
        att::CheckWritePermissions(attr->write_reqs(), att_->security());
    if (permissions_result.is_error()) {
      att_->ReplyWithError(tid, handle, permissions_result.error_value());
      return;
    }

    // Attributes with a static value cannot be written.
    if (attr->value()) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kWriteNotPermitted);
      return;
    }

    auto value_view = packet.payload_data().view(sizeof(params.handle));
    if (value_view.size() > att::kMaxAttributeValueLength) {
      att_->ReplyWithError(
          tid, handle, att::ErrorCode::kInvalidAttributeValueLength);
      return;
    }

    auto self = weak_self_.GetWeakPtr();
    auto result_cb = [self, tid, handle](fit::result<att::ErrorCode> status) {
      if (!self.is_alive())
        return;

      if (status.is_error()) {
        self->att_->ReplyWithError(tid, handle, status.error_value());
        return;
      }

      auto buffer = NewBuffer(1);
      (*buffer)[0] = att::kWriteResponse;
      self->att_->Reply(tid, std::move(buffer));
    };

    if (!attr->WriteAsync(peer_id_, 0, value_view, result_cb)) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kWriteNotPermitted);
    }
  }

  void OnPrepareWriteRequest(att::Bearer::TransactionId tid,
                             const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kPrepareWriteRequest);

    if (packet.payload_size() < sizeof(att::PrepareWriteRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::PrepareWriteRequestParams>();
    att::Handle handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);
    uint16_t offset =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.offset);
    auto value_view = packet.payload_data().view(sizeof(params.handle) +
                                                 sizeof(params.offset));

    if (prepare_queue_.size() >= att::kPrepareQueueMaxCapacity) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kPrepareQueueFull);
      return;
    }

    // Validate attribute handle and perform security checks (see Vol 3, Part F,
    // 3.4.6.1 for required checks)
    const auto* attr = db()->FindAttribute(handle);
    if (!attr) {
      att_->ReplyWithError(tid, handle, att::ErrorCode::kInvalidHandle);
      return;
    }

    fit::result<att::ErrorCode> status =
        att::CheckWritePermissions(attr->write_reqs(), att_->security());
    if (status.is_error()) {
      att_->ReplyWithError(tid, handle, status.error_value());
      return;
    }

    prepare_queue_.push(att::QueuedWrite(handle, offset, value_view));

    // Reply back with the request payload.
    auto buffer = std::make_unique<DynamicByteBuffer>(packet.size());
    att::PacketWriter writer(att::kPrepareWriteResponse, buffer.get());
    writer.mutable_payload_data().Write(packet.payload_data());

    att_->Reply(tid, std::move(buffer));
  }

  void OnExecuteWriteRequest(att::Bearer::TransactionId tid,
                             const att::PacketReader& packet) {
    PW_DCHECK(packet.opcode() == att::kExecuteWriteRequest);

    if (packet.payload_size() != sizeof(att::ExecuteWriteRequestParams)) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    const auto& params = packet.payload<att::ExecuteWriteRequestParams>();
    if (params.flags == att::ExecuteWriteFlag::kCancelAll) {
      prepare_queue_ = {};

      auto buffer = std::make_unique<DynamicByteBuffer>(1);
      att::PacketWriter writer(att::kExecuteWriteResponse, buffer.get());
      att_->Reply(tid, std::move(buffer));
      return;
    }

    if (params.flags != att::ExecuteWriteFlag::kWritePending) {
      att_->ReplyWithError(
          tid, att::kInvalidHandle, att::ErrorCode::kInvalidPDU);
      return;
    }

    auto self = weak_self_.GetWeakPtr();
    auto result_cb = [self,
                      tid](fit::result<std::tuple<att::Handle, att::ErrorCode>>
                               result) mutable {
      if (!self.is_alive())
        return;

      if (result.is_error()) {
        auto [handle, ecode] = result.error_value();
        self->att_->ReplyWithError(tid, handle, ecode);
        return;
      }

      auto rsp = std::make_unique<DynamicByteBuffer>(1);
      att::PacketWriter writer(att::kExecuteWriteResponse, rsp.get());
      self->att_->Reply(tid, std::move(rsp));
    };
    db()->ExecuteWriteQueue(peer_id_,
                            std::move(prepare_queue_),
                            att_->security(),
                            std::move(result_cb));
  }

  // Helper function to serve the Read By Type and Read By Group Type requests.
  // This searches |db| for attributes with the given |type| and adds as many
  // attributes as it can fit within the given |max_data_list_size|. The
  // attribute value that should be included within each attribute data list
  // entry is returned in |out_value_size|.
  //
  // If the result is a dynamic attribute, |out_results| will contain at most
  // one entry. |out_value_size| will point to an undefined value in that case.
  //
  // On error, returns an error code that can be used in a ATT Error Response.
  fit::result<att::ErrorCode> ReadByTypeHelper(
      att::Handle start,
      att::Handle end,
      const UUID& type,
      bool group_type,
      size_t max_data_list_size,
      size_t max_value_size,
      size_t entry_prefix_size,
      size_t* out_value_size,
      std::list<const att::Attribute*>* out_results) {
    PW_DCHECK(out_results);
    PW_DCHECK(out_value_size);

    if (start == att::kInvalidHandle || start > end)
      return fit::error(att::ErrorCode::kInvalidHandle);

    auto iter = db()->GetIterator(start, end, &type, group_type);
    if (iter.AtEnd())
      return fit::error(att::ErrorCode::kAttributeNotFound);

    // |value_size| is the size of the complete attribute value for each result
    // entry. |entry_size| = |value_size| + |entry_prefix_size|. We store these
    // separately to avoid recalculating one every it gets checked.
    size_t value_size;
    size_t entry_size;
    std::list<const att::Attribute*> results;

    for (; !iter.AtEnd(); iter.Advance()) {
      const auto* attr = iter.get();
      PW_DCHECK(attr);

      fit::result<att::ErrorCode> security_result =
          att::CheckReadPermissions(attr->read_reqs(), att_->security());
      if (security_result.is_error()) {
        // Return error only if this is the first result that matched. We simply
        // stop the search otherwise.
        if (results.empty()) {
          return security_result;
        }
        break;
      }

      // The first result determines |value_size| and |entry_size|.
      if (results.empty()) {
        if (!attr->value()) {
          // If the first value is dynamic then this is the only attribute that
          // this call will return. No need to calculate the value size.
          results.push_back(attr);
          break;
        }

        value_size = attr->value()->size();  // untruncated value size
        entry_size =
            std::min(std::min(value_size, max_value_size) + entry_prefix_size,
                     max_data_list_size);

        // Actual value size to include in a PDU.
        *out_value_size = entry_size - entry_prefix_size;

      } else if (!attr->value() || attr->value()->size() != value_size ||
                 entry_size > max_data_list_size) {
        // Stop the search and exclude this attribute because either:
        // a. we ran into a dynamic value in a result that contains static
        // values, b. the matching attribute has a different value size than the
        // first
        //    attribute,
        // c. there is no remaining space in the response PDU.
        break;
      }

      results.push_back(attr);
      max_data_list_size -= entry_size;
    }

    if (results.empty())
      return fit::error(att::ErrorCode::kAttributeNotFound);

    *out_results = std::move(results);
    return fit::ok();
  }

  PeerId peer_id_;
  LocalServiceManager::WeakPtr local_services_;
  att::Bearer::WeakPtr att_;

  // The queue data structure used for queued writes (see Vol 3, Part F, 3.4.6).
  att::PrepareWriteQueue prepare_queue_;

  // ATT protocol request handler IDs
  // TODO(armansito): Storing all these IDs here feels wasteful. Provide a way
  // to unregister GATT server callbacks collectively from an att::Bearer, given
  // that it's server-role functionalities are uniquely handled by this class.
  att::Bearer::HandlerId exchange_mtu_id_;
  att::Bearer::HandlerId find_information_id_;
  att::Bearer::HandlerId read_by_group_type_id_;
  att::Bearer::HandlerId read_by_type_id_;
  att::Bearer::HandlerId read_req_id_;
  att::Bearer::HandlerId write_req_id_;
  att::Bearer::HandlerId write_cmd_id_;
  att::Bearer::HandlerId read_blob_req_id_;
  att::Bearer::HandlerId find_by_type_value_id_;
  att::Bearer::HandlerId prepare_write_id_;
  att::Bearer::HandlerId exec_write_id_;

  WeakSelf<AttBasedServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AttBasedServer);
};

// static
std::unique_ptr<Server> Server::Create(
    PeerId peer_id,
    LocalServiceManager::WeakPtr local_services,
    att::Bearer::WeakPtr bearer) {
  return std::make_unique<AttBasedServer>(
      peer_id, std::move(local_services), std::move(bearer));
}
}  // namespace bt::gatt
