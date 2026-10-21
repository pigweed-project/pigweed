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

#include "pw_containers/internal/var_len_entry_queue_iterator.h"

#include <cstddef>
#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_preprocessor/util.h"

namespace pw::containers::internal {

VarLenEntryQueueIterator<std::byte> VarLenEntryQueueIteratorC::From(
    const pw_InlineVarLenEntryQueue_Iterator& iterator) {
  ByteSpan bytes(reinterpret_cast<std::byte*>(iterator.internal_data),
                 iterator.internal_size);
  return VarLenEntryQueueIterator<std::byte>(bytes, iterator.internal_offset);
}

VarLenEntryQueueIterator<const std::byte> VarLenEntryQueueIteratorC::From(
    const pw_InlineVarLenEntryQueue_ConstIterator& iterator) {
  ConstByteSpan bytes(
      reinterpret_cast<const std::byte*>(iterator.internal_data),
      iterator.internal_size);
  return VarLenEntryQueueIterator<const std::byte>(bytes,
                                                   iterator.internal_offset);
}

pw_InlineVarLenEntryQueue_Iterator VarLenEntryQueueIteratorC::To(
    const VarLenEntryQueueIterator<std::byte>& cxx_iterator) {
  return pw_InlineVarLenEntryQueue_Iterator{
      .internal_data = reinterpret_cast<uint8_t*>(cxx_iterator.data_.data()),
      .internal_size = static_cast<uint32_t>(cxx_iterator.data_.size()),
      .internal_offset = cxx_iterator.offset_,
  };
}

pw_InlineVarLenEntryQueue_ConstIterator VarLenEntryQueueIteratorC::To(
    const VarLenEntryQueueIterator<const std::byte>& cxx_iterator) {
  return pw_InlineVarLenEntryQueue_ConstIterator{
      .internal_data =
          reinterpret_cast<const uint8_t*>(cxx_iterator.data_.data()),
      .internal_size = static_cast<uint32_t>(cxx_iterator.data_.size()),
      .internal_offset = cxx_iterator.offset_,
  };
}

}  // namespace pw::containers::internal

using pw::containers::internal::VarLenEntryC;
using pw::containers::internal::VarLenEntryQueueIteratorC;

PW_EXTERN_C_START

void pw_InlineVarLenEntryQueue_Iterator_Advance(
    pw_InlineVarLenEntryQueue_Iterator* iterator) {
  auto cxx_iterator = VarLenEntryQueueIteratorC::From(*iterator);
  cxx_iterator++;
  *iterator = VarLenEntryQueueIteratorC::To(cxx_iterator);
}

void pw_InlineVarLenEntryQueue_ConstIterator_Advance(
    pw_InlineVarLenEntryQueue_ConstIterator* iterator) {
  auto cxx_iterator = VarLenEntryQueueIteratorC::From(*iterator);
  cxx_iterator++;
  *iterator = VarLenEntryQueueIteratorC::To(cxx_iterator);
}

bool pw_InlineVarLenEntryQueue_Iterator_Equal(
    const pw_InlineVarLenEntryQueue_Iterator* lhs,
    const pw_InlineVarLenEntryQueue_Iterator* rhs) {
  auto cxx_rhs = VarLenEntryQueueIteratorC::From(*lhs);
  auto cxx_lhs = VarLenEntryQueueIteratorC::From(*rhs);
  return cxx_rhs == cxx_lhs;
}

bool pw_InlineVarLenEntryQueue_ConstIterator_Equal(
    const pw_InlineVarLenEntryQueue_ConstIterator* lhs,
    const pw_InlineVarLenEntryQueue_ConstIterator* rhs) {
  auto cxx_rhs = VarLenEntryQueueIteratorC::From(*lhs);
  auto cxx_lhs = VarLenEntryQueueIteratorC::From(*rhs);
  return cxx_rhs == cxx_lhs;
}

pw_InlineVarLenEntryQueue_Entry pw_InlineVarLenEntryQueue_GetEntry(
    const pw_InlineVarLenEntryQueue_Iterator* iterator) {
  auto cxx_iterator = VarLenEntryQueueIteratorC::From(*iterator);
  return VarLenEntryC::To(*cxx_iterator);
}

pw_InlineVarLenEntryQueue_ConstEntry pw_InlineVarLenEntryQueue_GetConstEntry(
    const pw_InlineVarLenEntryQueue_ConstIterator* iterator) {
  auto cxx_iterator = VarLenEntryQueueIteratorC::From(*iterator);
  return VarLenEntryC::To(*cxx_iterator);
}

PW_EXTERN_C_END
