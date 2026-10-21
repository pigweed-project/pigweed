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

#include "pw_containers/inline_var_len_entry_queue.h"

#include <cstddef>
#include <cstdint>
#include <new>

#include "pw_bytes/span.h"
#include "pw_containers/internal/var_len_entry_queue_iterator.h"

namespace {

class InlineVarLenEntryQueueC {
 public:
  static pw::InlineVarLenEntryQueue<>& From(
      pw_InlineVarLenEntryQueue_Handle queue) {
    return *std::launder(
        reinterpret_cast<pw::InlineVarLenEntryQueue<>*>(queue));
  }

  static const pw::InlineVarLenEntryQueue<>& From(
      pw_InlineVarLenEntryQueue_ConstHandle queue) {
    return *std::launder(
        reinterpret_cast<const pw::InlineVarLenEntryQueue<>*>(queue));
  }
};

}  // namespace

using pw::containers::internal::VarLenEntryQueueIteratorC;

extern "C" {

void pw_InlineVarLenEntryQueue_Init(uint32_t array[],
                                    size_t array_size_uint32) {
  pw::BasicInlineVarLenEntryQueue<std::byte>::Init(array, array_size_uint32);
}

pw_InlineVarLenEntryQueue_Iterator pw_InlineVarLenEntryQueue_Begin(
    pw_InlineVarLenEntryQueue_Handle queue) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return VarLenEntryQueueIteratorC::To(cxx_queue.begin());
}

pw_InlineVarLenEntryQueue_ConstIterator pw_InlineVarLenEntryQueue_ConstBegin(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return VarLenEntryQueueIteratorC::To(cxx_queue.cbegin());
}

pw_InlineVarLenEntryQueue_Iterator pw_InlineVarLenEntryQueue_End(
    pw_InlineVarLenEntryQueue_Handle queue) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return VarLenEntryQueueIteratorC::To(cxx_queue.end());
}

pw_InlineVarLenEntryQueue_ConstIterator pw_InlineVarLenEntryQueue_ConstEnd(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return VarLenEntryQueueIteratorC::To(cxx_queue.cend());
}

bool pw_InlineVarLenEntryQueue_Empty(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return cxx_queue.empty();
}

uint32_t pw_InlineVarLenEntryQueue_Size(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return static_cast<uint32_t>(cxx_queue.size());
}

uint32_t pw_InlineVarLenEntryQueue_MaxSize(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return static_cast<uint32_t>(cxx_queue.max_size());
}

uint32_t pw_InlineVarLenEntryQueue_SizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return static_cast<uint32_t>(cxx_queue.size_bytes());
}

uint32_t pw_InlineVarLenEntryQueue_MaxSizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  const auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return static_cast<uint32_t>(cxx_queue.max_size_bytes());
}

void pw_InlineVarLenEntryQueue_Push(pw_InlineVarLenEntryQueue_Handle queue,
                                    const void* data,
                                    uint32_t data_size_bytes) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  pw::ConstByteSpan bytes(reinterpret_cast<const std::byte*>(data),
                          data_size_bytes);
  cxx_queue.push(bytes);
}

bool pw_InlineVarLenEntryQueue_TryPush(pw_InlineVarLenEntryQueue_Handle queue,
                                       const void* data,
                                       const uint32_t data_size_bytes) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  pw::ConstByteSpan bytes(reinterpret_cast<const std::byte*>(data),
                          data_size_bytes);
  return cxx_queue.try_push(bytes);
}

void pw_InlineVarLenEntryQueue_PushOverwrite(
    pw_InlineVarLenEntryQueue_Handle queue,
    const void* data,
    uint32_t data_size_bytes) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  pw::ConstByteSpan bytes(reinterpret_cast<const std::byte*>(data),
                          data_size_bytes);
  cxx_queue.push_overwrite(bytes);
}

void pw_InlineVarLenEntryQueue_Pop(pw_InlineVarLenEntryQueue_Handle queue) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  cxx_queue.pop();
}

void pw_InlineVarLenEntryQueue_Clear(pw_InlineVarLenEntryQueue_Handle queue) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  cxx_queue.clear();
}

uint32_t pw_InlineVarLenEntryQueue_RawStorageSizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue) {
  auto& cxx_queue = InlineVarLenEntryQueueC::From(queue);
  return static_cast<uint32_t>(cxx_queue.raw_storage().size());
}

void pw_InlineVarLenEntryQueue_CopyEntries(
    pw_InlineVarLenEntryQueue_ConstHandle from,
    pw_InlineVarLenEntryQueue_Handle to) {
  auto& cxx_from = InlineVarLenEntryQueueC::From(from);
  auto& cxx_to = InlineVarLenEntryQueueC::From(to);
  return CopyVarLenEntries(cxx_from, cxx_to);
}

void pw_InlineVarLenEntryQueue_CopyEntriesOverwrite(
    pw_InlineVarLenEntryQueue_ConstHandle from,
    pw_InlineVarLenEntryQueue_Handle to) {
  auto& cxx_from = InlineVarLenEntryQueueC::From(from);
  auto& cxx_to = InlineVarLenEntryQueueC::From(to);
  return CopyVarLenEntriesOverwrite(cxx_from, cxx_to);
}

void pw_InlineVarLenEntryQueue_MoveEntries(
    pw_InlineVarLenEntryQueue_Handle from,
    pw_InlineVarLenEntryQueue_Handle to) {
  auto& cxx_from = InlineVarLenEntryQueueC::From(from);
  auto& cxx_to = InlineVarLenEntryQueueC::From(to);
  return MoveVarLenEntries(cxx_from, cxx_to);
}

void pw_InlineVarLenEntryQueue_MoveEntriesOverwrite(
    pw_InlineVarLenEntryQueue_Handle from,
    pw_InlineVarLenEntryQueue_Handle to) {
  auto& cxx_from = InlineVarLenEntryQueueC::From(from);
  auto& cxx_to = InlineVarLenEntryQueueC::From(to);
  return MoveVarLenEntriesOverwrite(cxx_from, cxx_to);
}

}  // extern "C"
