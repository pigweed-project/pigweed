// Copyright 2024 The Pigweed Authors
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

// DOCSTAG: [pw_allocator-examples-custom_allocator]
#include <cstddef>

#include "pw_allocator/forwarding_allocator.h"

namespace examples {

class CustomAllocator : public pw::allocator::ForwardingAllocator {
 private:
  using Base = pw::allocator::ForwardingAllocator;

 public:
  using Layout = pw::allocator::Layout;

  CustomAllocator(pw::Allocator& allocator, size_t threshold);

 protected:
  /// @copydoc pw::Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc pw::Allocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc pw::Allocator::Resize
  bool DoResize(void*, size_t) override { return false; }

 private:
  size_t used_ = 0;
  size_t threshold_ = 0;
};

}  // namespace examples
