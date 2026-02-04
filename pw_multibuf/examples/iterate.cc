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

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_bytes/span.h"
#include "pw_checksum/crc32.h"
#include "pw_multibuf/multibuf.h"
#include "pw_random/xor_shift.h"

namespace examples {

constexpr size_t kMaxSize = 256;

std::array<std::byte, kMaxSize> static_buffer;

// DOCSTAG: [pw_multibuf-examples-iterate-create]
/// Creates a MultiBuf that holds non-contiguous memory regions with different
/// memory ownership.
pw::ConstMultiBuf::Instance CreateMultiBuf(pw::Allocator& allocator) {
  pw::ConstMultiBuf::Instance mbuf(allocator);
  pw::random::XorShiftStarRng64 rng(1);
  size_t size;

  // Add some owned data.
  rng.GetInt(size, kMaxSize);
  auto owned_data = allocator.MakeUnique<std::byte[]>(size);
  rng.Get({owned_data.get(), size});
  mbuf->PushBack(std::move(owned_data));

  // Add some static data.
  rng.GetInt(size, kMaxSize);
  pw::ByteSpan static_data(static_buffer.data(), size);
  rng.Get(static_data);
  mbuf->PushBack(static_data);

  // Add some shared data.
  rng.GetInt(size, kMaxSize);
  auto shared_data = allocator.MakeShared<std::byte[]>(size);
  rng.Get({shared_data.get(), size});
  mbuf->PushBack(shared_data);

  return mbuf;
}
// DOCSTAG: [pw_multibuf-examples-iterate-create]

// DOCSTAG: [pw_multibuf-examples-iterate-bytes]
/// Calculates the CRC32 checksum of a MultiBuf one byte at a time.
uint32_t BytesChecksum(const pw::ConstMultiBuf& mbuf) {
  pw::checksum::Crc32 crc32;
  for (std::byte b : mbuf) {
    crc32.Update(b);
  }
  return crc32.value();
}
// DOCSTAG: [pw_multibuf-examples-iterate-bytes]

// DOCSTAG: [pw_multibuf-examples-iterate-chunks]
/// Calculates the CRC32 checksum of a MultiBuf one chunk at a time.
uint32_t ChunksChecksum(const pw::ConstMultiBuf& mbuf) {
  pw::checksum::Crc32 crc32;
  for (pw::ConstByteSpan s : mbuf.ConstChunks()) {
    crc32.Update(s);
  }
  return crc32.value();
}
// DOCSTAG: [pw_multibuf-examples-iterate-chunks]

TEST(IterateTest, BytesChecksumMatchesChunksChecksum) {
  pw::allocator::test::AllocatorForTest<512> allocator;
  auto mbuf = CreateMultiBuf(allocator);
  EXPECT_EQ(BytesChecksum(mbuf), ChunksChecksum(mbuf));
}

}  // namespace examples
