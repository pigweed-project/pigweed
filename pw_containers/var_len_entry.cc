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

#include "pw_containers/internal/var_len_entry.h"

#include <cstddef>
#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_preprocessor/util.h"

namespace pw::containers::internal {

VarLenEntry<std::byte> VarLenEntryC::From(
    const pw_InlineVarLenEntryQueue_Entry& entry) {
  ByteSpan data1(reinterpret_cast<std::byte*>(entry.data_1), entry.size_1);
  ByteSpan data2(reinterpret_cast<std::byte*>(entry.data_2), entry.size_2);
  return VarLenEntry<std::byte>(data1, data2);
}

VarLenEntry<const std::byte> VarLenEntryC::From(
    const pw_InlineVarLenEntryQueue_ConstEntry& entry) {
  ConstByteSpan data1(reinterpret_cast<const std::byte*>(entry.data_1),
                      entry.size_1);
  ConstByteSpan data2(reinterpret_cast<const std::byte*>(entry.data_2),
                      entry.size_2);
  return VarLenEntry<const std::byte>(data1, data2);
}

pw_InlineVarLenEntryQueue_Entry VarLenEntryC::To(
    const VarLenEntry<std::byte>& cxx_entry) {
  return pw_InlineVarLenEntryQueue_Entry{
      .data_1 = reinterpret_cast<uint8_t*>(cxx_entry.data1_.data()),
      .size_1 = static_cast<uint32_t>(cxx_entry.data1_.size()),
      .data_2 = reinterpret_cast<uint8_t*>(cxx_entry.data2_.data()),
      .size_2 = static_cast<uint32_t>(cxx_entry.data2_.size()),
  };
}

pw_InlineVarLenEntryQueue_ConstEntry VarLenEntryC::To(
    const VarLenEntry<const std::byte>& cxx_entry) {
  return pw_InlineVarLenEntryQueue_ConstEntry{
      .data_1 = reinterpret_cast<const uint8_t*>(cxx_entry.data1_.data()),
      .size_1 = static_cast<uint32_t>(cxx_entry.data1_.size()),
      .data_2 = reinterpret_cast<const uint8_t*>(cxx_entry.data2_.data()),
      .size_2 = static_cast<uint32_t>(cxx_entry.data2_.size()),
  };
}

}  // namespace pw::containers::internal

using pw::containers::internal::VarLenEntryC;

PW_EXTERN_C_START

uint32_t pw_InlineVarLenEntryQueue_Entry_Copy(
    const pw_InlineVarLenEntryQueue_Entry* entry, void* dest, uint32_t count) {
  auto cxx_entry = VarLenEntryC::From(*entry);
  auto* ptr = reinterpret_cast<std::byte*>(dest);
  return static_cast<uint32_t>(cxx_entry.copy(ptr, count));
}

uint32_t pw_InlineVarLenEntryQueue_ConstEntry_Copy(
    const pw_InlineVarLenEntryQueue_ConstEntry* entry,
    void* dest,
    uint32_t count) {
  auto cxx_entry = VarLenEntryC::From(*entry);
  auto* ptr = reinterpret_cast<std::byte*>(dest);
  return static_cast<uint32_t>(cxx_entry.copy(ptr, count));
}

uint8_t pw_InlineVarLenEntryQueue_Entry_At(
    const pw_InlineVarLenEntryQueue_Entry* entry, size_t index) {
  auto cxx_entry = VarLenEntryC::From(*entry);
  return static_cast<uint8_t>(cxx_entry.at(index));
}

uint8_t pw_InlineVarLenEntryQueue_ConstEntry_At(
    const pw_InlineVarLenEntryQueue_ConstEntry* entry, size_t index) {
  auto cxx_entry = VarLenEntryC::From(*entry);
  return static_cast<uint8_t>(cxx_entry.at(index));
}

PW_EXTERN_C_END
