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

#include <cstddef>
#include <cstring>
#include <initializer_list>

#include "pw_unit_test/framework.h"

namespace pw::multibuf::test {

template <typename OwnedChunkType>
class ChunkTestBase : public ::testing::Test {
 protected:
  static constexpr size_t kArbitraryChunkSize = 32;

  bool HasAllocations() const { return DoHasAllocations(); }

  OwnedChunkType MakeChunk(size_t size, std::byte initializer = std::byte(0)) {
    OwnedChunkType chunk = DoMakeChunk(size);
    if (size != 0) {
      std::memset(
          chunk.data(), static_cast<uint8_t>(initializer), chunk.size());
    }
    return chunk;
  }

  OwnedChunkType MakeChunk(std::initializer_list<std::byte> data) {
    OwnedChunkType chunk = DoMakeChunk(data.size());
    std::copy(data.begin(), data.end(), chunk.data());
    return chunk;
  }

  OwnedChunkType MakeChunk(ConstByteSpan data) {
    OwnedChunkType chunk = DoMakeChunk(data.size());
    std::copy(data.begin(), data.end(), chunk.data());
    return chunk;
  }

 private:
  virtual bool DoHasAllocations() const = 0;

  virtual OwnedChunkType DoMakeChunk(size_t size) = 0;
};

}  // namespace pw::multibuf::test
