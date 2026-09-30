// Copyright 2026 The Pigweed Authors
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

#include "pw_allocator/framing_allocator.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::allocator {

/// A FramingAllocator that stores "tag" tokens with each allocation.
///
/// This can aid in attribution when debugging.
class TaggingAllocator : public FramingAllocator<pw::tokenizer::Token> {
 private:
  using Base = FramingAllocator<pw::tokenizer::Token>;

 public:
  constexpr TaggingAllocator(pw::tokenizer::Token default_token,
                             Allocator& allocator)
      : Base(allocator), default_token_(default_token) {}

  /// Returns the token associated with the given allocation. Pointer must be to
  /// an allocation by a ``TaggingAllocator``.
  pw::tokenizer::Token GetToken(const void* ptr) const;

  /// Sets the token associated with the given allocation. Pointer must be to
  /// an allocation by a ``TaggingAllocator``.
  void SetToken(void* ptr, pw::tokenizer::Token token) const;

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

 private:
  const pw::tokenizer::Token default_token_;
};

}  // namespace pw::allocator
