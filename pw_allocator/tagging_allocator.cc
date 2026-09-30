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

#include "pw_allocator/tagging_allocator.h"

namespace pw::allocator {

pw::tokenizer::Token TaggingAllocator::GetToken(const void* ptr) const {
  const pw::tokenizer::Token* token_ptr = GetPrefix(ptr);
  return *token_ptr;
}

void TaggingAllocator::SetToken(void* ptr, pw::tokenizer::Token token) const {
  pw::tokenizer::Token* token_ptr = GetPrefix(ptr);
  *token_ptr = token;
}

void* TaggingAllocator::DoAllocate(Layout layout) {
  void* ptr = Base::DoAllocate(layout);
  if (ptr != nullptr) {
    SetToken(ptr, default_token_);
  }
  return ptr;
}

}  // namespace pw::allocator
