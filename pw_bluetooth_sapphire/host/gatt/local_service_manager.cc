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

#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <algorithm>
#include <cinttypes>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

namespace bt::gatt {
namespace {

// Adds characteristic definition attributes to |grouping| for |chrc|. Returns
// the characteristic value handle.
att::Handle InsertCharacteristicAttributes(
    att::AttributeGrouping* grouping,
    const Characteristic& chrc,
    att::Attribute::ReadHandler read_handler,
    att::Attribute::WriteHandler write_handler) {
  PW_DCHECK(grouping);
  PW_DCHECK(!grouping->complete());
  PW_DCHECK(read_handler);
  PW_DCHECK(write_handler);

  // Characteristic Declaration (Vol 3, Part G, 3.3.1).
  auto* decl_attr = grouping->AddAttribute(
      types::kCharacteristicDeclaration,
      att::AccessRequirements(/*encryption=*/false,
                              /*authentication=*/false,
                              /*authorization=*/false),  // read (no security)
      att::AccessRequirements());                        // write (not allowed)
  PW_DCHECK(decl_attr);

  // Characteristic Value Declaration (Vol 3, Part G, 3.3.2)
  auto* value_attr = grouping->AddAttribute(
      chrc.type(), chrc.read_permissions(), chrc.write_permissions());
  PW_DCHECK(value_attr);

  value_attr->set_read_handler(std::move(read_handler));
  value_attr->set_write_handler(std::move(write_handler));

  size_t uuid_size = chrc.type().CompactSize(/*allow_32bit=*/false);
  PW_DCHECK(uuid_size == 2 || uuid_size == 16);

  // The characteristic declaration value contains:
  // 1 octet: properties
  // 2 octets: value handle
  // 2 or 16 octets: UUID
  DynamicByteBuffer decl_value(3 + uuid_size);
  decl_value[0] = chrc.properties();
  decl_value[1] = static_cast<uint8_t>(value_attr->handle());
  decl_value[2] = static_cast<uint8_t>(value_attr->handle() >> 8);

  auto uuid_view = decl_value.mutable_view(3);
  chrc.type().ToBytes(&uuid_view, /*allow_32bit=*/false);
  decl_attr->SetValue(decl_value);

  return value_attr->handle();
}

// Adds a characteristic descriptor declaration to |grouping| for |desc|.
void InsertDescriptorAttribute(att::AttributeGrouping* grouping,
                               const UUID& type,
                               const att::AccessRequirements& read_reqs,
                               const att::AccessRequirements& write_reqs,
                               att::Attribute::ReadHandler read_handler,
                               att::Attribute::WriteHandler write_handler) {
  PW_DCHECK(grouping);
  PW_DCHECK(!grouping->complete());
  PW_DCHECK(read_handler);
  PW_DCHECK(write_handler);

  // There is no special declaration attribute type for descriptors.
  auto* attr = grouping->AddAttribute(type, read_reqs, write_reqs);
  PW_DCHECK(attr);

  attr->set_read_handler(std::move(read_handler));
  attr->set_write_handler(std::move(write_handler));
}

// Returns false if the given service hierarchy contains repeating identifiers.
// Returns the number of attributes that will be in the service attribute group
// (exluding the service declaration) in |out_attrs|.
bool ValidateService(const Service& service, size_t* out_attr_count) {
  PW_DCHECK(out_attr_count);

  size_t attr_count = 0u;
  std::unordered_set<IdType> ids;
  for (const auto& chrc_ptr : service.characteristics()) {
    if (ids.count(chrc_ptr->id()) != 0u) {
      bt_log(TRACE, "gatt", "server: repeated ID: %" PRIu64, chrc_ptr->id());
      return false;
    }

    ids.insert(chrc_ptr->id());

    // +1: Characteristic Declaration (Vol 3, Part G, 3.3.1)
    // +1: Characteristic Value Declaration (Vol 3, Part G, 3.3.2)
    attr_count += 2;

    // Increment the count for the CCC descriptor if the characteristic supports
    // notifications or indications.
    if ((chrc_ptr->properties() & Property::kNotify) ||
        (chrc_ptr->properties() & Property::kIndicate)) {
      attr_count++;
    }

    for (const auto& desc_ptr : chrc_ptr->descriptors()) {
      if (ids.count(desc_ptr->id()) != 0u) {
        bt_log(TRACE, "gatt", "server: repeated ID: %" PRIu64, desc_ptr->id());
        return false;
      }

      // Reject descriptors with types that are internally managed by us.
      if (desc_ptr->type() == types::kClientCharacteristicConfig ||
          desc_ptr->type() == types::kCharacteristicExtProperties ||
          desc_ptr->type() == types::kServerCharacteristicConfig) {
        bt_log(TRACE,
               "gatt",
               "server: disallowed descriptor type: %s",
               desc_ptr->type().ToString().c_str());
        return false;
      }

      ids.insert(desc_ptr->id());

      // +1: Characteristic Descriptor Declaration (Vol 3, Part G, 3.3.3)
      attr_count++;
    }
    if (chrc_ptr->extended_properties()) {
      attr_count++;
    }
  }

  *out_attr_count = attr_count;

  return true;
}

}  // namespace

class LocalServiceManager::ServiceData final {
 public:
  ServiceData(IdType id,
              att::AttributeGrouping* grouping,
              Service* service,
              ReadHandler&& read_handler,
              WriteHandler&& write_handler,
              ClientConfigCallback&& ccc_callback)
      : id_(id),
        read_handler_(std::forward<ReadHandler>(read_handler)),
        write_handler_(std::forward<WriteHandler>(write_handler)),
        ccc_callback_(std::forward<ClientConfigCallback>(ccc_callback)),
        weak_self_(this) {
    PW_DCHECK(read_handler_);
    PW_DCHECK(write_handler_);
    PW_DCHECK(ccc_callback_);
    PW_DCHECK(grouping);

    start_handle_ = grouping->start_handle();
    end_handle_ = grouping->end_handle();

    // Sort characteristics by UUID size (see Vol 3, Part G, 3.3.1).
    auto chrcs = service->ReleaseCharacteristics();
    std::sort(chrcs.begin(),
              chrcs.end(),
              [](const auto& chrc_ptr1, const auto& chrc_ptr2) {
                return chrc_ptr1->type().CompactSize(/*allow_32bit=*/false) <
                       chrc_ptr2->type().CompactSize(/*allow_32bit=*/false);
              });
    for (auto& chrc : chrcs) {
      AddCharacteristic(grouping, std::move(chrc));
    }
  }

  inline IdType id() const { return id_; }
  inline att::Handle start_handle() const { return start_handle_; }
  inline att::Handle end_handle() const { return end_handle_; }

  bool GetCharacteristicConfig(IdType chrc_id,
                               PeerId peer_id,
                               ClientCharacteristicConfig* out_config) {
    PW_DCHECK(out_config);

    auto iter = chrc_configs_.find(chrc_id);
    if (iter == chrc_configs_.end())
      return false;

    uint16_t value = iter->second.Get(peer_id);
    out_config->handle = iter->second.handle();
    out_config->notify = value & kCCCNotificationBit;
    out_config->indicate = value & kCCCIndicationBit;

    return true;
  }

  // Clean up our knoweledge of the diconnecting peer.
  void DisconnectClient(PeerId peer_id) {
    for (auto& id_config_pair : chrc_configs_) {
      id_config_pair.second.Erase(peer_id);
    }
  }

 private:
  class CharacteristicConfig {
   public:
    explicit CharacteristicConfig(att::Handle handle) : handle_(handle) {}
    CharacteristicConfig(CharacteristicConfig&&) = default;
    CharacteristicConfig& operator=(CharacteristicConfig&&) = default;

    // The characteristic handle.
    att::Handle handle() const { return handle_; }

    uint16_t Get(PeerId peer_id) {
      auto iter = client_states_.find(peer_id);

      // If a configuration doesn't exist for |peer_id| then return the default
      // value.
      if (iter == client_states_.end())
        return 0;

      return iter->second;
    }

    void Set(PeerId peer_id, uint16_t value) {
      client_states_[peer_id] = value;
    }

    void Erase(PeerId peer_id) { client_states_.erase(peer_id); }

   private:
    att::Handle handle_;
    std::unordered_map<PeerId, uint16_t> client_states_;

    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(CharacteristicConfig);
  };

  // Called when a read request is performed on a CCC descriptor belonging to
  // the characteristic identified by |chrc_id|.
  void OnReadCCC(IdType chrc_id,
                 PeerId peer_id,
                 att::Handle,
                 uint16_t,
                 att::Attribute::ReadResultCallback result_cb) {
    uint16_t value = 0;
    auto iter = chrc_configs_.find(chrc_id);
    if (iter != chrc_configs_.end()) {
      value = iter->second.Get(peer_id);
    }

    value = pw::bytes::ConvertOrderTo(cpp20::endian::little, value);
    result_cb(
        fit::ok(),
        BufferView(reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
  }

  // Called when a write request is performed on a CCC descriptor belonging to
  // the characteristic identified by |chrc_id|.
  void OnWriteCCC(IdType chrc_id,
                  uint8_t chrc_props,
                  PeerId peer_id,
                  att::Handle handle,
                  uint16_t offset,
                  const ByteBuffer& value,
                  att::Attribute::WriteResultCallback result_cb) {
    if (offset != 0u) {
      result_cb(fit::error(att::ErrorCode::kInvalidOffset));
      return;
    }

    if (value.size() != sizeof(uint16_t)) {
      result_cb(fit::error(att::ErrorCode::kInvalidAttributeValueLength));
      return;
    }

    uint16_t ccc_value = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                     value.To<uint16_t>());
    if (ccc_value > (kCCCNotificationBit | kCCCIndicationBit)) {
      result_cb(fit::error(att::ErrorCode::kInvalidPDU));
      return;
    }

    bool notify = ccc_value & kCCCNotificationBit;
    bool indicate = ccc_value & kCCCIndicationBit;

    if ((notify && !(chrc_props & Property::kNotify)) ||
        (indicate && !(chrc_props & Property::kIndicate))) {
      result_cb(fit::error(att::ErrorCode::kWriteNotPermitted));
      return;
    }

    auto iter = chrc_configs_.find(chrc_id);
    if (iter == chrc_configs_.end()) {
      auto result_pair =
          chrc_configs_.emplace(chrc_id, CharacteristicConfig(handle));
      iter = result_pair.first;
    }

    // Send a reply back.
    result_cb(fit::ok());

    uint16_t current_value = iter->second.Get(peer_id);
    iter->second.Set(peer_id, ccc_value);

    if (current_value != ccc_value) {
      ccc_callback_(id_, chrc_id, peer_id, notify, indicate);
    }
  }

  void AddCharacteristic(att::AttributeGrouping* grouping,
                         CharacteristicPtr chrc) {
    // Set up the characteristic callbacks.
    // TODO(armansito): Consider tracking a transaction timeout here
    // (fxbug.dev/42142121).
    IdType id = chrc->id();
    uint8_t props = chrc->properties();
    uint16_t ext_props = chrc->extended_properties();
    auto self = weak_self_.GetWeakPtr();

    auto read_handler = [self, id, props](PeerId peer_id,
                                          att::Handle,
                                          uint16_t offset,
                                          auto result_cb) {
      if (!self.is_alive()) {
        result_cb(fit::error(att::ErrorCode::kUnlikelyError), BufferView());
        return;
      }

      // ATT permissions checks passed if we got here; also check the
      // characteristic property.
      if (!(props & Property::kRead)) {
        // TODO(armansito): Return kRequestNotSupported?
        result_cb(fit::error(att::ErrorCode::kReadNotPermitted), BufferView());
        return;
      }

      self->read_handler_(peer_id, self->id_, id, offset, std::move(result_cb));
    };

    auto write_handler = [self, id, props](PeerId peer_id,
                                           att::Handle,
                                           uint16_t offset,
                                           const auto& value,
                                           auto result_cb) {
      if (!self.is_alive()) {
        if (result_cb)
          result_cb(fit::error(att::ErrorCode::kUnlikelyError));
        return;
      }

      // If |result_cb| was provided, then this is a write request and the
      // characteristic must support the "write" procedure.
      if (result_cb && !(props & Property::kWrite)) {
        // TODO(armansito): Return kRequestNotSupported?
        result_cb(fit::error(att::ErrorCode::kWriteNotPermitted));
        return;
      }

      if (!result_cb && !(props & Property::kWriteWithoutResponse))
        return;

      self->write_handler_(
          peer_id, self->id_, id, offset, value, std::move(result_cb));
    };

    att::Handle chrc_handle = InsertCharacteristicAttributes(
        grouping, *chrc, std::move(read_handler), std::move(write_handler));

    if (props & Property::kNotify || props & Property::kIndicate) {
      AddCCCDescriptor(grouping, *chrc, chrc_handle);
    }

    if (ext_props) {
      auto* decl_attr = grouping->AddAttribute(
          types::kCharacteristicExtProperties,
          att::AccessRequirements(
              /*encryption=*/false,
              /*authentication=*/false,
              /*authorization=*/false),  // read (no security)
          att::AccessRequirements());    // write (not allowed)
      PW_DCHECK(decl_attr);
      decl_attr->SetValue(StaticByteBuffer(
          (uint8_t)(ext_props & 0x00FF), (uint8_t)((ext_props & 0xFF00) >> 8)));
    }

    // TODO(armansito): Inject a SCC descriptor if the characteristic has the
    // broadcast property and if we ever support configured broadcasts.

    // Sort descriptors by UUID size. This is not required by the specification
    // but we do this to return as many descriptors as possible in a ATT Find
    // Information response.
    auto descs = chrc->ReleaseDescriptors();
    std::sort(descs.begin(),
              descs.end(),
              [](const auto& desc_ptr1, const auto& desc_ptr2) {
                return desc_ptr1->type().CompactSize(/*allow_32bit=*/false) <
                       desc_ptr2->type().CompactSize(/*allow_32bit=*/false);
              });
    for (auto& desc : descs) {
      AddDescriptor(grouping, std::move(desc));
    }
  }

  void AddDescriptor(att::AttributeGrouping* grouping, DescriptorPtr desc) {
    auto self = weak_self_.GetWeakPtr();
    auto read_handler = [self, id = desc->id()](PeerId peer_id,
                                                att::Handle,
                                                uint16_t offset,
                                                auto result_cb) {
      if (!self.is_alive()) {
        result_cb(fit::error(att::ErrorCode::kUnlikelyError), BufferView());
        return;
      }

      self->read_handler_(peer_id, self->id_, id, offset, std::move(result_cb));
    };

    auto write_handler = [self, id = desc->id()](PeerId peer_id,
                                                 att::Handle,
                                                 uint16_t offset,
                                                 const auto& value,
                                                 auto result_cb) {
      // Descriptors cannot be written using the "write without response"
      // procedure.
      if (!result_cb) {
        return;
      }

      if (!self.is_alive()) {
        result_cb(fit::error(att::ErrorCode::kUnlikelyError));
        return;
      }

      self->write_handler_(
          peer_id, self->id_, id, offset, value, std::move(result_cb));
    };

    InsertDescriptorAttribute(grouping,
                              desc->type(),
                              desc->read_permissions(),
                              desc->write_permissions(),
                              std::move(read_handler),
                              std::move(write_handler));
  }

  void AddCCCDescriptor(att::AttributeGrouping* grouping,
                        const Characteristic& chrc,
                        att::Handle chrc_handle) {
    PW_DCHECK(chrc.update_permissions().allowed());

    // Readable with no authentication or authorization (Vol 3, Part G,
    // 3.3.3.3). We let the service determine the encryption permission.
    att::AccessRequirements read_reqs(
        chrc.update_permissions().encryption_required(),
        /*authentication=*/false,
        /*authorization=*/false);

    IdType id = chrc.id();
    auto self = weak_self_.GetWeakPtr();

    auto read_handler = [self, id, chrc_handle](const auto& peer_id,
                                                att::Handle,
                                                uint16_t offset,
                                                auto result_cb) {
      if (!self.is_alive()) {
        result_cb(fit::error(att::ErrorCode::kUnlikelyError), BufferView());
        return;
      }

      self->OnReadCCC(id, peer_id, chrc_handle, offset, std::move(result_cb));
    };

    auto write_handler = [self, id, chrc_handle, props = chrc.properties()](
                             const auto& peer_id,
                             att::Handle,
                             uint16_t offset,
                             const auto& value,
                             auto result_cb) {
      // Descriptors cannot be written using the "write without response"
      // procedure.
      if (!result_cb) {
        return;
      }

      if (!self.is_alive()) {
        result_cb(fit::error(att::ErrorCode::kUnlikelyError));
        return;
      }

      self->OnWriteCCC(
          id, props, peer_id, chrc_handle, offset, value, std::move(result_cb));
    };

    // The write permission is determined by the service.
    InsertDescriptorAttribute(grouping,
                              types::kClientCharacteristicConfig,
                              read_reqs,
                              chrc.update_permissions(),
                              std::move(read_handler),
                              std::move(write_handler));
  }

  IdType id_;
  att::Handle start_handle_;
  att::Handle end_handle_;
  ReadHandler read_handler_;
  WriteHandler write_handler_;
  ClientConfigCallback ccc_callback_;

  // Characteristic configuration states.
  // TODO(armansito): Add a mechanism to persist client configuration for bonded
  // devices.
  std::unordered_map<IdType, CharacteristicConfig> chrc_configs_;

  WeakSelf<ServiceData> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ServiceData);
};

LocalServiceManager::LocalServiceManager()
    : WeakSelf(this),
      db_(std::make_unique<att::Database>()),
      next_service_id_(1ull) {
  PW_DCHECK(db_);
}

LocalServiceManager::~LocalServiceManager() = default;

IdType LocalServiceManager::RegisterService(ServicePtr service,
                                            ReadHandler read_handler,
                                            WriteHandler write_handler,
                                            ClientConfigCallback ccc_callback) {
  PW_DCHECK(service);
  PW_DCHECK(read_handler);
  PW_DCHECK(write_handler);
  PW_DCHECK(ccc_callback);

  if (services_.find(next_service_id_) != services_.end()) {
    bt_log(TRACE, "gatt", "server: Ran out of service IDs");
    return kInvalidId;
  }

  size_t attr_count;
  if (!ValidateService(*service, &attr_count))
    return kInvalidId;

  // GATT does not support 32-bit UUIDs.
  const BufferView service_decl_value =
      service->type().CompactView(/*allow_32bit=*/false);

  // TODO(armansito): Cluster services with 16-bit and 128-bit together inside
  // |db_| (Vol 3, Part G, 3.1).

  att::AttributeGrouping* grouping = db_->NewGrouping(
      service->primary() ? types::kPrimaryService : types::kSecondaryService,
      attr_count,
      service_decl_value);
  if (!grouping) {
    bt_log(DEBUG,
           "gatt",
           "server: Failed to allocate attribute grouping for service");
    return kInvalidId;
  }

  // Creating a ServiceData will populate the attribute grouping.
  auto service_data = std::make_unique<ServiceData>(next_service_id_,
                                                    grouping,
                                                    service.get(),
                                                    std::move(read_handler),
                                                    std::move(write_handler),
                                                    std::move(ccc_callback));
  PW_DCHECK(grouping->complete());
  grouping->set_active(true);

  // TODO(armansito): Handle potential 64-bit unsigned overflow?
  IdType id = next_service_id_++;

  services_[id] = std::move(service_data);
  if (service_changed_callback_) {
    service_changed_callback_(
        id, grouping->start_handle(), grouping->end_handle());
  }

  return id;
}

bool LocalServiceManager::UnregisterService(IdType service_id) {
  auto iter = services_.find(service_id);
  if (iter == services_.end())
    return false;

  const att::Handle start_handle = iter->second->start_handle();
  const att::Handle end_handle = iter->second->end_handle();
  db_->RemoveGrouping(start_handle);
  services_.erase(iter);

  if (service_changed_callback_) {
    service_changed_callback_(service_id, start_handle, end_handle);
  }
  return true;
}

bool LocalServiceManager::GetCharacteristicConfig(
    IdType service_id,
    IdType chrc_id,
    PeerId peer_id,
    ClientCharacteristicConfig* out_config) {
  PW_DCHECK(out_config);

  auto iter = services_.find(service_id);
  if (iter == services_.end())
    return false;

  return iter->second->GetCharacteristicConfig(chrc_id, peer_id, out_config);
}

void LocalServiceManager::DisconnectClient(PeerId peer_id) {
  for (auto& id_service_pair : services_) {
    id_service_pair.second->DisconnectClient(peer_id);
  }
}

}  // namespace bt::gatt
