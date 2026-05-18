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
#include <pw_assert/check.h>

#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"

namespace bt::sdp {

// See Bluetooth Core Spec v5.0, Vol 3, Part B, Sec 3.1.
//
// Each Data element has a header and a data field. The header field contains a
// type descriptor and a size descriptor. The header field is of variable
// length, which is determined by the size descriptor.
//
// DataElements all start as the Null type, and then can be set to any type.
//
// Examples:
//    DataElement elem;  // A null type
//    uint32_t seymour = 0xFEED;
//    elem.Set(seymour);
//    PW_DCHECK(elem.type() == DataElement::Type::kUnsignedInt);
//    PW_DCHECK(elem.Get<uint32_t>());
//    PW_DCHECK(*elem.Get<uint32_t>() == seymour);
//
//    std::vector<DataElement> service_class_ids;
//    DataElement uuid;
//    uuid.Set(UUID(sdp::ServiceClass::kAudioSource));
//    service_class_ids.emplace_back(std::move(uuid));
//    elem.Set(service_class_ids);
//    PW_DCHECK(e.type() == DataElement::Type::kSequence);
//    PW_DCHECK(!e.Get<uint32_t>());
class DataElement {
 public:
  // Type Descriptors. Only the top 5 bits are used, see kTypeMask in Bluetooth
  // Core Spec v5.0, Vol 3, Part B, Sec 3.2.
  enum class Type : uint8_t {
    kNull = (0 << 3),
    kUnsignedInt = (1 << 3),
    kSignedInt = (2 << 3),
    kUuid = (3 << 3),
    kString = (4 << 3),
    kBoolean = (5 << 3),
    kSequence = (6 << 3),
    kAlternative = (7 << 3),
    kUrl = (8 << 3),
  };
  constexpr static uint8_t kTypeMask = 0xF8;

  // Size Descriptor describing the size of the data following.
  //
  // Only three bits are used. For 0-4, the size is 2^(value) except in the case
  // of kNull, in which case the size is 0. otherwise, the size is described in
  // 2^(5-value) extra bytes following.
  //
  // v45.0, Vol 3, Part B, Sec 3.3
  enum class Size : uint8_t {
    kOneByte = 0,
    kTwoBytes = 1,
    kFourBytes = 2,
    kEightBytes = 3,
    kSixteenBytes = 4,
    kNextOne = 5,
    kNextTwo = 6,
    kNextFour = 7,
  };
  constexpr static uint8_t kSizeMask = 0x07;

  // Constructs a Null data element.
  DataElement();
  ~DataElement() = default;

  // No assignment operator.
  DataElement& operator=(const DataElement&) = delete;

  // Default move constructor and move-assignment
  DataElement(DataElement&&) = default;
  DataElement& operator=(DataElement&&) = default;

  explicit DataElement(const bt::DynamicByteBuffer& value) { Set(value); }
  explicit DataElement(const std::string& value) { Set(value); }
  explicit DataElement(std::vector<DataElement>&& value) {
    Set(std::move(value));
  }

  // Convenience constructor to create a DataElement from a basic type.
  template <typename T>
  explicit DataElement(T value) {
    Set<T>(std::move(value));
  }

  // Make a deep copy of this element.
  DataElement Clone() const { return DataElement(*this); }

  // Reads a DataElement from |buffer|, replacing any data that was in |elem|.
  // Returns the amount of space occupied on |buffer| by the data element, or
  // zero if no element could be read.
  static size_t Read(DataElement* elem, const ByteBuffer& buffer);

  // The type of this element.
  Type type() const { return type_; }

  // The size of this element.
  Size size() const { return size_; }

  // Sets the value of this element to |value|.
  //
  // Defined specializations:
  //    typename                                     - type()
  //    std::nullptr_t                               - kNull
  //    uint8_t, .., uint64_t                        - kUnsignedInt
  //    int8_t .. int64_t                            - kSignedInt
  //    const UUID&                                  - kUuid
  //    const bt::DynamicByteBuffer or std::string   - kString
  //    bool                                         - kBoolean
  //    std::vector<DataElement>                     - kSequence
  //    (Use SetUrl())                               - kUrl
  template <typename T>
  void Set(T value);
  void Set(const bt::DynamicByteBuffer& value);
  void Set(const std::string& value);
  void Set(std::vector<DataElement>&& value);

  // Sets this element's value to an alternative of the items in |items|
  void SetAlternative(std::vector<DataElement>&& items);

  // Sets this element's value to the provided |url|. No-op if |url| contains
  // invalid URI characters as defined in [RFC
  // 3986](https://www.rfc-editor.org/rfc/rfc3986).
  void SetUrl(const std::string& url);

  // Get the URL value of this element. Returns an optional without a value if
  // the wrong type is stored.
  std::optional<std::string> GetUrl() const;

  // Get the value of this element. Has the same defined specializations as
  // Set(). Returns an optional without a value if the wrong type is stored.
  template <typename T>
  std::optional<T> Get() const;

  // Get a pointer to an element in a DataElement Sequence. Returns nullptr if
  // type() is not kSequence or the index is invalid. Only valid as long as the
  // containing sequence is valid.
  const DataElement* At(size_t idx) const;

  // Get the number of elements in a DataElement Sequence or Alternative.
  // Returns an nullopt_t if this is not a Sequence or Alternative type.
  std::optional<size_t> ElementCount() const;

  // Calculates the number of bytes that this DataElement will use if it's
  // written using Write().
  size_t WriteSize() const;

  // Writes this DataElement to |buffer|. Returns the number of bytes used for
  // writing this element.
  size_t Write(MutableByteBuffer* buffer) const;

  // Debug representation of this element (including it's type and size) in a
  // string, i.e. UnsignedInt:4(15) or Sequence { UUID(1567), UUID(2502) }
  std::string ToString() const;

 private:
  // Copy constructor for Clone().
  DataElement(const DataElement&);

  // Sets the size type based on a variable size (Next one, two, or four)
  void SetVariableSize(size_t length);

  Type type_;
  Size size_;

  // Various types for the stored value. These are only valid if the type_ is
  // set correctly.
  int64_t int_value_;
  uint64_t uint_value_;
  UUID uuid_;
  bt::DynamicByteBuffer bytes_;
  std::vector<DataElement> aggregate_;
};

}  // namespace bt::sdp
