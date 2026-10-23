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

#include "pw_allocator/fallback_allocator.h"

#include <algorithm>
#include <cstddef>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/fragmentation.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/check.h"
#include "pw_result/result.h"

namespace pw::allocator {

FallbackAllocator::FallbackAllocator(pw::Allocator& primary,
                                     pw::Allocator& secondary)
    : pw::Allocator(primary.capabilities() | secondary.capabilities()),
      primary_(primary),
      secondary_(secondary) {
  if constexpr (Hardening::kIncludesBasicChecks) {
    PW_CHECK(primary.HasCapability(Capability::kImplementsRecognizes));
  }
}

void* FallbackAllocator::DoAllocate(Layout layout) {
  void* ptr = primary_.Allocate(layout);
  return ptr != nullptr ? ptr : secondary_.Allocate(layout);
}

void FallbackAllocator::DoDeallocate(void* ptr) {
  if (Recognizes(primary_, ptr)) {
    primary_.Deallocate(ptr);
  } else {
    secondary_.Deallocate(ptr);
  }
}

bool FallbackAllocator::DoResize(void* ptr, size_t new_size) {
  return Recognizes(primary_, ptr) ? primary_.Resize(ptr, new_size)
                                   : secondary_.Resize(ptr, new_size);
}

size_t FallbackAllocator::DoGetAllocated() const {
  return primary_.GetAllocated() + secondary_.GetAllocated();
}

std::optional<Fragmentation> FallbackAllocator::DoMeasureFragmentation() const {
  auto primary = primary_.MeasureFragmentation();
  auto secondary = secondary_.MeasureFragmentation();
  if (!primary.has_value() && !secondary.has_value()) {
    return std::nullopt;
  }
  Fragmentation fragmentation;
  if (primary.has_value()) {
    fragmentation += *primary;
  }
  if (secondary.has_value()) {
    fragmentation += *secondary;
  }
  return fragmentation;
}

Result<Layout> FallbackAllocator::DoGetInfo(InfoType info_type,
                                            const void* ptr) const {
  Result<Layout> primary = GetInfo(primary_, info_type, ptr);
  if (info_type != InfoType::kCapacity && primary.ok()) {
    return primary;
  }

  Result<Layout> secondary = GetInfo(secondary_, info_type, ptr);
  if (info_type != InfoType::kCapacity && secondary.ok()) {
    return secondary;
  }

  if (info_type != InfoType::kCapacity) {
    return primary.status().IsUnimplemented() ? secondary : primary;
  }

  if (!primary.ok()) {
    return secondary;
  }

  if (!secondary.ok()) {
    return primary;
  }

  return Layout(primary->size() + secondary->size(),
                std::max(primary->alignment(), secondary->alignment()));
}

}  // namespace pw::allocator
