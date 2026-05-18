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

#include "pw_bluetooth_sapphire/internal/host/sdp/pdu.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <memory>

#include "public/pw_bluetooth_sapphire/internal/host/sdp/pdu.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/packet_view.h"
#include "pw_bluetooth_sapphire/internal/host/common/slab_allocator.h"

namespace bt::sdp {

namespace {

// Min size is Sequence uint8 (2 bytes) + uint16_t (3 bytes)
// See description of AttributeIDList in ServiceAttribute transaction
// Spec v5.0, Vol 3, Part B, Sec 4.6.1
constexpr size_t kMinAttributeIDListBytes = 5;

// The maximum amount of services allowed in a service search.
// Spec v5.0, Vol 3, Part B, Sec 4.5.1
constexpr size_t kMaxServiceSearchSize = 12;

// The maximum amount of Attribute list data we will store when parsing a
// response to ServiceAttribute or ServiceSearchAttribute responses. 640kb ought
// to be enough for anybody.
constexpr size_t kMaxSupportedAttributeListBytes = 655360;

// Validates continuation state in |buf|, which should be the configuration
// state bytes of a PDU.
// Returns true if the continuation state is valid here, false otherwise.
// Sets |out| to point to it if present and valid.
bool ValidContinuationState(const ByteBuffer& buf, BufferView* out) {
  PW_DCHECK(out);
  if (buf.size() == 0) {
    return false;
  }
  uint8_t len = buf[0];
  if (len == 0) {
    *out = BufferView();
    return true;
  }
  if (len >= kMaxContStateLength || len > (buf.size() - 1)) {
    return false;
  }
  *out = buf.view(1, len);
  return true;
}

MutableByteBufferPtr NewSdpBuffer(size_t buffer_size) {
  // TODO(fxbug.dev/42083692): Remove unique_ptr->DynamicByteBuffer double
  // indirection once sufficient progress has been made on the attached bug
  // (specifically re:l2cap::Channel::Send).
  return std::make_unique<DynamicByteBuffer>(buffer_size);
}

MutableByteBufferPtr BuildNewPdu(OpCode pdu_id,
                                 TransactionId tid,
                                 uint16_t param_length) {
  MutableByteBufferPtr ptr = NewSdpBuffer(sizeof(Header) + param_length);
  MutablePacketView<Header> packet(ptr.get(), param_length);
  packet.mutable_header()->pdu_id = pdu_id;
  packet.mutable_header()->tid =
      pw::bytes::ConvertOrderTo(cpp20::endian::big, tid);
  packet.mutable_header()->param_length =
      pw::bytes::ConvertOrderTo(cpp20::endian::big, param_length);
  return ptr;
}

// Parses an Attribute ID List sequence where every element is either:
// - 16-bit unsigned integer representing a specific Attribute ID
// - 32-bit unsigned integer which the high order 16-bits represent a
//   beginning attribute ID and the low order 16-bits represent a
//   ending attribute ID of a range.
// Returns the number of bytes taken by the list, or zero if an error
// occurred (wrong order, wrong format, too many elements).
size_t ReadAttributeIDList(const ByteBuffer& buf,
                           std::list<AttributeRange>* attribute_ranges,
                           size_t max_elements) {
  DataElement attribute_list_elem;
  size_t elem_size = DataElement::Read(&attribute_list_elem, buf);
  if ((elem_size == 0) ||
      (attribute_list_elem.type() != DataElement::Type::kSequence)) {
    bt_log(TRACE, "sdp", "failed to parse attribute ranges, or not a sequence");
    attribute_ranges->clear();
    return 0;
  }
  size_t elem_count = *attribute_list_elem.ElementCount();
  if (elem_count > max_elements) {
    bt_log(WARN,
           "sdp",
           "too many attribute list elements in list: %zu > %zu",
           elem_count,
           max_elements);
    attribute_ranges->clear();
    return 0;
  }
  uint16_t last_attr = 0x0000;
  const DataElement* it = attribute_list_elem.At(0);
  for (size_t i = 0; it != nullptr; it = attribute_list_elem.At(++i)) {
    if (it->type() != DataElement::Type::kUnsignedInt) {
      bt_log(TRACE, "sdp", "attribute range sequence invalid element type");
      attribute_ranges->clear();
      return 0;
    }
    if (it->size() == DataElement::Size::kTwoBytes) {
      uint16_t single_attr_id = *(it->Get<uint16_t>());
      if (single_attr_id < last_attr) {
        attribute_ranges->clear();
        return 0;
      }
      attribute_ranges->emplace_back(single_attr_id, single_attr_id);
      last_attr = single_attr_id;
    } else if (it->size() == DataElement::Size::kFourBytes) {
      uint32_t attr_range = *(it->Get<uint32_t>());
      uint16_t start_id = attr_range >> 16;
      uint16_t end_id = attr_range & 0xFFFF;
      if ((start_id < last_attr) || (end_id < start_id)) {
        attribute_ranges->clear();
        return 0;
      }
      attribute_ranges->emplace_back(start_id, end_id);
      last_attr = end_id;
    } else {
      attribute_ranges->clear();
      return 0;
    }
  }
  return elem_size;
}

void AddToAttributeRanges(std::list<AttributeRange>* ranges,
                          AttributeId start,
                          AttributeId end) {
  auto it = ranges->begin();
  // Put the range in the list (possibly overlapping other ranges), with the
  // start in order.
  for (; it != ranges->end(); ++it) {
    if (start < it->start) {
      // This is where it should go.
      ranges->emplace(it, start, end);
    }
  }
  if (it == ranges->end()) {
    // It must be on the end.
    ranges->emplace_back(start, end);
  }
  // Merge any overlapping or adjacent ranges with no gaps.
  for (it = ranges->begin(); it != ranges->end();) {
    auto next = it;
    next++;
    if (next == ranges->end()) {
      return;
    }
    if (it->end >= (next->start - 1)) {
      next->start = it->start;
      if (next->end < it->end) {
        next->end = it->end;
      }
      it = ranges->erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

Request::Request() { cont_state_.Fill(0); }

void Request::SetContinuationState(const ByteBuffer& buf) {
  PW_DCHECK(buf.size() < kMaxContStateLength);
  cont_state_[0] = static_cast<uint8_t>(buf.size());
  if (cont_state_[0] == 0) {
    return;
  }
  auto v = cont_state_.mutable_view(sizeof(uint8_t));
  buf.Copy(&v);
}

bool Request::ParseContinuationState(const ByteBuffer& buf) {
  BufferView view;
  if (!ValidContinuationState(buf, &view)) {
    return false;
  }
  SetContinuationState(view);
  return true;
}

size_t Request::WriteContinuationState(MutableByteBuffer* buf) const {
  PW_DCHECK(buf->size() > cont_info_size());
  size_t written_size = sizeof(uint8_t) + cont_info_size();
  buf->Write(cont_state_.view(0, written_size));
  return written_size;
}

fit::result<Error<>> ErrorResponse::Parse(const ByteBuffer& buf) {
  if (complete()) {
    return ToResult(HostError::kNotReady);
  }
  if (buf.size() != sizeof(ErrorCode)) {
    return ToResult(HostError::kPacketMalformed);
  }
  error_code_ = ErrorCode(
      pw::bytes::ConvertOrderFrom(cpp20::endian::big, buf.To<uint16_t>()));
  return fit::ok();
}

MutableByteBufferPtr ErrorResponse::GetPDU(uint16_t,
                                           TransactionId tid,
                                           uint16_t,
                                           const ByteBuffer&) const {
  if (!complete()) {
    return nullptr;
  }
  auto ptr = BuildNewPdu(kErrorResponse, tid, sizeof(ErrorCode));
  size_t written = sizeof(Header);

  ptr->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big,
                                static_cast<uint16_t>(error_code_.value())),
      written);

  return ptr;
}

ServiceSearchRequest::ServiceSearchRequest()
    : Request(), max_service_record_count_(0xFFFF) {}

ServiceSearchRequest::ServiceSearchRequest(const ByteBuffer& params)
    : ServiceSearchRequest() {
  DataElement search_pattern;
  size_t read_size = DataElement::Read(&search_pattern, params);
  if ((read_size == 0) ||
      (search_pattern.type() != DataElement::Type::kSequence)) {
    bt_log(TRACE, "sdp", "Failed to read search pattern");
    return;
  }
  size_t min_size = read_size + sizeof(uint16_t) + sizeof(uint8_t);
  if (params.size() < min_size) {
    bt_log(
        TRACE, "sdp", "Params too small: %zu < %zu", params.size(), min_size);
    return;
  }
  const DataElement* it;
  size_t count;
  for (count = 0, it = search_pattern.At(count); it != nullptr;
       it = search_pattern.At(++count)) {
    if ((count >= kMaxServiceSearchSize) ||
        (it->type() != DataElement::Type::kUuid)) {
      bt_log(TRACE, "sdp", "Search pattern invalid: wrong type or too many");
      service_search_pattern_.clear();
      return;
    }
    service_search_pattern_.emplace(*(it->Get<UUID>()));
  }
  if (count == 0) {
    bt_log(TRACE, "sdp", "Search pattern invalid: no records");
    return;
  }
  max_service_record_count_ = pw::bytes::ConvertOrderFrom(
      cpp20::endian::big, params.view(read_size).To<uint16_t>());
  // Max returned count must be 0x0001-0xFFFF (Spec Vol 3, Part B, 4.5.1)
  if (max_service_record_count_ == 0) {
    bt_log(TRACE, "sdp", "Search invalid: max record count must be > 0");
    return;
  }
  read_size += sizeof(uint16_t);
  if (!ParseContinuationState(params.view(read_size))) {
    service_search_pattern_.clear();
    return;
  }
  PW_DCHECK(valid());
}

bool ServiceSearchRequest::valid() const {
  return max_service_record_count_ > 0 && service_search_pattern_.size() > 0 &&
         service_search_pattern_.size() <= kMaxServiceSearchSize;
}

ByteBufferPtr ServiceSearchRequest::GetPDU(TransactionId tid) const {
  if (!valid()) {
    return nullptr;
  }

  // MaximumServiceRecordCount + continuation state count + continuation state
  // size
  uint16_t size = sizeof(uint16_t) + sizeof(uint8_t) + cont_info_size();

  std::vector<DataElement> pattern(service_search_pattern_.size());
  size_t i = 0;
  for (auto& it : service_search_pattern_) {
    pattern.at(i).Set(it);
    i++;
  }
  DataElement search_pattern(std::move(pattern));

  // The maximum number of service UUIDs in the service is 12 (checked by
  // valid() above), so |size| shouldn't be anywhere near the max PDU size.
  size += search_pattern.WriteSize();

  auto buf = BuildNewPdu(kServiceSearchRequest, tid, size);
  size_t written = sizeof(Header);

  // Write ServiceSearchPattern
  auto write_view = buf->mutable_view(written);
  written += search_pattern.Write(&write_view);
  // Write MaxServiceRecordCount
  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, max_service_record_count_),
      written);
  written += sizeof(uint16_t);
  // Write Continuation State
  write_view = buf->mutable_view(written);
  written += WriteContinuationState(&write_view);

  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

ServiceSearchResponse::ServiceSearchResponse()
    : total_service_record_count_(0) {}

bool ServiceSearchResponse::complete() const {
  return total_service_record_count_ == service_record_handle_list_.size();
}

const BufferView ServiceSearchResponse::ContinuationState() const {
  if (!continuation_state_) {
    return BufferView();
  }
  return continuation_state_->view();
}

fit::result<Error<>> ServiceSearchResponse::Parse(const ByteBuffer& buf) {
  if (complete() && total_service_record_count_ != 0) {
    // This response was previously complete and non-empty.
    bt_log(TRACE, "sdp", "Can't parse into a complete response");
    return ToResult(HostError::kNotReady);
  }
  if (buf.size() < (2 * sizeof(uint16_t))) {
    bt_log(TRACE, "sdp", "Packet too small to parse");
    return ToResult(HostError::kPacketMalformed);
  }

  uint16_t total_service_record_count =
      pw::bytes::ConvertOrderFrom(cpp20::endian::big, buf.To<uint16_t>());
  size_t read_size = sizeof(uint16_t);
  if (total_service_record_count_ != 0 &&
      total_service_record_count_ != total_service_record_count) {
    bt_log(TRACE, "sdp", "Continuing packet has different record count");
    return ToResult(HostError::kPacketMalformed);
  }
  total_service_record_count_ = total_service_record_count;

  uint16_t record_count = pw::bytes::ConvertOrderFrom(
      cpp20::endian::big, buf.view(read_size).To<uint16_t>());
  read_size += sizeof(uint16_t);
  size_t expected_record_bytes = sizeof(ServiceHandle) * record_count;
  if (buf.size() < (read_size + expected_record_bytes)) {
    bt_log(TRACE,
           "sdp",
           "Packet too small for %d records: %zu",
           record_count,
           buf.size());
    return ToResult(HostError::kPacketMalformed);
  }
  BufferView cont_state_view;
  if (!ValidContinuationState(buf.view(read_size + expected_record_bytes),
                              &cont_state_view)) {
    bt_log(TRACE, "sdp", "Failed to find continuation state");
    return ToResult(HostError::kPacketMalformed);
  }
  size_t expected_size = read_size + expected_record_bytes +
                         cont_state_view.size() + sizeof(uint8_t);
  if (expected_size != buf.size()) {
    bt_log(TRACE,
           "sdp",
           "Packet should be %zu not %zu",
           expected_size,
           buf.size());
    return ToResult(HostError::kPacketMalformed);
  }

  for (uint16_t i = 0; i < record_count; i++) {
    auto view = buf.view(read_size + i * sizeof(ServiceHandle));
    service_record_handle_list_.emplace_back(
        pw::bytes::ConvertOrderFrom(cpp20::endian::big, view.To<uint32_t>()));
  }
  if (cont_state_view.size() == 0) {
    continuation_state_ = nullptr;
  } else {
    continuation_state_ = NewBuffer(cont_state_view.size());
    continuation_state_->Write(cont_state_view);
    return ToResult(HostError::kInProgress);
  }
  return fit::ok();
}

// Continuation state: Index of the start record for the continued response.
MutableByteBufferPtr ServiceSearchResponse::GetPDU(
    uint16_t req_max,
    TransactionId tid,
    uint16_t max_size,
    const ByteBuffer& cont_state) const {
  if (!complete()) {
    return nullptr;
  }
  uint16_t start_idx = 0;
  if (cont_state.size() == sizeof(uint16_t)) {
    start_idx = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                            cont_state.To<uint16_t>());
  } else if (cont_state.size() != 0) {
    // We don't generate continuation state of any other length.
    return nullptr;
  }

  uint16_t response_record_count = total_service_record_count_;
  if (req_max < response_record_count) {
    bt_log(TRACE,
           "sdp",
           "Limit ServiceSearchResponse to %d/%d records",
           req_max,
           response_record_count);
    response_record_count = req_max;
  }

  if (cont_state.size() > 0 && response_record_count <= start_idx) {
    // Invalid continuation state, out of range.
    return nullptr;
  }

  uint16_t current_record_count = response_record_count - start_idx;

  // Minimum size is zero records with no continuation state.
  // Header + TotalServiceRecordCount(uint16_t) +
  // CurrentServiceRecordCount(uint16_t) + ContinuationState count (uint8_t)
  constexpr uint16_t min_size =
      sizeof(Header) + (2 * sizeof(uint16_t)) + sizeof(uint8_t);

  if (max_size < min_size) {
    // Can't generate a PDU, it's too small to hold even no records.
    return nullptr;
  }

  // The most records we can send in a packet of max_size (including a
  // continuation and Header)
  const uint16_t max_records = (max_size - min_size) / sizeof(ServiceHandle);

  uint8_t info_length = 0;
  if (max_records < current_record_count) {
    bt_log(TRACE,
           "sdp",
           "Max Size limits to %hu/%d records",
           max_records,
           current_record_count);
    current_record_count = max_records;
    info_length = sizeof(uint16_t);
  }

  // Note: we exclude Header from size here
  // TotalServiceRecordCount(uint16_t) + CurrentServiceRecordCount(uint16_t) +
  // ServiceRecordHandleList (CurrentServiceRecordCount * ServiceHandle) +
  // ContinuationState count (uint8_t)
  const uint16_t size = (2 * sizeof(uint16_t)) +
                        (current_record_count * sizeof(ServiceHandle)) +
                        sizeof(uint8_t) + info_length;

  auto buf = BuildNewPdu(kServiceSearchResponse, tid, size);
  if (!buf) {
    return buf;
  }
  PW_CHECK(buf->size() <= max_size);

  size_t written = sizeof(Header);
  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, response_record_count),
      written);
  written += sizeof(uint16_t);
  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, current_record_count),
      written);
  written += sizeof(uint16_t);

  for (size_t i = 0; i < current_record_count; i++) {
    buf->WriteObj(pw::bytes::ConvertOrderTo(
                      cpp20::endian::big,
                      static_cast<uint32_t>(
                          service_record_handle_list_.at(start_idx + i))),
                  written);
    written += sizeof(ServiceHandle);
  }

  // Continuation state
  buf->WriteObj(info_length, written);
  written += sizeof(uint8_t);
  if (info_length > 0) {
    start_idx += current_record_count;
    buf->WriteObj(pw::bytes::ConvertOrderTo(cpp20::endian::big, start_idx),
                  written);
    written += sizeof(uint16_t);
  }
  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

ServiceAttributeRequest::ServiceAttributeRequest()
    : service_record_handle_(0), max_attribute_byte_count_(0xFFFF) {}

ServiceAttributeRequest::ServiceAttributeRequest(const ByteBuffer& params) {
  if (params.size() < sizeof(uint32_t) + sizeof(uint16_t)) {
    bt_log(TRACE, "sdp", "packet too small for ServiceAttributeRequest");
    max_attribute_byte_count_ = 0;
    return;
  }

  service_record_handle_ =
      pw::bytes::ConvertOrderFrom(cpp20::endian::big, params.To<uint32_t>());
  size_t read_size = sizeof(uint32_t);
  max_attribute_byte_count_ = pw::bytes::ConvertOrderFrom(
      cpp20::endian::big, params.view(read_size).To<uint16_t>());
  if (max_attribute_byte_count_ < kMinMaximumAttributeByteCount) {
    bt_log(TRACE,
           "sdp",
           "max attribute byte count too small (%hu < %zu)",
           max_attribute_byte_count_,
           kMinMaximumAttributeByteCount);
    return;
  }
  read_size += sizeof(uint16_t);

  size_t elem_size = ReadAttributeIDList(
      params.view(read_size), &attribute_ranges_, kMaxAttributeRangesInRequest);
  if (attribute_ranges_.size() == 0) {
    max_attribute_byte_count_ = 0;
    return;
  }
  read_size += elem_size;

  if (!ParseContinuationState(params.view(read_size))) {
    attribute_ranges_.clear();
    return;
  }
  PW_DCHECK(valid());
}

bool ServiceAttributeRequest::valid() const {
  return (max_attribute_byte_count_ >= kMinMaximumAttributeByteCount) &&
         (attribute_ranges_.size() > 0) &&
         (attribute_ranges_.size() <= kMaxAttributeRangesInRequest);
}

ByteBufferPtr ServiceAttributeRequest::GetPDU(TransactionId tid) const {
  if (!valid()) {
    return nullptr;
  }

  size_t size = sizeof(ServiceHandle) + sizeof(uint16_t) + sizeof(uint8_t) +
                cont_info_size();

  std::vector<DataElement> attribute_list(attribute_ranges_.size());
  size_t idx = 0;
  for (const auto& it : attribute_ranges_) {
    if (it.start == it.end) {
      attribute_list.at(idx).Set<uint16_t>(it.start);
    } else {
      uint32_t attr_range = (static_cast<uint32_t>(it.start) << 16);
      attr_range |= it.end;
      attribute_list.at(idx).Set<uint32_t>(attr_range);
    }
    idx++;
  }

  DataElement attribute_list_elem(std::move(attribute_list));
  size += attribute_list_elem.WriteSize();

  // valid() ensures that |size| can't overflow due to too many attribute
  // ranges.
  PW_DCHECK(size <= std::numeric_limits<uint16_t>::max());
  auto buf =
      BuildNewPdu(kServiceAttributeRequest, tid, static_cast<uint16_t>(size));

  size_t written = sizeof(Header);

  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, service_record_handle_),
      written);
  written += sizeof(uint32_t);

  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, max_attribute_byte_count_),
      written);
  written += sizeof(uint16_t);

  auto mut_view = buf->mutable_view(written);
  written += attribute_list_elem.Write(&mut_view);

  mut_view = buf->mutable_view(written);
  written += WriteContinuationState(&mut_view);
  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

void ServiceAttributeRequest::AddAttribute(AttributeId id) {
  AddToAttributeRanges(&attribute_ranges_, id, id);
}

void ServiceAttributeRequest::AddAttributeRange(AttributeId start,
                                                AttributeId end) {
  AddToAttributeRanges(&attribute_ranges_, start, end);
}

ServiceAttributeResponse::ServiceAttributeResponse() {}

const BufferView ServiceAttributeResponse::ContinuationState() const {
  if (!continuation_state_) {
    return BufferView();
  }
  return continuation_state_->view();
}

bool ServiceAttributeResponse::complete() const { return !continuation_state_; }

fit::result<Error<>> ServiceAttributeResponse::Parse(const ByteBuffer& buf) {
  if (complete() && attributes_.size() != 0) {
    // This response was previously complete and non-empty
    bt_log(TRACE, "sdp", "Can't parse into a complete response");
    // partial_response_ is already empty
    return ToResult(HostError::kNotReady);
  }

  if (buf.size() < sizeof(uint16_t)) {
    bt_log(TRACE, "sdp", "Packet too small to parse");
    return ToResult(HostError::kPacketMalformed);
  }

  uint32_t attribute_list_byte_count =
      pw::bytes::ConvertOrderFrom(cpp20::endian::big, buf.To<uint16_t>());
  size_t read_size = sizeof(uint16_t);
  if (buf.size() < read_size + attribute_list_byte_count + sizeof(uint8_t)) {
    bt_log(TRACE, "sdp", "Not enough bytes in rest of packet");
    return ToResult(HostError::kPacketMalformed);
  }
  // Check to see if there's continuation.
  BufferView cont_state_view;
  if (!ValidContinuationState(buf.view(read_size + attribute_list_byte_count),
                              &cont_state_view)) {
    bt_log(TRACE, "sdp", "Continuation state is not valid");
    return ToResult(HostError::kPacketMalformed);
  }

  if (cont_state_view.size() == 0) {
    continuation_state_ = nullptr;
  } else {
    continuation_state_ = NewBuffer(cont_state_view.size());
    continuation_state_->Write(cont_state_view);
  }

  size_t expected_size = read_size + attribute_list_byte_count +
                         cont_state_view.size() + sizeof(uint8_t);
  if (buf.size() != expected_size) {
    bt_log(TRACE,
           "sdp",
           "Packet should be %zu not %zu",
           expected_size,
           buf.size());
    return ToResult(HostError::kPacketMalformed);
  }

  auto attribute_list_bytes = buf.view(read_size, attribute_list_byte_count);
  if (partial_response_ || ContinuationState().size()) {
    // Append to the incomplete buffer.
    size_t new_partial_size = attribute_list_byte_count;
    if (partial_response_) {
      new_partial_size += partial_response_->size();
    }
    // We currently don't support more than approx 10 packets of the max size.
    if (new_partial_size > kMaxSupportedAttributeListBytes) {
      bt_log(INFO,
             "sdp",
             "ServiceAttributeResponse exceeds supported size (%zu), dropping",
             new_partial_size);
      partial_response_ = nullptr;
      return ToResult(HostError::kNotSupported);
    }

    auto new_partial = NewBuffer(new_partial_size);
    if (partial_response_) {
      new_partial->Write(partial_response_->view());
      new_partial->Write(attribute_list_bytes, partial_response_->size());
    } else {
      new_partial->Write(attribute_list_bytes);
    }
    partial_response_ = std::move(new_partial);
    if (continuation_state_) {
      // This is incomplete, we can't parse it yet.
      bt_log(TRACE, "sdp", "Continuation state, returning in progress");
      return ToResult(HostError::kInProgress);
    }
    attribute_list_bytes = partial_response_->view();
  }

  DataElement attribute_list;
  size_t elem_size = DataElement::Read(&attribute_list, attribute_list_bytes);
  if ((elem_size == 0) ||
      (attribute_list.type() != DataElement::Type::kSequence)) {
    bt_log(
        TRACE, "sdp", "Couldn't parse attribute list or it wasn't a sequence");
    return ToResult(HostError::kPacketMalformed);
  }

  // Data Element sequence containing alternating attribute id and attribute
  // value pairs. Only the requested attributes that are present are included.
  // They are sorted in ascending attribute ID order.
  AttributeId last_id = 0;
  size_t idx = 0;
  for (auto* it = attribute_list.At(0); it != nullptr;
       it = attribute_list.At(idx)) {
    auto* val = attribute_list.At(idx + 1);
    std::optional<AttributeId> id = it->Get<uint16_t>();
    if (!id || (val == nullptr)) {
      attributes_.clear();
      return ToResult(HostError::kPacketMalformed);
    }
    if (*id < last_id) {
      attributes_.clear();
      return ToResult(HostError::kPacketMalformed);
    }
    attributes_.emplace(*id, val->Clone());
    last_id = *id;
    idx += 2;
  }
  return fit::ok();
}

// Continuation state: index of # of bytes into the attribute list element
MutableByteBufferPtr ServiceAttributeResponse::GetPDU(
    uint16_t req_max,
    TransactionId tid,
    uint16_t max_size,
    const ByteBuffer& cont_state) const {
  if (!complete()) {
    return nullptr;
  }
  // If there's continuation state, it's the # of bytes previously written
  // of the attribute list.
  uint32_t bytes_skipped = 0;
  if (cont_state.size() == sizeof(uint32_t)) {
    bytes_skipped = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                                cont_state.To<uint32_t>());
  } else if (cont_state.size() != 0) {
    // We don't generate continuation states of any other length.
    return nullptr;
  }

  // Returned in pairs of (attribute id, attribute value)
  std::vector<DataElement> list;
  list.reserve(2 * attributes_.size());
  for (const auto& it : attributes_) {
    list.emplace_back(static_cast<uint16_t>(it.first));
    list.emplace_back(it.second.Clone());
  }
  DataElement list_elem(std::move(list));

  size_t write_size = list_elem.WriteSize();

  if (bytes_skipped > write_size) {
    bt_log(TRACE,
           "sdp",
           "continuation out of range: %d > %zu",
           bytes_skipped,
           write_size);
    return nullptr;
  }

  // Minimum size is header, byte_count, 2 attribute bytes, and a zero length
  // continuation state
  constexpr uint16_t min_size =
      sizeof(Header) + sizeof(uint16_t) + 2 + sizeof(uint8_t);

  if (min_size > max_size) {
    // Can't make a PDU because we don't have enough space.
    return nullptr;
  }

  uint8_t info_length = 0;
  size_t attribute_list_byte_count = write_size - bytes_skipped;

  // Two attribute bytes counted in the min_size are excluded
  const uint16_t max_attribute_byte_count = max_size - min_size + 2;
  if (attribute_list_byte_count > max_attribute_byte_count) {
    info_length = sizeof(uint32_t);
    bt_log(TRACE,
           "sdp",
           "Max size limits attribute size to %hu of %zu",
           max_attribute_byte_count - info_length,
           attribute_list_byte_count);
    attribute_list_byte_count = max_attribute_byte_count - info_length;
  }

  if (attribute_list_byte_count > req_max) {
    bt_log(TRACE,
           "sdp",
           "Requested size limits attribute size to %d of %zu",
           req_max,
           attribute_list_byte_count);
    attribute_list_byte_count = req_max;
    info_length = sizeof(uint32_t);
  }

  // Casting to uint16_t is safe as attribute_list_byte_count was limited to
  // max_attribute_byte_count above.
  uint16_t size =
      static_cast<uint16_t>(sizeof(uint16_t) + attribute_list_byte_count +
                            sizeof(uint8_t) + info_length);
  auto buf = BuildNewPdu(kServiceAttributeResponse, tid, size);

  size_t written = sizeof(Header);

  buf->WriteObj(
      pw::bytes::ConvertOrderTo(
          cpp20::endian::big, static_cast<uint16_t>(attribute_list_byte_count)),
      written);
  written += sizeof(uint16_t);

  auto attribute_list_bytes = NewBuffer(write_size);
  list_elem.Write(attribute_list_bytes.get());
  buf->Write(
      attribute_list_bytes->view(bytes_skipped, attribute_list_byte_count),
      written);
  written += attribute_list_byte_count;

  // Continuation state
  buf->WriteObj(info_length, written);
  written += sizeof(uint8_t);
  if (info_length > 0) {
    bytes_skipped += attribute_list_byte_count;
    buf->WriteObj(pw::bytes::ConvertOrderTo(cpp20::endian::big, bytes_skipped),
                  written);
    written += sizeof(uint32_t);
  }
  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

ServiceSearchAttributeRequest::ServiceSearchAttributeRequest()
    : Request(), max_attribute_byte_count_(0xFFFF) {}

ServiceSearchAttributeRequest::ServiceSearchAttributeRequest(
    const ByteBuffer& params) {
  DataElement search_pattern;
  size_t read_size = DataElement::Read(&search_pattern, params);
  if ((read_size == 0) ||
      (search_pattern.type() != DataElement::Type::kSequence)) {
    bt_log(TRACE, "sdp", "failed to read search pattern");
    max_attribute_byte_count_ = 0;
    return;
  }
  // Minimum size is ServiceSearchPattern (varies, above) +
  // MaximumAttributeByteCount + AttributeIDList + Cont State (uint8)
  if (params.size() < read_size + sizeof(max_attribute_byte_count_) +
                          kMinAttributeIDListBytes + sizeof(uint8_t)) {
    bt_log(TRACE, "sdp", "packet too small for ServiceSearchAttributeRequest");
    max_attribute_byte_count_ = 0;
    return;
  }

  const DataElement* it;
  size_t count;
  for (count = 0, it = search_pattern.At(count); it != nullptr;
       it = search_pattern.At(++count)) {
    if ((count >= kMaxServiceSearchSize) ||
        (it->type() != DataElement::Type::kUuid)) {
      bt_log(TRACE, "sdp", "search pattern is invalid");
      service_search_pattern_.clear();
      return;
    }
    service_search_pattern_.emplace(*(it->Get<UUID>()));
  }
  if (count == 0) {
    bt_log(TRACE, "sdp", "no elements in search pattern");
    max_attribute_byte_count_ = 0;
    return;
  }

  max_attribute_byte_count_ = pw::bytes::ConvertOrderFrom(
      cpp20::endian::big, params.view(read_size).To<uint16_t>());
  if (max_attribute_byte_count_ < kMinMaximumAttributeByteCount) {
    bt_log(TRACE,
           "sdp",
           "max attribute byte count to small (%d)",
           max_attribute_byte_count_);
    max_attribute_byte_count_ = 0;
    return;
  }
  read_size += sizeof(uint16_t);

  size_t elem_size = ReadAttributeIDList(
      params.view(read_size), &attribute_ranges_, kMaxAttributeRangesInRequest);
  if (attribute_ranges_.size() == 0) {
    max_attribute_byte_count_ = 0;
    return;
  }
  read_size += elem_size;

  if (!ParseContinuationState(params.view(read_size))) {
    attribute_ranges_.clear();
    return;
  }

  bt_log(TRACE,
         "sdp",
         "parsed: %zu search uuids, %hu max bytes, %zu attribute ranges",
         service_search_pattern_.size(),
         max_attribute_byte_count_,
         attribute_ranges_.size());

  PW_DCHECK(valid());
}

bool ServiceSearchAttributeRequest::valid() const {
  return (max_attribute_byte_count_ >= kMinMaximumAttributeByteCount) &&
         (service_search_pattern_.size() > 0) &&
         (service_search_pattern_.size() <= kMaxServiceSearchSize) &&
         (attribute_ranges_.size() > 0) &&
         (attribute_ranges_.size() <= kMaxAttributeRangesInRequest);
}

ByteBufferPtr ServiceSearchAttributeRequest::GetPDU(TransactionId tid) const {
  if (!valid()) {
    return nullptr;
  }

  // Size of fixed length components: MaxAttributesByteCount, continuation info
  uint16_t size = sizeof(max_attribute_byte_count_) + cont_info_size() + 1;

  std::vector<DataElement> attribute_list(attribute_ranges_.size());
  size_t idx = 0;
  for (const auto& it : attribute_ranges_) {
    if (it.start == it.end) {
      attribute_list.at(idx).Set<uint16_t>(it.start);
    } else {
      uint32_t attr_range = (static_cast<uint32_t>(it.start) << 16);
      attr_range |= it.end;
      attribute_list.at(idx).Set<uint32_t>(attr_range);
    }
    idx++;
  }

  DataElement attribute_list_elem(std::move(attribute_list));
  // The valid() check above prevents the attribute list from being long enough
  // to overflow |size|.
  size += attribute_list_elem.WriteSize();

  std::vector<DataElement> pattern(service_search_pattern_.size());
  size_t i = 0;
  for (const auto& it : service_search_pattern_) {
    pattern.at(i).Set<UUID>(it);
    i++;
  }
  DataElement search_pattern(std::move(pattern));
  size += search_pattern.WriteSize();

  auto buf = BuildNewPdu(kServiceSearchAttributeRequest, tid, size);

  size_t written = sizeof(Header);

  auto mut_view = buf->mutable_view(written);
  written += search_pattern.Write(&mut_view);

  buf->WriteObj(
      pw::bytes::ConvertOrderTo(cpp20::endian::big, max_attribute_byte_count_),
      written);
  written += sizeof(uint16_t);

  mut_view = buf->mutable_view(written);
  written += attribute_list_elem.Write(&mut_view);

  mut_view = buf->mutable_view(written);
  written += WriteContinuationState(&mut_view);
  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

void ServiceSearchAttributeRequest::AddAttribute(AttributeId id) {
  AddToAttributeRanges(&attribute_ranges_, id, id);
}

void ServiceSearchAttributeRequest::AddAttributeRange(AttributeId start,
                                                      AttributeId end) {
  AddToAttributeRanges(&attribute_ranges_, start, end);
}

ServiceSearchAttributeResponse::ServiceSearchAttributeResponse() {}

const BufferView ServiceSearchAttributeResponse::ContinuationState() const {
  if (!continuation_state_) {
    return BufferView();
  }
  return continuation_state_->view();
}

bool ServiceSearchAttributeResponse::complete() const {
  return !continuation_state_;
}

fit::result<Error<>> ServiceSearchAttributeResponse::Parse(
    const ByteBuffer& buf) {
  if (complete() && attribute_lists_.size() != 0) {
    // This response was previously complete and non-empty
    bt_log(TRACE, "sdp", "can't parse into a complete response");
    PW_DCHECK(!partial_response_);
    return ToResult(HostError::kNotReady);
  }

  // Minimum size is an AttributeListsByteCount, an empty AttributeLists
  // (two bytes) and an empty continuation state (1 byte) of AttributeLists
  if (buf.size() < sizeof(uint16_t) + 3) {
    bt_log(TRACE, "sdp", "packet too small to parse");
    return ToResult(HostError::kPacketMalformed);
  }

  uint16_t attribute_lists_byte_count =
      pw::bytes::ConvertOrderFrom(cpp20::endian::big, buf.To<uint16_t>());
  size_t read_size = sizeof(uint16_t);
  if (buf.view(read_size).size() <
      attribute_lists_byte_count + sizeof(uint8_t)) {
    bt_log(TRACE, "sdp", "not enough bytes in rest of packet as indicated");
    return ToResult(HostError::kPacketMalformed);
  }
  // Check to see if there's continuation.
  BufferView cont_state_view;
  if (!ValidContinuationState(buf.view(read_size + attribute_lists_byte_count),
                              &cont_state_view)) {
    bt_log(TRACE, "sdp", "continuation state is not valid");
    return ToResult(HostError::kPacketMalformed);
  }

  if (cont_state_view.size() == 0) {
    continuation_state_ = nullptr;
  } else {
    continuation_state_ = NewBuffer(cont_state_view.size());
    continuation_state_->Write(cont_state_view);
  }

  auto attribute_lists_bytes = buf.view(read_size, attribute_lists_byte_count);
  if (partial_response_ || ContinuationState().size()) {
    // Append to the incomplete buffer.
    size_t new_partial_size = attribute_lists_byte_count;
    if (partial_response_) {
      new_partial_size += partial_response_->size();
    }
    // We currently don't support more than approx 10 packets of the max size.
    if (new_partial_size > kMaxSupportedAttributeListBytes) {
      bt_log(INFO,
             "sdp",
             "ServiceSearchAttributeResponse exceeds supported size, dropping");
      partial_response_ = nullptr;
      return ToResult(HostError::kNotSupported);
    }

    auto new_partial = NewBuffer(new_partial_size);
    if (partial_response_) {
      new_partial->Write(partial_response_->view());
      new_partial->Write(attribute_lists_bytes, partial_response_->size());
    } else {
      new_partial->Write(attribute_lists_bytes);
    }
    partial_response_ = std::move(new_partial);
    if (continuation_state_) {
      // This is incomplete, we can't parse it yet.
      bt_log(TRACE, "sdp", "continuation state found, returning in progress");
      return ToResult(HostError::kInProgress);
    }
    attribute_lists_bytes = partial_response_->view();
  }

  DataElement attribute_lists;
  size_t elem_size = DataElement::Read(&attribute_lists, attribute_lists_bytes);
  if ((elem_size == 0) ||
      (attribute_lists.type() != DataElement::Type::kSequence)) {
    bt_log(TRACE, "sdp", "couldn't parse attribute lists or wasn't a sequence");
    return ToResult(HostError::kPacketMalformed);
  }
  bt_log(TRACE,
         "sdp",
         "parsed AttributeLists: %s",
         attribute_lists.ToString().c_str());

  // Data Element sequence containing alternating attribute id and attribute
  // value pairs. Only the requested attributes that are present are included.
  // They are sorted in ascending attribute ID order.
  size_t list_idx = 0;
  for (auto* src_list_it = attribute_lists.At(0); src_list_it != nullptr;
       src_list_it = attribute_lists.At(++list_idx)) {
    if ((src_list_it->type() != DataElement::Type::kSequence)) {
      bt_log(TRACE, "sdp", "list %zu wasn't a sequence", list_idx);
      return ToResult(HostError::kPacketMalformed);
    }
    auto [dest_list_it, _] = attribute_lists_.emplace(
        list_idx, std::map<AttributeId, DataElement>());
    AttributeId last_id = 0;
    size_t idx = 0;
    for (const DataElement* it = src_list_it->At(0); it != nullptr;
         it = src_list_it->At(idx)) {
      const DataElement* val = src_list_it->At(idx + 1);
      std::optional<AttributeId> id = it->Get<uint16_t>();
      if (!id || (val == nullptr)) {
        attribute_lists_.clear();
        bt_log(TRACE, "sdp", "attribute isn't a number or value doesn't exist");
        return ToResult(HostError::kPacketMalformed);
      }
      bt_log(TRACE,
             "sdp",
             "adding %zu:%s = %s",
             list_idx,
             bt_str(*it),
             bt_str(*val));
      if (*id < last_id) {
        bt_log(INFO,
               "sdp",
               "attribute ids are in wrong order, ignoring for compat");
      }
      auto [_, inserted] = (*dest_list_it).second.emplace(*id, val->Clone());
      if (!inserted) {
        attribute_lists_.clear();
        bt_log(WARN, "sdp", "attribute was duplicated in attribute response");
        return ToResult(HostError::kPacketMalformed);
      }
      last_id = *id;
      idx += 2;
    }
  }
  partial_response_ = nullptr;
  return fit::ok();
}

void ServiceSearchAttributeResponse::SetAttribute(uint32_t idx,
                                                  AttributeId id,
                                                  DataElement value) {
  if (attribute_lists_.find(idx) == attribute_lists_.end()) {
    attribute_lists_.emplace(idx, std::map<AttributeId, DataElement>());
  }
  attribute_lists_[idx].emplace(id, std::move(value));
}

// Continuation state: index of # of bytes into the attribute list element
MutableByteBufferPtr ServiceSearchAttributeResponse::GetPDU(
    uint16_t req_max,
    TransactionId tid,
    uint16_t max_size,
    const ByteBuffer& cont_state) const {
  if (!complete()) {
    return nullptr;
  }
  // If there's continuation state, it's the # of bytes previously written
  // of the attribute list.
  uint32_t bytes_skipped = 0;
  if (cont_state.size() == sizeof(uint32_t)) {
    bytes_skipped = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                                cont_state.To<uint32_t>());
  } else if (cont_state.size() != 0) {
    // We don't generate continuation states of any other length.
    return nullptr;
  }

  std::vector<DataElement> lists;
  lists.reserve(attribute_lists_.size());
  for (const auto& it : attribute_lists_) {
    // Returned in pairs of (attribute id, attribute value)
    std::vector<DataElement> list;
    list.reserve(2 * it.second.size());
    for (const auto& elem_it : it.second) {
      list.emplace_back(static_cast<uint16_t>(elem_it.first));
      list.emplace_back(elem_it.second.Clone());
    }

    lists.emplace_back(std::move(list));
  }

  DataElement list_elem(std::move(lists));

  size_t write_size = list_elem.WriteSize();

  if (bytes_skipped > write_size) {
    bt_log(TRACE,
           "sdp",
           "continuation out of range: %d > %zu",
           bytes_skipped,
           write_size);
    return nullptr;
  }

  // Minimum size is header, byte_count, 2 attribute bytes, and a zero length
  // continuation state
  constexpr uint16_t min_size =
      sizeof(Header) + sizeof(uint16_t) + 2 + sizeof(uint8_t);

  if (min_size > max_size) {
    // Can't make a PDU because we don't have enough space.
    return nullptr;
  }

  uint8_t info_length = 0;
  size_t attribute_lists_byte_count = write_size - bytes_skipped;

  // Two attribute bytes counted in the min_size excluded
  uint16_t max_attribute_byte_count = max_size - min_size + 2;
  if (attribute_lists_byte_count > max_attribute_byte_count) {
    info_length = sizeof(uint32_t);
    bt_log(TRACE,
           "sdp",
           "Max size limits attribute size to %hu of %zu",
           max_attribute_byte_count - info_length,
           attribute_lists_byte_count);
    attribute_lists_byte_count = max_attribute_byte_count - info_length;
  }

  if (attribute_lists_byte_count > req_max) {
    bt_log(TRACE,
           "sdp",
           "Requested size limits attribute size to %d of %zu",
           req_max,
           attribute_lists_byte_count);
    attribute_lists_byte_count = req_max;
    info_length = sizeof(uint32_t);
  }

  // Safe to case to uint16_t as attribute_lists_byte_count was limited to
  // max_attribute_byte_count above.
  uint16_t size =
      static_cast<uint16_t>(sizeof(uint16_t) + attribute_lists_byte_count +
                            sizeof(uint8_t) + info_length);
  auto buf = BuildNewPdu(kServiceSearchAttributeResponse, tid, size);

  size_t written = sizeof(Header);

  buf->WriteObj(pw::bytes::ConvertOrderTo(
                    cpp20::endian::big,
                    static_cast<uint16_t>(attribute_lists_byte_count)),
                written);
  written += sizeof(uint16_t);

  auto attribute_list_bytes = NewBuffer(write_size);
  list_elem.Write(attribute_list_bytes.get());
  buf->Write(
      attribute_list_bytes->view(bytes_skipped, attribute_lists_byte_count),
      written);
  written += attribute_lists_byte_count;

  // Continuation state
  buf->WriteObj(info_length, written);
  written += sizeof(uint8_t);
  if (info_length > 0) {
    // Safe to cast because the value is constrained by the number of local
    // attributes, which will be much less than UINT32_MAX. bytes_skipped (an
    // untrusted value) is checked for out-of-range above.
    PW_DCHECK(bytes_skipped + attribute_lists_byte_count <
              std::numeric_limits<uint32_t>::max());
    bytes_skipped =
        static_cast<uint32_t>(bytes_skipped + attribute_lists_byte_count);
    buf->WriteObj(pw::bytes::ConvertOrderTo(cpp20::endian::big, bytes_skipped),
                  written);
    written += sizeof(uint32_t);
  }
  PW_DCHECK(written == sizeof(Header) + size);
  return buf;
}

}  // namespace bt::sdp
