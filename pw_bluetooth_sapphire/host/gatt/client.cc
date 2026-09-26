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

#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/slab_allocator.h"
#include "pw_bluetooth_sapphire/internal/host/common/trace.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

using bt::HostError;

namespace bt::gatt {
namespace {

MutableByteBufferPtr NewPDU(size_t param_size) {
  auto pdu = NewBuffer(sizeof(att::Header) + param_size);
  if (!pdu) {
    bt_log(DEBUG, "att", "out of memory");
  }
  return pdu;
}

template <att::UUIDType Format,
          typename EntryType = att::InformationData<Format>>
bool ProcessDescriptorDiscoveryResponse(
    att::Handle range_start,
    att::Handle range_end,
    BufferView entries,
    Client::DescriptorCallback desc_callback,
    att::Handle* out_last_handle) {
  PW_DCHECK(out_last_handle);

  if (entries.size() % sizeof(EntryType)) {
    bt_log(DEBUG, "gatt", "malformed information data list");
    return false;
  }

  att::Handle last_handle = range_end;
  while (entries.size()) {
    const EntryType& entry = entries.To<EntryType>();

    att::Handle desc_handle =
        pw::bytes::ConvertOrderFrom(cpp20::endian::little, entry.handle);

    // Stop and report an error if the server erroneously responds with
    // an attribute outside the requested range.
    if (desc_handle > range_end || desc_handle < range_start) {
      bt_log(DEBUG,
             "gatt",
             "descriptor handle out of range (handle: %#.4x, "
             "range: %#.4x - %#.4x)",
             desc_handle,
             range_start,
             range_end);
      return false;
    }

    // The handles must be strictly increasing.
    if (last_handle != range_end && desc_handle <= last_handle) {
      bt_log(DEBUG, "gatt", "descriptor handles not strictly increasing");
      return false;
    }

    last_handle = desc_handle;

    // Notify the handler.
    desc_callback(DescriptorData(desc_handle, UUID(entry.uuid)));

    entries = entries.view(sizeof(EntryType));
  }

  *out_last_handle = last_handle;
  return true;
}

}  // namespace

class Impl final : public Client {
 public:
  explicit Impl(att::Bearer::WeakPtr bearer)
      : att_(std::move(bearer)), weak_self_(this) {
    PW_DCHECK(att_.is_alive());

    auto handler = [this](auto txn_id, const att::PacketReader& pdu) {
      PW_DCHECK(pdu.opcode() == att::kNotification ||
                pdu.opcode() == att::kIndication);

      if (pdu.payload_size() < sizeof(att::NotificationParams)) {
        // Received a malformed notification. Disconnect the link.
        bt_log(DEBUG, "gatt", "malformed notification/indication PDU");
        att_->ShutDown();
        return;
      }

      bool is_ind = pdu.opcode() == att::kIndication;
      const auto& params = pdu.payload<att::NotificationParams>();
      att::Handle handle =
          pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.handle);
      size_t value_size = pdu.payload_size() - sizeof(att::Handle);

      // Auto-confirm indications.
      if (is_ind) {
        auto confirmation_pdu = NewPDU(0u);
        if (confirmation_pdu) {
          att::PacketWriter(att::kConfirmation, confirmation_pdu.get());
          att_->Reply(txn_id, std::move(confirmation_pdu));
        } else {
          att_->ReplyWithError(
              txn_id, handle, att::ErrorCode::kInsufficientResources);
        }
      }

      bool maybe_truncated = false;
      // If the value is the max size that fits in the MTU, it may be truncated.
      if (value_size ==
          att_->mtu() - sizeof(att::OpCode) - sizeof(att::Handle)) {
        maybe_truncated = true;
      }

      // Run the handler
      if (notification_handler_) {
        notification_handler_(is_ind,
                              handle,
                              BufferView(params.value, value_size),
                              maybe_truncated);
      } else {
        bt_log(
            TRACE, "gatt", "dropped notification/indication without handler");
      }
    };

    not_handler_id_ = att_->RegisterHandler(att::kNotification, handler);
    ind_handler_id_ = att_->RegisterHandler(att::kIndication, handler);
  }

  ~Impl() override {
    att_->UnregisterHandler(not_handler_id_);
    att_->UnregisterHandler(ind_handler_id_);
  }

  using WeakPtr = WeakSelf<Client>::WeakPtr;
  WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

 private:
  uint16_t mtu() const override { return att_->mtu(); }

  void ExchangeMTU(MTUCallback mtu_callback) override {
    auto pdu = NewPDU(sizeof(att::ExchangeMTURequestParams));
    if (!pdu) {
      mtu_callback(fit::error(att::Error(HostError::kOutOfMemory)));
      return;
    }

    att::PacketWriter writer(att::kExchangeMTURequest, pdu.get());
    auto params = writer.mutable_payload<att::ExchangeMTURequestParams>();
    params->client_rx_mtu =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, att_->preferred_mtu());

    auto rsp_cb = [this, mtu_cb = std::move(mtu_callback)](
                      att::Bearer::TransactionResult result) mutable {
      if (result.is_ok()) {
        const att::PacketReader& rsp = result.value();
        PW_DCHECK(rsp.opcode() == att::kExchangeMTUResponse);

        if (rsp.payload_size() != sizeof(att::ExchangeMTUResponseParams)) {
          // Received a malformed response. Disconnect the link.
          att_->ShutDown();

          mtu_cb(fit::error(att::Error(HostError::kPacketMalformed)));
          return;
        }

        const auto& rsp_params = rsp.payload<att::ExchangeMTUResponseParams>();
        uint16_t server_mtu = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little, rsp_params.server_rx_mtu);

        // If the minimum value is less than the default MTU, then go with the
        // default MTU (Vol 3, Part F, 3.4.2.2).
        uint16_t final_mtu = std::max(
            att::kLEMinMTU, std::min(server_mtu, att_->preferred_mtu()));
        att_->set_mtu(final_mtu);

        mtu_cb(fit::ok(final_mtu));
        return;
      }
      const auto& [error, handle] = result.error_value();

      // "If the Error Response is sent by the server with the Error Code
      // set to Request Not Supported, [...] the default MTU shall be used
      // (Vol 3, Part G, 4.3.1)"
      if (error.is(att::ErrorCode::kRequestNotSupported)) {
        bt_log(
            DEBUG, "gatt", "peer does not support MTU exchange: using default");
        att_->set_mtu(att::kLEMinMTU);
        mtu_cb(fit::error(error));
        return;
      }

      bt_log(DEBUG, "gatt", "MTU exchange failed: %s", bt_str(error));
      mtu_cb(fit::error(error));
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void DiscoverServices(ServiceKind kind,
                        ServiceCallback svc_callback,
                        att::ResultFunction<> status_callback) override {
    DiscoverServicesInRange(kind,
                            att::kHandleMin,
                            att::kHandleMax,
                            std::move(svc_callback),
                            std::move(status_callback));
  }

  void DiscoverServicesInRange(ServiceKind kind,
                               att::Handle range_start,
                               att::Handle range_end,
                               ServiceCallback svc_callback,
                               att::ResultFunction<> status_callback) override {
    PW_CHECK(range_start <= range_end);

    auto pdu = NewPDU(sizeof(att::ReadByGroupTypeRequestParams16));
    if (!pdu) {
      status_callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kReadByGroupTypeRequest, pdu.get());
    auto* params =
        writer.mutable_payload<att::ReadByGroupTypeRequestParams16>();
    params->start_handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, range_start);
    params->end_handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, range_end);
    params->type = pw::bytes::ConvertOrderTo(cpp20::endian::little,
                                             kind == ServiceKind::PRIMARY
                                                 ? types::kPrimaryService16
                                                 : types::kSecondaryService16);

    auto rsp_cb = [this,
                   kind,
                   range_start,
                   range_end,
                   svc_cb = std::move(svc_callback),
                   res_cb = std::move(status_callback)](
                      att::Bearer::TransactionResult result) mutable {
      if (result.is_error()) {
        const att::Error& error = result.error_value().first;

        // An Error Response code of "Attribute Not Found" indicates the end of
        // the procedure (v5.0, Vol 3, Part G, 4.4.1).
        if (error.is(att::ErrorCode::kAttributeNotFound)) {
          res_cb(fit::ok());
          return;
        }

        res_cb(fit::error(error));
        return;
      }

      const att::PacketReader& rsp = result.value();
      PW_DCHECK(rsp.opcode() == att::kReadByGroupTypeResponse);
      TRACE_DURATION("bluetooth",
                     "gatt::Client::DiscoverServicesInRange rsp_cb",
                     "size",
                     rsp.size());

      if (rsp.payload_size() < sizeof(att::ReadByGroupTypeResponseParams)) {
        // Received malformed response. Disconnect the link.
        bt_log(DEBUG, "gatt", "received malformed Read By Group Type response");
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      const auto& rsp_params =
          rsp.payload<att::ReadByGroupTypeResponseParams>();
      uint8_t entry_length = rsp_params.length;

      // We expect the returned attribute value to be a 16-bit or 128-bit
      // service UUID.
      constexpr size_t kAttrDataSize16 =
          sizeof(att::AttributeGroupDataEntry) + sizeof(att::AttributeType16);
      constexpr size_t kAttrDataSize128 =
          sizeof(att::AttributeGroupDataEntry) + sizeof(att::AttributeType128);

      if (entry_length != kAttrDataSize16 && entry_length != kAttrDataSize128) {
        bt_log(DEBUG, "gatt", "invalid attribute data length");
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      BufferView attr_data_list(rsp_params.attribute_data_list,
                                rsp.payload_size() - 1);
      if (attr_data_list.size() % entry_length) {
        bt_log(DEBUG, "gatt", "malformed attribute data list");
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      std::optional<att::Handle> last_handle;
      while (attr_data_list.size()) {
        att::Handle start = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little,
            attr_data_list
                .ReadMember<&att::AttributeGroupDataEntry::start_handle>());
        att::Handle end = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little,
            attr_data_list
                .ReadMember<&att::AttributeGroupDataEntry::group_end_handle>());

        if (end < start) {
          bt_log(DEBUG, "gatt", "received malformed service range values");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        if (start < range_start || start > range_end) {
          bt_log(DEBUG,
                 "gatt",
                 "received service range values outside of requested range");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        // "The Attribute Data List is ordered sequentially based on the
        // attribute handles." (Core Spec v5.3, Vol 3, Part F, Sec 3.4.4.10)
        if (last_handle.has_value() && start <= last_handle.value()) {
          bt_log(DEBUG, "gatt", "received services out of order");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        // This must succeed as we have performed the appropriate checks above.
        auto uuid_bytes =
            attr_data_list.view(offsetof(att::AttributeGroupDataEntry, value),
                                entry_length - (2 * sizeof(att::Handle)));
        UUID uuid(uuid_bytes);

        ServiceData service(kind, start, end, uuid);
        last_handle = service.range_end;

        // Notify the handler.
        svc_cb(service);

        attr_data_list = attr_data_list.view(entry_length);
      }

      // The procedure is over if we have reached the end of the handle range.
      if (!last_handle.has_value() || last_handle.value() >= range_end) {
        res_cb(fit::ok());
        return;
      }

      // Request the next batch.
      DiscoverServicesInRange(kind,
                              last_handle.value() + 1,
                              range_end,
                              std::move(svc_cb),
                              std::move(res_cb));
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void DiscoverServicesWithUuids(ServiceKind kind,
                                 ServiceCallback svc_cb,
                                 att::ResultFunction<> status_cb,
                                 std::vector<UUID> uuids) override {
    DiscoverServicesWithUuidsInRange(kind,
                                     att::kHandleMin,
                                     att::kHandleMax,
                                     std::move(svc_cb),
                                     std::move(status_cb),
                                     std::move(uuids));
  }

  void DiscoverServicesWithUuidsInRange(ServiceKind kind,
                                        att::Handle range_start,
                                        att::Handle range_end,
                                        ServiceCallback svc_callback,
                                        att::ResultFunction<> status_callback,
                                        std::vector<UUID> uuids) override {
    PW_CHECK(range_start <= range_end);
    PW_CHECK(!uuids.empty());
    UUID uuid = uuids.back();
    uuids.pop_back();

    auto recursive_status_cb = [this,
                                range_start,
                                range_end,
                                kind,
                                svc_cb = svc_callback.share(),
                                status_cb = std::move(status_callback),
                                remaining_uuids =
                                    std::move(uuids)](auto status) mutable {
      // Base case
      if (status.is_error() || remaining_uuids.empty()) {
        status_cb(status);
        return;
      }

      // Recursively discover with the remaining UUIDs.
      DiscoverServicesWithUuidsInRange(kind,
                                       range_start,
                                       range_end,
                                       std::move(svc_cb),
                                       std::move(status_cb),
                                       std::move(remaining_uuids));
    };

    // Discover the last uuid in uuids.
    DiscoverServicesByUuidInRange(kind,
                                  range_start,
                                  range_end,
                                  std::move(svc_callback),
                                  std::move(recursive_status_cb),
                                  uuid);
  }

  void DiscoverServicesByUuidInRange(ServiceKind kind,
                                     att::Handle start,
                                     att::Handle end,
                                     ServiceCallback svc_callback,
                                     att::ResultFunction<> status_callback,
                                     UUID uuid) {
    size_t uuid_size_bytes = uuid.CompactSize(/* allow 32 bit UUIDs */ false);
    auto pdu =
        NewPDU(sizeof(att::FindByTypeValueRequestParams) + uuid_size_bytes);
    if (!pdu) {
      status_callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kFindByTypeValueRequest, pdu.get());
    auto* params = writer.mutable_payload<att::FindByTypeValueRequestParams>();
    params->start_handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, start);
    params->end_handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, end);
    params->type = pw::bytes::ConvertOrderTo(cpp20::endian::little,
                                             kind == ServiceKind::PRIMARY
                                                 ? types::kPrimaryService16
                                                 : types::kSecondaryService16);
    MutableBufferView value_view(params->value, uuid_size_bytes);
    uuid.ToBytes(&value_view, /* allow 32 bit UUIDs */ false);

    auto rsp_cb = [this,
                   kind,
                   discovery_range_start = start,
                   discovery_range_end = end,
                   svc_cb = std::move(svc_callback),
                   res_cb = std::move(status_callback),
                   uuid](att::Bearer::TransactionResult result) mutable {
      if (result.is_error()) {
        const att::Error& error = result.error_value().first;

        // An Error Response code of "Attribute Not Found" indicates the end of
        // the procedure (v5.0, Vol 3, Part G, 4.4.2).
        if (error.is(att::ErrorCode::kAttributeNotFound)) {
          res_cb(fit::ok());
          return;
        }

        res_cb(fit::error(error));
        return;
      }

      const att::PacketReader& rsp = result.value();
      PW_DCHECK(rsp.opcode() == att::kFindByTypeValueResponse);

      size_t payload_size = rsp.payload_size();
      if (payload_size < 1 ||
          payload_size % sizeof(att::FindByTypeValueResponseParams) != 0) {
        // Received malformed response. Disconnect the link.
        bt_log(DEBUG,
               "gatt",
               "received malformed Find By Type Value response with size %zu",
               payload_size);
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      BufferView handle_list = rsp.payload_data();

      std::optional<att::Handle> last_handle;
      while (handle_list.size()) {
        const auto& entry = handle_list.To<att::HandlesInformationList>();

        att::Handle start_handle =
            pw::bytes::ConvertOrderFrom(cpp20::endian::little, entry.handle);
        att::Handle end_handle = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little, entry.group_end_handle);

        if (end_handle < start_handle) {
          bt_log(DEBUG, "gatt", "received malformed service range values");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        if (start_handle < discovery_range_start ||
            start_handle > discovery_range_end) {
          bt_log(DEBUG,
                 "gatt",
                 "received service range values outside of requested range");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        // "The Handles Information List is ordered sequentially based on the
        // found attribute handles." (Core Spec v5.3, Vol 3, Part F,
        // Sec 3.4.3.4)
        if (last_handle.has_value() && start_handle <= last_handle.value()) {
          bt_log(DEBUG, "gatt", "received services out of order");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        ServiceData service(kind, start_handle, end_handle, uuid);

        // Notify the handler.
        svc_cb(service);

        // HandlesInformationList is a single element of the list.
        size_t entry_length = sizeof(att::HandlesInformationList);
        handle_list = handle_list.view(entry_length);

        last_handle = service.range_end;
      }

      // The procedure is over if we have reached the end of the handle range.
      if (!last_handle.has_value() ||
          last_handle.value() == discovery_range_end) {
        res_cb(fit::ok());
        return;
      }

      // Request the next batch.
      DiscoverServicesByUuidInRange(kind,
                                    last_handle.value() + 1,
                                    discovery_range_end,
                                    std::move(svc_cb),
                                    std::move(res_cb),
                                    uuid);
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void DiscoverCharacteristics(att::Handle range_start,
                               att::Handle range_end,
                               CharacteristicCallback chrc_callback,
                               att::ResultFunction<> status_callback) override {
    PW_CHECK(range_start <= range_end);
    PW_CHECK(chrc_callback);
    PW_CHECK(status_callback);

    if (range_start == range_end) {
      status_callback(fit::ok());
      return;
    }

    auto read_by_type_cb = [this,
                            range_end,
                            chrc_cb = std::move(chrc_callback),
                            res_cb = status_callback.share()](
                               ReadByTypeResult result) mutable {
      TRACE_DURATION("bluetooth",
                     "gatt::Client::DiscoverCharacteristics read_by_type_cb");

      if (result.is_error()) {
        const auto error = result.error_value().error;

        // An Error Response code of "Attribute Not Found" indicates the end
        // of the procedure (v5.0, Vol 3, Part G, 4.6.1).
        if (error.is(att::ErrorCode::kAttributeNotFound)) {
          res_cb(fit::ok());
          return;
        }

        res_cb(fit::error(error));
        return;
      }

      auto& attributes = result.value();

      // ReadByTypeRequest() should return an error result if there are no
      // attributes in a success response.
      PW_CHECK(!attributes.empty());

      for (auto& char_attr : attributes) {
        Properties properties = 0u;
        att::Handle value_handle = 0u;
        UUID value_uuid;

        // The characteristic declaration value contains:
        // 1 octet: properties
        // 2 octets: value handle
        // 2 or 16 octets: UUID
        if (char_attr.value.size() ==
            sizeof(CharacteristicDeclarationAttributeValue<
                   att::UUIDType::k16Bit>)) {
          auto attr_value = char_attr.value.To<
              CharacteristicDeclarationAttributeValue<att::UUIDType::k16Bit>>();
          properties = attr_value.properties;
          value_handle = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                     attr_value.value_handle);
          value_uuid = UUID(attr_value.value_uuid);
        } else if (char_attr.value.size() ==
                   sizeof(CharacteristicDeclarationAttributeValue<
                          att::UUIDType::k128Bit>)) {
          auto attr_value =
              char_attr.value.To<CharacteristicDeclarationAttributeValue<
                  att::UUIDType::k128Bit>>();
          properties = attr_value.properties;
          value_handle = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                     attr_value.value_handle);
          value_uuid = UUID(attr_value.value_uuid);
        } else {
          bt_log(DEBUG,
                 "gatt",
                 "invalid characteristic declaration attribute value size");
          att_->ShutDown();
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        // Vol 3, Part G, 3.3: "The Characteristic Value declaration shall
        // exist immediately following the characteristic declaration."
        if (value_handle != char_attr.handle + 1) {
          bt_log(
              DEBUG, "gatt", "characteristic value doesn't follow declaration");
          res_cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        // Notify the handler. By default, there are no extended properties to
        // report.
        chrc_cb(CharacteristicData(properties,
                                   /*ext_props=*/std::nullopt,
                                   char_attr.handle,
                                   value_handle,
                                   value_uuid));
      }

      // The procedure is over if we have reached the end of the handle
      // range.
      const auto last_handle = attributes.back().handle;
      if (last_handle == range_end) {
        res_cb(fit::ok());
        return;
      }

      // Request the next batch.
      DiscoverCharacteristics(
          last_handle + 1, range_end, std::move(chrc_cb), std::move(res_cb));
    };

    ReadByTypeRequest(types::kCharacteristicDeclaration,
                      range_start,
                      range_end,
                      std::move(read_by_type_cb));
  }

  void DiscoverDescriptors(att::Handle range_start,
                           att::Handle range_end,
                           DescriptorCallback desc_callback,
                           att::ResultFunction<> status_callback) override {
    PW_DCHECK(range_start <= range_end);
    PW_DCHECK(desc_callback);
    PW_DCHECK(status_callback);

    auto pdu = NewPDU(sizeof(att::FindInformationRequestParams));
    if (!pdu) {
      status_callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kFindInformationRequest, pdu.get());
    auto* params = writer.mutable_payload<att::FindInformationRequestParams>();
    params->start_handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, range_start);
    params->end_handle =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, range_end);

    auto rsp_cb = [this,
                   range_start,
                   range_end,
                   desc_cb = std::move(desc_callback),
                   res_cb = std::move(status_callback)](
                      att::Bearer::TransactionResult result) mutable {
      if (result.is_error()) {
        const att::Error& error = result.error_value().first;

        // An Error Response code of "Attribute Not Found" indicates the end of
        // the procedure (v5.0, Vol 3, Part G, 4.7.1).
        if (error.is(att::ErrorCode::kAttributeNotFound)) {
          res_cb(fit::ok());
          return;
        }

        res_cb(fit::error(error));
        return;
      }
      const att::PacketReader& rsp = result.value();
      PW_DCHECK(rsp.opcode() == att::kFindInformationResponse);
      TRACE_DURATION("bluetooth",
                     "gatt::Client::DiscoverDescriptors rsp_cb",
                     "size",
                     rsp.size());

      if (rsp.payload_size() < sizeof(att::FindInformationResponseParams)) {
        bt_log(DEBUG, "gatt", "received malformed Find Information response");
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      const auto& rsp_params =
          rsp.payload<att::FindInformationResponseParams>();
      BufferView entries = rsp.payload_data().view(sizeof(rsp_params.format));

      att::Handle last_handle;
      bool well_formed;
      switch (rsp_params.format) {
        case att::UUIDType::k16Bit:
          well_formed =
              ProcessDescriptorDiscoveryResponse<att::UUIDType::k16Bit>(
                  range_start,
                  range_end,
                  entries,
                  desc_cb.share(),
                  &last_handle);
          break;
        case att::UUIDType::k128Bit:
          well_formed =
              ProcessDescriptorDiscoveryResponse<att::UUIDType::k128Bit>(
                  range_start,
                  range_end,
                  entries,
                  desc_cb.share(),
                  &last_handle);
          break;
        default:
          bt_log(DEBUG, "gatt", "invalid information data format");
          well_formed = false;
          break;
      }

      if (!well_formed) {
        att_->ShutDown();
        res_cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      // The procedure is over if we have reached the end of the handle range.
      if (last_handle == range_end) {
        res_cb(fit::ok());
        return;
      }

      // Request the next batch.
      DiscoverDescriptors(
          last_handle + 1, range_end, std::move(desc_cb), std::move(res_cb));
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void ReadRequest(att::Handle handle, ReadCallback callback) override {
    auto pdu = NewPDU(sizeof(att::ReadRequestParams));
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory),
               BufferView(),
               /*maybe_truncated=*/false);
      return;
    }

    att::PacketWriter writer(att::kReadRequest, pdu.get());
    auto params = writer.mutable_payload<att::ReadRequestParams>();
    params->handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);

    auto rsp_cb = [this, cb = std::move(callback)](
                      att::Bearer::TransactionResult result) {
      if (result.is_ok()) {
        const att::PacketReader& rsp = result.value();
        PW_DCHECK(rsp.opcode() == att::kReadResponse);
        bool maybe_truncated =
            (rsp.payload_size() != att::kMaxAttributeValueLength) &&
            (rsp.payload_size() == (mtu() - sizeof(rsp.opcode())));
        cb(fit::ok(), rsp.payload_data(), maybe_truncated);
        return;
      }
      const auto& [error, err_handle] = result.error_value();
      bt_log(DEBUG,
             "gatt",
             "read request failed: %s, handle %#.4x",
             bt_str(error),
             err_handle);
      cb(fit::error(error), BufferView(), /*maybe_truncated=*/false);
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void ReadByTypeRequest(const UUID& type,
                         att::Handle start_handle,
                         att::Handle end_handle,
                         ReadByTypeCallback callback) override {
    size_t type_size = type.CompactSize(/*allow_32bit=*/false);
    PW_CHECK(type_size == sizeof(uint16_t) || type_size == sizeof(UInt128));
    auto pdu = NewPDU(type_size == sizeof(uint16_t)
                          ? sizeof(att::ReadByTypeRequestParams16)
                          : sizeof(att::ReadByTypeRequestParams128));
    if (!pdu) {
      callback(fit::error(
          ReadByTypeError{Error(HostError::kOutOfMemory), std::nullopt}));
      return;
    }

    att::PacketWriter writer(att::kReadByTypeRequest, pdu.get());
    if (type_size == sizeof(uint16_t)) {
      auto params = writer.mutable_payload<att::ReadByTypeRequestParams16>();
      params->start_handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, start_handle);
      params->end_handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, end_handle);
      auto type_view = MutableBufferView(&params->type, sizeof(params->type));
      type.ToBytes(&type_view, /*allow_32bit=*/false);
    } else {
      auto params = writer.mutable_payload<att::ReadByTypeRequestParams128>();
      params->start_handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, start_handle);
      params->end_handle =
          pw::bytes::ConvertOrderTo(cpp20::endian::little, end_handle);
      auto type_view = MutableBufferView(&params->type, sizeof(params->type));
      type.ToBytes(&type_view, /*allow_32bit=*/false);
    }

    auto rsp_cb = [this, cb = std::move(callback), start_handle, end_handle](
                      att::Bearer::TransactionResult result) {
      if (result.is_error()) {
        const auto& [error, handle] = result.error_value();
        bt_log(DEBUG,
               "gatt",
               "read by type request failed: %s, handle %#.4x",
               bt_str(error),
               handle);
        // Only some errors have handles.
        std::optional<att::Handle> cb_handle =
            handle ? std::optional(handle) : std::nullopt;
        cb(fit::error(ReadByTypeError{error, cb_handle}));
        return;
      }
      const att::PacketReader& rsp = result.value();
      PW_CHECK(rsp.opcode() == att::kReadByTypeResponse);
      if (rsp.payload_size() < sizeof(att::ReadByTypeResponseParams)) {
        cb(fit::error(
            ReadByTypeError{Error(HostError::kPacketMalformed), std::nullopt}));
        return;
      }

      const auto& params = rsp.payload<att::ReadByTypeResponseParams>();
      // The response contains a list of attribute handle-value pairs of uniform
      // length.
      const size_t list_size = rsp.payload_size() - sizeof(params.length);
      const size_t pair_size = params.length;

      // Success response must:
      // a) Specify valid pair length (at least the size of a handle).
      // b) Have at least 1 pair (otherwise the Attribute Not Found error should
      // have been
      //    sent).
      // c) Have a list size that is evenly divisible by pair size.
      if (pair_size < sizeof(att::Handle) || list_size < sizeof(att::Handle) ||
          list_size % pair_size != 0) {
        cb(fit::error(
            ReadByTypeError{Error(HostError::kPacketMalformed), std::nullopt}));
        return;
      }

      std::vector<ReadByTypeValue> attributes;
      BufferView attr_list_view(params.attribute_data_list,
                                rsp.payload_size() - sizeof(params.length));
      while (attr_list_view.size() >= params.length) {
        const BufferView pair_view = attr_list_view.view(0, pair_size);
        const att::Handle handle = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little, pair_view.To<att::Handle>());

        if (handle < start_handle || handle > end_handle) {
          bt_log(TRACE,
                 "gatt",
                 "client received read by type response with handle outside of "
                 "requested range");
          cb(fit::error(ReadByTypeError{Error(HostError::kPacketMalformed),
                                        std::nullopt}));
          return;
        }

        if (!attributes.empty() && attributes.back().handle >= handle) {
          bt_log(TRACE,
                 "gatt",
                 "client received read by type response with handles in "
                 "non-increasing order");
          cb(fit::error(ReadByTypeError{Error(HostError::kPacketMalformed),
                                        std::nullopt}));
          return;
        }

        auto value_view = pair_view.view(sizeof(att::Handle));

        // The value may be truncated if it maxes out the length parameter or
        // the MTU, whichever is smaller (Core Spec v5.2, Vol 3, Part F,
        // Sec 3.4.4).
        const size_t mtu_max_value_size =
            mtu() - sizeof(att::kReadByTypeResponse) -
            sizeof(att::ReadByTypeResponseParams) - sizeof(att::Handle);
        bool maybe_truncated =
            (value_view.size() ==
             std::min(static_cast<size_t>(att::kMaxReadByTypeValueLength),
                      mtu_max_value_size));

        attributes.push_back(
            ReadByTypeValue{handle, value_view, maybe_truncated});

        // Advance list view to next pair (or end of list).
        attr_list_view = attr_list_view.view(pair_size);
      }
      PW_CHECK(attr_list_view.size() == 0);

      cb(fit::ok(std::move(attributes)));
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void ReadBlobRequest(att::Handle handle,
                       uint16_t offset,
                       ReadCallback callback) override {
    auto pdu = NewPDU(sizeof(att::ReadBlobRequestParams));
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory),
               BufferView(),
               /*maybe_truncated=*/false);
      return;
    }

    att::PacketWriter writer(att::kReadBlobRequest, pdu.get());
    auto params = writer.mutable_payload<att::ReadBlobRequestParams>();
    params->handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);
    params->offset = pw::bytes::ConvertOrderTo(cpp20::endian::little, offset);

    auto rsp_cb = [this, offset, cb = std::move(callback)](
                      att::Bearer::TransactionResult result) {
      if (result.is_ok()) {
        const att::PacketReader& rsp = result.value();
        PW_DCHECK(rsp.opcode() == att::kReadBlobResponse);
        bool maybe_truncated =
            (static_cast<size_t>(offset) + rsp.payload_size() !=
             att::kMaxAttributeValueLength) &&
            (rsp.payload_data().size() == (mtu() - sizeof(att::OpCode)));
        cb(fit::ok(), rsp.payload_data(), maybe_truncated);
        return;
      }
      const auto& [error, err_handle] = result.error_value();
      bt_log(DEBUG,
             "gatt",
             "read blob request failed: %s, handle: %#.4x",
             bt_str(error),
             err_handle);
      cb(fit::error(error), BufferView(), /*maybe_truncated=*/false);
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void WriteRequest(att::Handle handle,
                    const ByteBuffer& value,
                    att::ResultFunction<> callback) override {
    const size_t payload_size = sizeof(att::WriteRequestParams) + value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(TRACE, "gatt", "write request payload exceeds MTU");
      callback(ToResult(HostError::kPacketMalformed));
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::WriteRequestParams>();
    params->handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);

    auto value_view =
        writer.mutable_payload_data().mutable_view(sizeof(att::Handle));
    value.Copy(&value_view);

    auto rsp_cb = [this, cb = std::move(callback)](
                      att::Bearer::TransactionResult result) {
      if (result.is_error()) {
        const auto& [error, err_handle] = result.error_value();
        bt_log(DEBUG,
               "gatt",
               "write request failed: %s, handle: %#.2x",
               bt_str(error),
               err_handle);
        cb(fit::error(error));
        return;
      }
      const att::PacketReader& rsp = result.value();
      PW_DCHECK(rsp.opcode() == att::kWriteResponse);

      if (rsp.payload_size()) {
        att_->ShutDown();
        cb(ToResult(HostError::kPacketMalformed));
        return;
      }

      cb(fit::ok());
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  // An internal object for storing the write queue, callback, and reliability
  // mode of a long write operation.
  struct PreparedWrite {
    bt::att::PrepareWriteQueue prep_write_queue;
    bt::att::ResultFunction<> callback;
    ReliableMode reliable_mode;
  };

  void ExecutePrepareWrites(att::PrepareWriteQueue prep_write_queue,
                            ReliableMode reliable_mode,
                            att::ResultFunction<> callback) override {
    PreparedWrite new_request;
    new_request.prep_write_queue = std::move(prep_write_queue);
    new_request.callback = std::move(callback);
    new_request.reliable_mode = std::move(reliable_mode);
    long_write_queue_.push(std::move(new_request));

    // If the |long_write_queue| has a pending request, then appending this
    // request will be sufficient, otherwise kick off the request.
    if (long_write_queue_.size() == 1) {
      ProcessWriteQueue(std::move(long_write_queue_.front()));
    }
  }

  void ProcessWriteQueue(PreparedWrite prepared_write) {
    if (!prepared_write.prep_write_queue.empty()) {
      att::QueuedWrite prep_write_request =
          std::move(prepared_write.prep_write_queue.front());
      // A copy of the |prep_write_request| is made to pass into the capture
      // list for |prep_write_cb|. It will be used to validate the echoed blob.
      auto prep_write_copy = att::QueuedWrite(prep_write_request.handle(),
                                              prep_write_request.offset(),
                                              prep_write_request.value());
      prepared_write.prep_write_queue.pop();

      auto prep_write_cb = [this,
                            prep_write = std::move(prepared_write),
                            requested_blob = std::move(prep_write_copy)](
                               att::Result<> status,
                               const ByteBuffer& blob) mutable {
        // If the write fails, cancel the prep writes and then move on to the
        // next long write in the queue. The device will echo the value written
        // in the blob, according to the spec (Vol 3, Part G, 4.9.4). The offset
        // and value will be verified if the requested |prep_write.mode| is
        // enabled (Vol 3, Part G, 4.9.5).

        if (prep_write.reliable_mode == ReliableMode::kEnabled) {
          if (blob.size() < sizeof(att::PrepareWriteResponseParams)) {
            // The response blob is malformed.
            status = ToResult(HostError::kNotReliable);
          } else {
            uint16_t blob_offset = pw::bytes::ConvertOrderFrom(
                cpp20::endian::little,
                blob.ReadMember<&att::PrepareWriteResponseParams::offset>());
            auto blob_value =
                blob.view(sizeof(att::PrepareWriteResponseParams));
            if ((blob_offset != requested_blob.offset()) ||
                !(blob_value == requested_blob.value())) {
              status = ToResult(HostError::kNotReliable);
            }
          }
        }

        if (status.is_error()) {
          auto exec_write_cb = [this,
                                self = weak_self_.GetWeakPtr(),
                                callback = std::move(prep_write.callback),
                                prep_write_status =
                                    status](att::Result<>) mutable {
            // In this case return the original failure status. This
            // effectively overrides the ExecuteWrite status.
            callback(prep_write_status);
            if (!self.is_alive()) {
              return;
            }
            // Now that this request is complete, remove it from the overall
            // queue.
            PW_DCHECK(!long_write_queue_.empty());
            long_write_queue_.pop();

            if (long_write_queue_.size() > 0) {
              ProcessWriteQueue(std::move(long_write_queue_.front()));
            }
          };

          ExecuteWriteRequest(att::ExecuteWriteFlag::kCancelAll,
                              std::move(exec_write_cb));

          return;
        }

        ProcessWriteQueue(std::move(prep_write));
      };

      PrepareWriteRequest(prep_write_request.handle(),
                          prep_write_request.offset(),
                          std::move(prep_write_request.value()),
                          std::move(prep_write_cb));
    }
    // End of this write, send and prepare for next item in overall write queue
    else {
      auto exec_write_cb = [this,
                            self = weak_self_.GetWeakPtr(),
                            callback = std::move(prepared_write.callback)](
                               att::Result<> status) mutable {
        callback(status);
        if (!self.is_alive()) {
          return;
        }
        // Now that this request is complete, remove it from the overall
        // queue.
        PW_DCHECK(!long_write_queue_.empty());
        long_write_queue_.pop();

        // If the super queue still has any long writes left to execute,
        // initiate them
        if (long_write_queue_.size() > 0) {
          ProcessWriteQueue(std::move(long_write_queue_.front()));
        }
      };

      ExecuteWriteRequest(att::ExecuteWriteFlag::kWritePending,
                          std::move(exec_write_cb));
    }
  }

  void PrepareWriteRequest(att::Handle handle,
                           uint16_t offset,
                           const ByteBuffer& part_value,
                           PrepareCallback callback) override {
    const size_t payload_size =
        sizeof(att::PrepareWriteRequestParams) + part_value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(TRACE, "gatt", "prepare write request payload exceeds MTU");
      callback(ToResult(HostError::kPacketMalformed), BufferView());
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory), BufferView());
      return;
    }

    att::PacketWriter writer(att::kPrepareWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::PrepareWriteRequestParams>();
    params->handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);
    params->offset = pw::bytes::ConvertOrderTo(cpp20::endian::little, offset);

    auto header_size = sizeof(att::Handle) + sizeof(uint16_t);
    auto value_view = writer.mutable_payload_data().mutable_view(header_size);
    part_value.Copy(&value_view);

    auto rsp_cb =
        [cb = std::move(callback)](att::Bearer::TransactionResult result) {
          if (result.is_ok()) {
            const att::PacketReader& rsp = result.value();
            PW_DCHECK(rsp.opcode() == att::kPrepareWriteResponse);
            cb(fit::ok(), rsp.payload_data());
            return;
          }
          const auto& [error, err_handle] = result.error_value();
          bt_log(DEBUG,
                 "gatt",
                 "prepare write request failed: %s, handle:"
                 "%#.4x",
                 bt_str(error),
                 err_handle);
          cb(fit::error(error), BufferView());
        };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void ExecuteWriteRequest(att::ExecuteWriteFlag flag,
                           att::ResultFunction<> callback) override {
    const size_t payload_size = sizeof(att::ExecuteWriteRequestParams);
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      // This really shouldn't happen because we aren't consuming any actual
      // payload here, but just in case...
      bt_log(TRACE, "gatt", "execute write request size exceeds MTU");
      callback(ToResult(HostError::kPacketMalformed));
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kExecuteWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::ExecuteWriteRequestParams>();
    params->flags = flag;

    auto rsp_cb = [this, cb = std::move(callback)](
                      att::Bearer::TransactionResult result) {
      if (result.is_ok()) {
        const att::PacketReader& rsp = result.value();
        PW_DCHECK(rsp.opcode() == att::kExecuteWriteResponse);

        if (rsp.payload_size()) {
          att_->ShutDown();
          cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        cb(fit::ok());
        return;
      }
      const att::Error& error = result.error_value().first;
      bt_log(DEBUG, "gatt", "execute write request failed: %s", bt_str(error));
      cb(fit::error(error));
    };

    att_->StartTransaction(std::move(pdu), BindCallback(std::move(rsp_cb)));
  }

  void WriteWithoutResponse(att::Handle handle,
                            const ByteBuffer& value,
                            att::ResultFunction<> callback) override {
    const size_t payload_size = sizeof(att::WriteRequestParams) + value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(DEBUG, "gatt", "write request payload exceeds MTU");
      callback(ToResult(HostError::kFailed));
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(ToResult(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kWriteCommand, pdu.get());
    auto params = writer.mutable_payload<att::WriteRequestParams>();
    params->handle = pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);

    auto value_view =
        writer.mutable_payload_data().mutable_view(sizeof(att::Handle));
    value.Copy(&value_view);

    [[maybe_unused]] bool _ = att_->SendWithoutResponse(std::move(pdu));
    callback(fit::ok());
  }

  void SetNotificationHandler(NotificationCallback handler) override {
    notification_handler_ = std::move(handler);
  }

  // Wraps |callback| in a TransactionCallback that only runs if this Client is
  // still alive.
  att::Bearer::TransactionCallback BindCallback(
      att::Bearer::TransactionCallback callback) {
    return [self = weak_self_.GetWeakPtr(),
            cb = std::move(callback)](auto rsp) mutable {
      if (self.is_alive()) {
        cb(rsp);
      }
    };
  }

  att::Bearer::WeakPtr att_;
  att::Bearer::HandlerId not_handler_id_;
  att::Bearer::HandlerId ind_handler_id_;

  NotificationCallback notification_handler_;
  // |long_write_queue_| contains long write requests, their
  // associated callbacks and reliable write modes.
  // Series of PrepareWrites are executed or cancelled at the same time so
  // this is used to block while a single series is processed.
  //
  // While the top element is processed, the |PrepareWriteQueue| and callback
  // will be empty and will be popped once the queue is cancelled or executed.
  // Following the processing of each queue, the client will automatically
  // process the next queue in the |long_write_queue_|.
  std::queue<PreparedWrite> long_write_queue_;
  WeakSelf<Client> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Impl);
};

// static
std::unique_ptr<Client> Client::Create(att::Bearer::WeakPtr bearer) {
  return std::make_unique<Impl>(std::move(bearer));
}

}  // namespace bt::gatt
