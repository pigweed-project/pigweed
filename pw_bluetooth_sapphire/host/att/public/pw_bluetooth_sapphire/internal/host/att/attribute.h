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

#pragma once
#include <lib/fit/function.h>
#include <lib/fit/result.h>
#include <pw_assert/check.h>

#include <memory>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::att {

// Defines the read or write access permissions for an attribute.
class AccessRequirements final {
 public:
  // The default access permission prevents the attribute from being accessed.
  AccessRequirements();

  // Enables access permission with the given requirements.
  // |min_enc_key_size| is the required minimum size of the key encrypting the
  // connection in order to access an attribute where |encryption| or
  // |authentication| is true.
  //
  // The default value of |min_enc_key_size| enforces maximum security, but
  // could prevent devices that don't support |kMaxEncryptionKeySize| from
  // accessing attributes that require encryption. In practice, it seems that
  // most devices that access such attributes support |kMaxEncryptionKeySize|.
  AccessRequirements(bool encryption,
                     bool authentication,
                     bool authorization,
                     uint8_t min_enc_key_size = sm::kMaxEncryptionKeySize);

  // Returns true if this attribute can be accessed at all.
  inline bool allowed() const {
    return value_ & kAttributePermissionBitAllowed;
  }

  // Returns true if no security is required.
  inline bool allowed_without_security() const {
    return value_ == kAttributePermissionBitAllowed;
  }

  // The following getters return the security requirements of this attribute:

  inline bool encryption_required() const {
    return value_ & kAttributePermissionBitEncryptionRequired;
  }

  inline bool authentication_required() const {
    return value_ & kAttributePermissionBitAuthenticationRequired;
  }

  inline bool authorization_required() const {
    return value_ & kAttributePermissionBitAuthorizationRequired;
  }

  inline uint8_t min_enc_key_size() const { return min_enc_key_size_; }

  inline bool operator==(const AccessRequirements& other) const {
    return value_ == other.value_ &&
           min_enc_key_size_ == other.min_enc_key_size_;
  }

 private:
  uint8_t value_;
  uint8_t min_enc_key_size_;
};

class AttributeGrouping;

// Represents an attribute. Each attribute is assigned a handle (unique within
// the scope of an Adapter) and a UUID that identifies its type. The type of an
// attribute dictates how to interpret the attribute value.
//
// Each attribute has a value of up to 512 octets. An Attribute can be
// configured to have a static value. In such a case the value can be directly
// obtained by calling |value()|. Such attributes cannot be written to.
//
// Otherwise, an Attribute is considered dynamic and its value must be accessed
// asynchronously by calling ReadAsync()/WriteAsync().
//
// Instances cannot be constructed directly and must be obtained from an
// AttributeGrouping.
//
// THREAD-SAFETY:
//
// This class is not thread-safe. The constructor/destructor and all public
// methods must be called on the same thread.
class Attribute final {
 public:
  // Allow move construction and assignment.
  Attribute(Attribute&& other) = default;
  Attribute& operator=(Attribute&& other) = default;

  bool is_initialized() const { return handle_ != kInvalidHandle; }

  Handle handle() const { return handle_; }
  const UUID& type() const { return type_; }

  // The grouping that this attribute belongs to.
  const AttributeGrouping& group() const { return *group_; }

  // Returns the current attribute value. Returns nullptr if the attribute has a
  // dynamic value.
  const ByteBuffer* value() const {
    return !value_.empty() ? &value_ : nullptr;
  }

  // The read/write permissions of this attribute.
  const AccessRequirements& read_reqs() const { return read_reqs_; }
  const AccessRequirements& write_reqs() const { return write_reqs_; }

  // Sets |value| as the static attribute value. Once a value is assigned it
  // cannot be overwritten. A static value cannot be assigned to an attribute
  // that permits writes as attribute writes need to propagate to the service
  // layer.
  void SetValue(const ByteBuffer& value);

  // Handlers for reading and writing and attribute value asynchronously. A
  // handler must call the provided the |result_callback| to signal the end of
  // the operation.
  using ReadResultCallback = fit::callback<void(fit::result<ErrorCode> status,
                                                const ByteBuffer& value)>;
  using ReadHandler = fit::function<void(PeerId peer_id,
                                         Handle handle,
                                         uint16_t offset,
                                         ReadResultCallback result_callback)>;
  void set_read_handler(ReadHandler read_handler) {
    read_handler_ = std::move(read_handler);
  }

  // An "ATT Write Command" will trigger WriteHandler with
  // a null |result_callback|
  using WriteResultCallback =
      fit::callback<void(fit::result<ErrorCode> status)>;
  using WriteHandler = fit::function<void(PeerId peer_id,
                                          Handle handle,
                                          uint16_t offset,
                                          const ByteBuffer& value,
                                          WriteResultCallback result_callback)>;
  void set_write_handler(WriteHandler write_handler) {
    write_handler_ = std::move(write_handler);
  }

  // Initiates an asynchronous read of the attribute value. Returns false if
  // this attribute is not dynamic.
  bool ReadAsync(PeerId peer_id,
                 uint16_t offset,
                 ReadResultCallback result_callback) const;

  // Initiates an asynchronous write of the attribute value. Returns false if
  // this attribute is not dynamic.
  bool WriteAsync(PeerId peer_id,
                  uint16_t offset,
                  const ByteBuffer& value,
                  WriteResultCallback result_callback) const;

 private:
  // Only an AttributeGrouping can construct this.
  friend class AttributeGrouping;

  // The default constructor will construct this attribute as uninitialized.
  // This is intended for STL containers.
  Attribute();
  Attribute(AttributeGrouping* group,
            Handle handle,
            const UUID& type,
            const AccessRequirements& read_reqs,
            const AccessRequirements& write_reqs);

  AttributeGrouping* group_;  // The group that owns this Attribute.
  Handle handle_;
  UUID type_;
  AccessRequirements read_reqs_;
  AccessRequirements write_reqs_;

  ReadHandler read_handler_;
  WriteHandler write_handler_;

  DynamicByteBuffer value_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Attribute);
};

// Represents a grouping of attributes (see Vol 3, Part F, 3.2.3). Each grouping
// contains at least one leading attribute that contains the group declaration.
// The type of this attribute dictates the type of the grouping.
//
// Each grouping covers a contiguous range of handle numbers. The size of the
// range is determined by the |attr_count| constructor argument which defines
// the number of attributes in the grouping following the declaration attribute.
// Once constructed, a grouping is not considered complete until available
// handles within the range have been populated.
class AttributeGrouping final {
 public:
  // Initializes this attribute grouping with a group declaration attribute and
  // enough storage for |attr_count| additional attributes. |decl_value| is
  // assigned as the read-only value of the declaration attribute.
  //
  // Note: |attr_count| should not cause the group end handle to exceed
  // att::kHandleMax.
  AttributeGrouping(const UUID& group_type,
                    Handle start_handle,
                    size_t attr_count,
                    const ByteBuffer& decl_value);

  // Inserts a new attribute into this grouping using the given parameters and
  // returns a pointer to it. Returns nullptr if the grouping is out of handles
  // to allocate.
  //
  // The caller should not hold on to the returned pointer as the Attribute
  // object is owned and managed by this AttributeGrouping.
  Attribute* AddAttribute(
      const UUID& type,
      const AccessRequirements& read_reqs = AccessRequirements(),
      const AccessRequirements& write_reqs = AccessRequirements());

  // Returns true if all attributes of this grouping have been populated.
  bool complete() const {
    return attributes_.size() == (end_handle_ - start_handle_ + 1);
  }

  const UUID& group_type() const {
    PW_DCHECK(!attributes_.empty());
    return attributes_[0].type();
  }

  // Value of the group declaration attribute.
  BufferView decl_value() const {
    PW_DCHECK(!attributes_.empty());
    PW_DCHECK(attributes_[0].value());
    return attributes_[0].value()->view();
  }

  // The start and end handles of this grouping (inclusive).
  Handle start_handle() const { return start_handle_; }
  Handle end_handle() const { return end_handle_; }

  bool active() const { return active_; }
  void set_active(bool active) {
    PW_DCHECK(complete(), "set_active() called on incomplete grouping!");
    active_ = active;
  }

  const std::vector<Attribute>& attributes() const { return attributes_; }

 private:
  Handle start_handle_;
  Handle end_handle_;

  // Only groupings that are active are considered when responding to ATT
  // requests.
  bool active_;

  // The Attributes in this grouping, including the declaration attribute. Space
  // is reserved for all attributes upon construction. The number of elements in
  // |attributes_| reflects how many of the attributes have been initialized.
  std::vector<Attribute> attributes_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AttributeGrouping);
};

}  // namespace bt::att
