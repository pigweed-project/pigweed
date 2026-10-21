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

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_containers/var_len_entry_queue.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::internal::VarLenEntry;
using pw::containers::internal::VarLenEntryQueueIterator;
using namespace std::literals::string_view_literals;

constexpr pw::VarLenEntryQueue MakeTestQueue(pw::ByteSpan buffer) {
  pw::VarLenEntryQueue queue(buffer);
  queue.push(pw::bytes::String("hello"));
  queue.push(pw::bytes::String("world"));
  queue.push(pw::bytes::String("test"));
  return queue;
}

PW_CONSTEXPR_TEST(VarLenEntryQueueIterator, DefaultConstructed, {
  VarLenEntryQueueIterator<std::byte> it;
  VarLenEntryQueueIterator<std::byte> other;
  PW_TEST_EXPECT_EQ(it, other);
  PW_TEST_EXPECT_FALSE(it != other);
});

PW_CONSTEXPR_TEST(VarLenEntryQueueIteratorTest, Equality, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it1 = queue.begin();
  VarLenEntryQueueIterator<std::byte> it2 = queue.begin();
  VarLenEntryQueueIterator<std::byte> it3 = ++(queue.begin());

  PW_TEST_EXPECT_EQ(it1, it2);
  PW_TEST_EXPECT_NE(it1, it3);
  PW_TEST_EXPECT_TRUE(it1 == it2);
  PW_TEST_EXPECT_TRUE(it1 != it3);
});

PW_CONSTEXPR_TEST(VarLenEntryQueueIteratorTest, Copy, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it1 = queue.begin();
  VarLenEntryQueueIterator<std::byte> it2(it1);
  PW_TEST_EXPECT_EQ(it1, it2);

  VarLenEntryQueueIterator<std::byte> it3;
  it3 = it1;
  PW_TEST_EXPECT_EQ(it1, it3);
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryQueueIteratorTest, Dereference, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it = queue.begin();

  const VarLenEntry<std::byte>& entry = *it;
  auto expected = pw::bytes::String("hello");
  PW_TEST_EXPECT_EQ(entry.size(), 5u);

  auto it_it = it->begin();
  for (const std::byte b : expected) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryQueueIteratorTest, Arrow, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it = queue.begin();

  auto expected = pw::bytes::String("hello");
  PW_TEST_EXPECT_EQ(it->size(), sizeof(expected));

  auto it_it = it->begin();
  for (const std::byte b : expected) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryQueueIteratorTest, PreIncrement, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it = queue.begin();
  ++it;

  const VarLenEntry<std::byte>& entry = *it;
  auto expected = pw::bytes::String("world");
  PW_TEST_EXPECT_EQ(entry.size(), sizeof(expected));

  auto it_it = it->begin();
  for (const std::byte b : expected) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryQueueIteratorTest, PostIncrement, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it = queue.begin();
  VarLenEntryQueueIterator<std::byte> prev_it = it++;

  auto prev_expected = pw::bytes::String("hello");
  PW_TEST_EXPECT_EQ(prev_it->size(), sizeof(prev_expected));

  auto it_it = prev_it->begin();
  for (const std::byte b : prev_expected) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, prev_it->end());

  auto expected = pw::bytes::String("world");
  PW_TEST_EXPECT_EQ(it->size(), sizeof(expected));

  it_it = it->begin();
  for (const std::byte b : expected) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryQueueIteratorTest, Iteration, {
  std::byte buffer[20] = {};
  pw::VarLenEntryQueue queue = MakeTestQueue(buffer);
  VarLenEntryQueueIterator<std::byte> it = queue.begin();

  // First item
  auto expected1 = pw::bytes::String("hello");
  PW_TEST_EXPECT_EQ(it->size(), sizeof(expected1));

  auto it_it = it->begin();
  for (const std::byte b : expected1) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());

  ++it;

  // Second item
  auto expected2 = pw::bytes::String("world");
  PW_TEST_EXPECT_EQ(it->size(), sizeof(expected2));

  it_it = it->begin();
  for (const std::byte b : expected2) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());

  ++it;

  // Third item
  auto expected3 = pw::bytes::String("test");
  PW_TEST_EXPECT_EQ(it->size(), sizeof(expected3));

  it_it = it->begin();
  for (const std::byte b : expected3) {
    PW_TEST_EXPECT_EQ(*it_it++, b);
  }
  PW_TEST_EXPECT_EQ(it_it, it->end());
});

}  // namespace
