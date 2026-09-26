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

#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"

#include <pw_assert/check.h>

#include "lib/fit/defer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/slab_allocator.h"

namespace bt::gatt {
namespace {

bool IsInternalUuid(const UUID& uuid) {
  // clang-format off
  return
    uuid == types::kPrimaryService ||
    uuid == types::kSecondaryService ||
    uuid == types::kIncludeDeclaration ||
    uuid == types::kCharacteristicDeclaration ||
    uuid == types::kCharacteristicExtProperties ||
    uuid == types::kCharacteristicUserDescription ||
    uuid == types::kClientCharacteristicConfig ||
    uuid == types::kServerCharacteristicConfig ||
    uuid == types::kCharacteristicFormat ||
    uuid == types::kCharacteristicAggregateFormat;
  // clang-format on
}

void ReportReadValueError(att::Result<> status,
                          RemoteService::ReadValueCallback callback) {
  callback(status, BufferView(), /*maybe_truncated=*/false);
}

CharacteristicMap CharacteristicsToCharacteristicMap(
    const std::map<CharacteristicHandle, RemoteCharacteristic>&
        characteristics) {
  CharacteristicMap characteristic_map;
  for (const auto& [_handle, chrc] : characteristics) {
    characteristic_map.try_emplace(_handle, chrc.info(), chrc.descriptors());
  }
  return characteristic_map;
}

}  // namespace

RemoteService::RemoteService(const ServiceData& service_data,
                             Client::WeakPtr client)
    : service_data_(service_data),
      client_(std::move(client)),
      remaining_descriptor_requests_(kSentinel) {
  PW_DCHECK(client_.is_alive());
}

RemoteService::~RemoteService() {
  for (auto& chr : characteristics_) {
    chr.second.set_service_changed(service_changed_);
  }
  characteristics_.clear();

  std::vector<fit::callback<void()>> rm_handlers = std::move(rm_handlers_);
  for (auto& handler : rm_handlers) {
    handler();
  }
}

bool RemoteService::AddRemovedHandler(fit::closure handler) {
  rm_handlers_.emplace_back(std::move(handler));
  return true;
}

void RemoteService::DiscoverCharacteristics(CharacteristicCallback callback) {
  // Characteristics already discovered. Return success.
  if (HasCharacteristics()) {
    // We return a new copy of only the immutable data of our characteristics
    // and their descriptors. This requires a copy, which *could* be expensive
    // in the (unlikely) case that a service has a very large number of
    // characteristics.
    callback(fit::ok(), CharacteristicsToCharacteristicMap(characteristics_));
    return;
  }

  // Queue this request.
  pending_discov_reqs_.emplace_back(std::move(callback));

  // Nothing to do if a write request is already pending.
  if (pending_discov_reqs_.size() > 1u)
    return;

  auto self = GetWeakPtr();
  auto chrc_cb = [self](const CharacteristicData& chr) {
    if (!self.is_alive()) {
      return;
    }
    // try_emplace should not fail here; our GATT::Client explicitly ensures
    // that handles are strictly ascending (as described in the spec) so we
    // should never see a handle collision
    self->characteristics_.try_emplace(
        CharacteristicHandle(chr.value_handle), self->client_, chr);
  };

  auto res_cb = [self](att::Result<> status) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (bt_is_error(status, TRACE, "gatt", "characteristic discovery failed")) {
      self->characteristics_.clear();
    }

    if (self->characteristics_.empty()) {
      if (status.is_ok()) {
        // This marks that characteristic discovery has completed
        // successfully.
        self->remaining_descriptor_requests_ = 0u;
      }

      // Skip descriptor discovery and end the procedure as no characteristics
      // were found (or the operation failed).
      self->CompleteCharacteristicDiscovery(status);
      return;
    }

    self->StartDescriptorDiscovery();
  };

  client_->DiscoverCharacteristics(service_data_.range_start,
                                   service_data_.range_end,
                                   std::move(chrc_cb),
                                   std::move(res_cb));
}

bool RemoteService::IsDiscovered() const {
  // TODO(armansito): Return true only if included services have also been
  // discovered.
  return HasCharacteristics();
}

void RemoteService::ReadCharacteristic(CharacteristicHandle id,
                                       ReadValueCallback callback) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    ReportReadValueError(status, std::move(callback));
    return;
  }

  if (!(chrc->info().properties & Property::kRead)) {
    bt_log(DEBUG, "gatt", "characteristic does not support \"read\"");
    ReportReadValueError(ToResult(HostError::kNotSupported),
                         std::move(callback));
    return;
  }

  client_->ReadRequest(chrc->info().value_handle, std::move(callback));
}

void RemoteService::ReadLongCharacteristic(CharacteristicHandle id,
                                           uint16_t offset,
                                           size_t max_bytes,
                                           ReadValueCallback callback) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    ReportReadValueError(status, std::move(callback));
    return;
  }

  if (!(chrc->info().properties & Property::kRead)) {
    bt_log(DEBUG, "gatt", "characteristic does not support \"read\"");
    ReportReadValueError(ToResult(HostError::kNotSupported),
                         std::move(callback));
    return;
  }

  if (max_bytes == 0) {
    bt_log(TRACE, "gatt", "invalid value for |max_bytes|: 0");
    ReportReadValueError(ToResult(HostError::kInvalidParameters),
                         std::move(callback));
    return;
  }

  // Set up the buffer in which we'll accumulate the blobs.
  auto buffer = NewBuffer(std::min(max_bytes, att::kMaxAttributeValueLength));
  if (!buffer) {
    ReportReadValueError(ToResult(HostError::kOutOfMemory),
                         std::move(callback));
    return;
  }

  ReadLongHelper(chrc->info().value_handle,
                 offset,
                 std::move(buffer),
                 0u /* bytes_read */,
                 std::move(callback));
}

void RemoteService::ReadByType(const UUID& type, ReadByTypeCallback callback) {
  // Caller should not request a UUID of an internal attribute (e.g. service
  // declaration).
  if (IsInternalUuid(type)) {
    bt_log(TRACE,
           "gatt",
           "ReadByType called with internal GATT type (type: %s)",
           bt_str(type));
    callback(ToResult(HostError::kInvalidParameters), {});
    return;
  }

  // Read range is entire service range.
  ReadByTypeHelper(type,
                   service_data_.range_start,
                   service_data_.range_end,
                   {},
                   std::move(callback));
}

void RemoteService::WriteCharacteristic(CharacteristicHandle id,
                                        std::vector<uint8_t> value,
                                        att::ResultFunction<> cb) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    cb(status);
    return;
  }

  if (!(chrc->info().properties & Property::kWrite)) {
    bt_log(DEBUG, "gatt", "characteristic does not support \"write\"");
    cb(ToResult(HostError::kNotSupported));
    return;
  }

  client_->WriteRequest(chrc->info().value_handle,
                        BufferView(value.data(), value.size()),
                        std::move(cb));
}

void RemoteService::WriteLongCharacteristic(CharacteristicHandle id,
                                            uint16_t offset,
                                            std::vector<uint8_t> value,
                                            ReliableMode reliable_mode,
                                            att::ResultFunction<> callback) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    callback(status);
    return;
  }

  if (!(chrc->info().properties & Property::kWrite)) {
    bt_log(DEBUG, "gatt", "characteristic does not support \"write\"");
    callback(ToResult(HostError::kNotSupported));
    return;
  }

  if ((reliable_mode == ReliableMode::kEnabled) &&
      ((!chrc->extended_properties().has_value()) ||
       (!(chrc->extended_properties().value() &
          ExtendedProperty::kReliableWrite)))) {
    bt_log(DEBUG,
           "gatt",
           "characteristic does not support \"reliable write\"; attempting "
           "request anyway");
  }

  SendLongWriteRequest(chrc->info().value_handle,
                       offset,
                       BufferView(value.data(), value.size()),
                       reliable_mode,
                       std::move(callback));
}

void RemoteService::WriteCharacteristicWithoutResponse(
    CharacteristicHandle id,
    std::vector<uint8_t> value,
    att::ResultFunction<> cb) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    cb(status);
    return;
  }

  if (!(chrc->info().properties &
        (Property::kWrite | Property::kWriteWithoutResponse))) {
    bt_log(DEBUG,
           "gatt",
           "characteristic does not support \"write without response\"");
    cb(ToResult(HostError::kNotSupported));
    return;
  }

  client_->WriteWithoutResponse(chrc->info().value_handle,
                                BufferView(value.data(), value.size()),
                                std::move(cb));
}

void RemoteService::ReadDescriptor(DescriptorHandle id,
                                   ReadValueCallback callback) {
  const DescriptorData* desc;
  fit::result status = GetDescriptor(id, &desc);
  PW_DCHECK(desc || status.is_error());
  if (status.is_error()) {
    ReportReadValueError(status, std::move(callback));
    return;
  }

  client_->ReadRequest(desc->handle, std::move(callback));
}

void RemoteService::ReadLongDescriptor(DescriptorHandle id,
                                       uint16_t offset,
                                       size_t max_bytes,
                                       ReadValueCallback callback) {
  const DescriptorData* desc;
  att::Result<> status = GetDescriptor(id, &desc);
  PW_DCHECK(desc || status.is_error());
  if (status.is_error()) {
    ReportReadValueError(status, std::move(callback));
    return;
  }

  if (max_bytes == 0) {
    bt_log(TRACE, "gatt", "invalid value for |max_bytes|: 0");
    ReportReadValueError(ToResult(HostError::kInvalidParameters),
                         std::move(callback));
    return;
  }

  // Set up the buffer in which we'll accumulate the blobs.
  auto buffer = NewBuffer(std::min(max_bytes, att::kMaxAttributeValueLength));
  if (!buffer) {
    ReportReadValueError(ToResult(HostError::kOutOfMemory),
                         std::move(callback));
    return;
  }

  ReadLongHelper(desc->handle,
                 offset,
                 std::move(buffer),
                 0u /* bytes_read */,
                 std::move(callback));
}

void RemoteService::WriteDescriptor(DescriptorHandle id,
                                    std::vector<uint8_t> value,
                                    att::ResultFunction<> callback) {
  const DescriptorData* desc;
  fit::result status = GetDescriptor(id, &desc);
  PW_DCHECK(desc || status.is_error());
  if (status.is_error()) {
    callback(status);
    return;
  }

  // Do not allow writing to internally reserved descriptors.
  if (desc->type == types::kClientCharacteristicConfig) {
    bt_log(DEBUG, "gatt", "writing to CCC descriptor not allowed");
    callback(ToResult(HostError::kNotSupported));
    return;
  }

  client_->WriteRequest(desc->handle,
                        BufferView(value.data(), value.size()),
                        std::move(callback));
}

void RemoteService::WriteLongDescriptor(DescriptorHandle id,
                                        uint16_t offset,
                                        std::vector<uint8_t> value,
                                        att::ResultFunction<> callback) {
  const DescriptorData* desc;
  fit::result status = GetDescriptor(id, &desc);
  PW_DCHECK(desc || status.is_error());
  if (status.is_error()) {
    callback(status);
    return;
  }

  // Do not allow writing to internally reserved descriptors.
  if (desc->type == types::kClientCharacteristicConfig) {
    bt_log(DEBUG, "gatt", "writing to CCC descriptor not allowed");
    callback(ToResult(HostError::kNotSupported));
    return;
  }

  // For writing long descriptors, reliable mode is not supported.
  auto mode = ReliableMode::kDisabled;
  SendLongWriteRequest(desc->handle,
                       offset,
                       BufferView(value.data(), value.size()),
                       mode,
                       std::move(callback));
}

void RemoteService::EnableNotifications(CharacteristicHandle id,
                                        ValueCallback callback,
                                        NotifyStatusCallback status_callback) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_error()) {
    status_callback(status, kInvalidId);
    return;
  }

  chrc->EnableNotifications(std::move(callback), std::move(status_callback));
}

void RemoteService::DisableNotifications(
    CharacteristicHandle id,
    IdType handler_id,
    att::ResultFunction<> status_callback) {
  RemoteCharacteristic* chrc;
  fit::result status = GetCharacteristic(id, &chrc);
  PW_DCHECK(chrc || status.is_error());
  if (status.is_ok() && !chrc->DisableNotifications(handler_id)) {
    status = ToResult(HostError::kNotFound);
  }
  status_callback(status);
}

void RemoteService::StartDescriptorDiscovery() {
  PW_DCHECK(!pending_discov_reqs_.empty());

  PW_CHECK(!characteristics_.empty());
  remaining_descriptor_requests_ = characteristics_.size();

  auto self = GetWeakPtr();

  // Callback called for each characteristic. This may be called in any
  // order since we request the descriptors of all characteristics all at
  // once.
  auto desc_done_callback = [self](att::Result<> status) {
    if (!self.is_alive()) {
      return;
    }

    // Do nothing if discovery was concluded earlier (which would have cleared
    // the pending discovery requests).
    if (self->pending_discov_reqs_.empty()) {
      return;
    }

    if (status.is_ok()) {
      self->remaining_descriptor_requests_ -= 1;

      // Defer handling
      if (self->remaining_descriptor_requests_ > 0) {
        return;
      }

      // HasCharacteristics() should return true now.
      PW_DCHECK(self->HasCharacteristics());

      // Fall through and notify clients below.
    } else {
      PW_DCHECK(!self->HasCharacteristics());
      bt_log(DEBUG, "gatt", "descriptor discovery failed %s", bt_str(status));
      self->characteristics_.clear();

      // Fall through and notify the clients below.
    }

    self->CompleteCharacteristicDiscovery(status);
  };

  // Characteristics are stored in an (ordered) std::map by value_handle, so we
  // iterate in order; according to the spec (BT 5.0 Vol 3, part G, 3.3), the
  // value handle must appear immediately after the characteristic handle so the
  // handles are also guaranteed to be in order. Therefore we can use the next
  // in the iteration to calculate the handle range.
  for (auto iter = characteristics_.begin(); iter != characteristics_.end();
       ++iter) {
    auto next = iter;
    ++next;
    att::Handle end_handle;
    if (next == characteristics_.end()) {
      end_handle = service_data_.range_end;
    } else {
      end_handle = next->second.info().handle - 1;
    }

    PW_DCHECK(client_.is_alive());
    iter->second.DiscoverDescriptors(end_handle, desc_done_callback);

    if (!self.is_alive() || self->characteristics_.empty()) {
      return;
    }
  }
}

fit::result<Error<>> RemoteService::GetCharacteristic(
    CharacteristicHandle id, RemoteCharacteristic** out_char) {
  PW_DCHECK(out_char);

  if (!HasCharacteristics()) {
    *out_char = nullptr;
    return fit::error(HostError::kNotReady);
  }

  auto chr = characteristics_.find(id);
  if (chr == characteristics_.end()) {
    *out_char = nullptr;
    return fit::error(HostError::kNotFound);
  }

  *out_char = &chr->second;
  return fit::ok();
}

fit::result<Error<>> RemoteService::GetDescriptor(
    DescriptorHandle id, const DescriptorData** out_desc) {
  PW_DCHECK(out_desc);

  if (!HasCharacteristics()) {
    *out_desc = nullptr;
    return fit::error(HostError::kNotReady);
  }

  for (auto iter = characteristics_.begin(); iter != characteristics_.end();
       ++iter) {
    auto next = iter;
    ++next;
    if (next == characteristics_.end() ||
        next->second.info().handle > id.value) {
      const auto& descriptors = iter->second.descriptors();
      auto desc = descriptors.find(id);
      if (desc != descriptors.end()) {
        *out_desc = &desc->second;
        return fit::ok();
      }
    }
  }

  *out_desc = nullptr;
  return fit::error(HostError::kNotFound);
}

void RemoteService::CompleteCharacteristicDiscovery(att::Result<> status) {
  PW_DCHECK(!pending_discov_reqs_.empty());
  PW_DCHECK(status.is_error() || remaining_descriptor_requests_ == 0u);

  // We return a new copy of only the immutable data of our characteristics and
  // their descriptors. This requires a copy, which *could* be expensive in the
  // (unlikely) case that a service has a very large number of characteristics.
  CharacteristicMap characteristic_map =
      CharacteristicsToCharacteristicMap(characteristics_);

  auto pending = std::move(pending_discov_reqs_);
  for (auto& discovery_req_cb : pending) {
    discovery_req_cb(status, characteristic_map);
  }
}

void RemoteService::SendLongWriteRequest(att::Handle handle,
                                         uint16_t offset,
                                         BufferView value,
                                         ReliableMode reliable_mode,
                                         att::ResultFunction<> final_cb) {
  att::PrepareWriteQueue long_write_queue;
  auto header_ln = sizeof(att::PrepareWriteRequestParams) + sizeof(att::OpCode);
  uint16_t bytes_written = 0;

  // Divide up the long write into it's constituent PreparedWrites and add them
  // to the queue.
  while (bytes_written < value.size()) {
    uint16_t part_value_size = static_cast<uint16_t>(
        std::min(client_->mtu() - header_ln, value.size() - bytes_written));
    auto part_buffer = value.view(bytes_written, part_value_size);

    long_write_queue.push(att::QueuedWrite(handle, offset, part_buffer));

    bytes_written += part_value_size;
    offset += part_value_size;
  }

  client_->ExecutePrepareWrites(std::move(long_write_queue),
                                std::move(reliable_mode),
                                std::move(final_cb));
}

void RemoteService::ReadLongHelper(att::Handle value_handle,
                                   uint16_t offset,
                                   MutableByteBufferPtr out_buffer,
                                   size_t bytes_read,
                                   ReadValueCallback callback) {
  PW_DCHECK(callback);
  PW_DCHECK(out_buffer);

  auto self = GetWeakPtr();
  auto read_cb = [self,
                  value_handle,
                  offset,
                  buffer = std::move(out_buffer),
                  bytes_read,
                  cb = std::move(callback)](
                     att::Result<> status,
                     const ByteBuffer& blob,
                     bool maybe_truncated_by_mtu) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (status.is_error()) {
      // "If the Characteristic Value is not longer than (ATT_MTU – 1) an
      // ATT_ERROR_RSP PDU with the error code set to kAttributeNotLong shall be
      // received on the first ATT_READ_BLOB_REQ PDU." (Core Spec v5.2, Vol 3,
      // Part G, Sec 4.8.3). Report the short value read in the previous
      // ATT_READ_REQ in this case.
      if (status.error_value().is(att::ErrorCode::kAttributeNotLong) &&
          offset == self->client_->mtu() - sizeof(att::OpCode)) {
        cb(fit::ok(),
           buffer->view(0, bytes_read),
           /*maybe_truncated=*/false);
        return;
      }

      ReportReadValueError(status, std::move(cb));
      return;
    }

    // Copy the blob into our |buffer|. |blob| may be truncated depending on the
    // size of |buffer|.
    PW_CHECK(bytes_read < buffer->size());
    size_t copy_size = std::min(blob.size(), buffer->size() - bytes_read);
    bool truncated_by_max_bytes = (blob.size() != copy_size);
    buffer->Write(blob.view(0, copy_size), bytes_read);
    bytes_read += copy_size;

    // We are done if the read was not truncated by the MTU or we have read the
    // maximum number of bytes requested.
    PW_CHECK(bytes_read <= buffer->size());
    if (!maybe_truncated_by_mtu || bytes_read == buffer->size()) {
      cb(fit::ok(),
         buffer->view(0, bytes_read),
         maybe_truncated_by_mtu || truncated_by_max_bytes);
      return;
    }

    // We have more bytes to read. Read the next blob.
    self->ReadLongHelper(value_handle,
                         static_cast<uint16_t>(offset + blob.size()),
                         std::move(buffer),
                         bytes_read,
                         std::move(cb));
  };

  // "To read the complete Characteristic Value an ATT_READ_REQ PDU should be
  // used for the first part of the value and ATT_READ_BLOB_REQ PDUs shall used
  // for the rest." (Core Spec v5.2, Vol 3, part G, Sec 4.8.3).
  if (offset == 0) {
    client_->ReadRequest(value_handle, std::move(read_cb));
    return;
  }

  client_->ReadBlobRequest(value_handle, offset, std::move(read_cb));
}

void RemoteService::ReadByTypeHelper(
    const UUID& type,
    att::Handle start,
    att::Handle end,
    std::vector<RemoteService::ReadByTypeResult> results,
    ReadByTypeCallback callback) {
  if (start > end) {
    callback(fit::ok(), std::move(results));
    return;
  }

  auto read_cb = [self = GetWeakPtr(),
                  type,
                  start,
                  end,
                  values_accum = std::move(results),
                  cb = std::move(callback)](
                     Client::ReadByTypeResult result) mutable {
    if (!self.is_alive()) {
      return;
    }

    // Pass results to client when this goes out of scope.
    auto deferred_cb =
        fit::defer_callback([&]() { cb(fit::ok(), std::move(values_accum)); });
    if (result.is_error()) {
      const att::Error& error = result.error_value().error;
      deferred_cb = [&cb, error] { cb(fit::error(error), /*values=*/{}); };
      if (error.is(att::ErrorCode::kAttributeNotFound)) {
        // Treat kAttributeNotFound error as success, since it's used to
        // indicate when a sequence of reads has successfully read all matching
        // attributes.
        deferred_cb = [&cb, &values_accum]() {
          cb(fit::ok(), std::move(values_accum));
        };
        return;
      }
      if (error.is_any_of(att::ErrorCode::kRequestNotSupported,
                          att::ErrorCode::kInsufficientResources,
                          att::ErrorCode::kInvalidPDU)) {
        // Pass up these protocol errors as they aren't handle specific or
        // recoverable.
        return;
      }
      if (error.is_protocol_error()) {
        // Other errors may correspond to reads of specific handles, so treat
        // them as a result and continue reading after the error.

        // A handle must be provided and in the requested read handle range.
        if (!result.error_value().handle.has_value()) {
          return;
        }
        att::Handle error_handle = result.error_value().handle.value();
        if (error_handle < start || error_handle > end) {
          deferred_cb = [&cb] {
            cb(fit::error(Error(HostError::kPacketMalformed)), /*values=*/{});
          };
          return;
        }

        values_accum.push_back(
            RemoteService::ReadByTypeResult{CharacteristicHandle(error_handle),
                                            fit::error(error.protocol_error()),
                                            /*maybe_truncated=*/false});

        // Do not attempt to read from the next handle if the error handle is
        // the max handle, as this would cause an overflow.
        if (error_handle == std::numeric_limits<att::Handle>::max()) {
          deferred_cb = [&cb, &values_accum]() {
            cb(fit::ok(), std::move(values_accum));
          };
          return;
        }

        // Start next read right after attribute causing error.
        att::Handle start_next = error_handle + 1;

        self->ReadByTypeHelper(
            type, start_next, end, std::move(values_accum), std::move(cb));
        deferred_cb.cancel();
        return;
      }
      return;
    }

    const std::vector<Client::ReadByTypeValue>& values = result.value();
    // Client already checks for invalid response where status is success but no
    // values are returned.
    PW_CHECK(!values.empty());

    // Convert and accumulate values.
    for (const auto& val : values) {
      auto buffer = NewBuffer(val.value.size());
      val.value.Copy(buffer.get());
      values_accum.push_back(ReadByTypeResult{CharacteristicHandle(val.handle),
                                              fit::ok(std::move(buffer)),
                                              val.maybe_truncated});
    }

    // Do not attempt to read from the next handle if the last value handle is
    // the max handle, as this would cause an overflow.
    if (values.back().handle == std::numeric_limits<att::Handle>::max()) {
      return;
    }

    // Start next read right after last returned attribute. Client already
    // checks that value handles are ascending and in range, so we are
    // guaranteed to make progress.
    att::Handle start_next = values.back().handle + 1;

    self->ReadByTypeHelper(
        type, start_next, end, std::move(values_accum), std::move(cb));
    deferred_cb.cancel();
  };
  client_->ReadByTypeRequest(type, start, end, std::move(read_cb));
}

void RemoteService::HandleNotification(att::Handle value_handle,
                                       const ByteBuffer& value,
                                       bool maybe_truncated) {
  auto iter = characteristics_.find(CharacteristicHandle(value_handle));
  if (iter != characteristics_.end()) {
    iter->second.HandleNotification(value, maybe_truncated);
  }
}

}  // namespace bt::gatt
