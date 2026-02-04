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

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/multibuf.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::examples {

// DOCSTAG: [pw_multibuf-examples-queue]
class MultiBufQueue {
 public:
  static Result<MultiBufQueue> Create(Allocator& allocator, size_t max_chunks) {
    MultiBufQueue queue(allocator);
    if (!queue.mbuf_->TryReserveChunks(max_chunks)) {
      return Status::ResourceExhausted();
    }
    return queue;
  }

  [[nodiscard]] bool empty() const { return mbuf_->empty(); }

  [[nodiscard]] bool full() const { return mbuf_->at_capacity(); }

  void push_back(UniquePtr<std::byte[]>&& bytes) {
    PW_ASSERT(!full());
    mbuf_->PushBack(std::move(bytes));
  }

  UniquePtr<const std::byte[]> pop_front() {
    return mbuf_->Release(mbuf_->cbegin());
  }

 private:
  constexpr explicit MultiBufQueue(Allocator& allocator) : mbuf_(allocator) {}

  ConstMultiBuf::Instance mbuf_;
};
// DOCSTAG: [pw_multibuf-examples-queue]

TEST(RingBufferTest, CanPushAndPop) {
  allocator::test::AllocatorForTest<512> allocator;
  constexpr std::array<const char*, 3> kWords = {"foo", "bar", "baz"};
  auto queue = MultiBufQueue::Create(allocator, 3);
  ASSERT_EQ(queue.status(), OkStatus());
  EXPECT_TRUE(queue->empty());

  for (const char* word : kWords) {
    auto s = allocator.MakeUnique<std::byte[]>(4);
    std::strncpy(reinterpret_cast<char*>(s.get()), word, s.size());
    queue->push_back(std::move(s));
  }
  EXPECT_TRUE(queue->full());

  for (const char* word : kWords) {
    auto s = queue->pop_front();
    EXPECT_STREQ(reinterpret_cast<const char*>(s.get()), word);
  }
  EXPECT_TRUE(queue->empty());
}

}  // namespace pw::multibuf::examples
