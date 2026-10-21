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

#include "pw_containers/internal/var_len_entry_iterator.h"

#include <cstddef>
#include <string_view>

#include "pw_bytes/array.h"
#include "pw_containers/var_len_entry_queue.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::internal::VarLenEntry;
using pw::containers::internal::VarLenEntryIterator;
using namespace std::literals::string_view_literals;

PW_CONSTEXPR_TEST(VarLenEntryIterator, DefaultConstructed, {
  VarLenEntryIterator<std::byte> it1;
  VarLenEntryIterator<std::byte> it2;
  PW_TEST_EXPECT_EQ(it1, it2);
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryIterator, IterationContiguous, {
  std::byte buffer[10] = {};
  pw::VarLenEntryQueue queue(buffer);

  auto data = pw::bytes::String("ABCDE");
  queue.push(data);
  VarLenEntry<const std::byte> entry = queue.front();

  auto it = entry.begin();
  for (std::byte b : data) {
    PW_TEST_EXPECT_EQ(*it++, b);
  }
  PW_TEST_EXPECT_EQ(it, entry.end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryIterator, IterationDiscontiguous, {
  std::byte buffer[8] = {};
  pw::VarLenEntryQueue queue(buffer);

  auto overwritten = pw::bytes::String("12");
  queue.push(overwritten);

  auto data = pw::bytes::String("ABCDE");
  queue.push_overwrite(data);

  VarLenEntry<const std::byte> entry = queue.front();

  auto it = entry.begin();
  for (std::byte b : data) {
    PW_TEST_EXPECT_EQ(*it++, b);
  }
  PW_TEST_EXPECT_EQ(it, entry.end());
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryIterator, Addition, {
  std::byte buffer[10] = {};
  pw::VarLenEntryQueue queue(buffer);

  auto data = pw::bytes::String("01234567");
  queue.push(data);

  VarLenEntry<const std::byte> entry = queue.front();

  auto it = entry.begin();
  PW_TEST_EXPECT_EQ(*(it + 3), std::byte{'3'});
  PW_TEST_EXPECT_EQ(*(5 + it), std::byte{'5'});

  it += 7;
  PW_TEST_EXPECT_EQ(*it, std::byte{'7'});
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryIterator, Equality, {
  std::byte buffer[10] = {};
  pw::VarLenEntryQueue queue(buffer);

  auto data = pw::bytes::String("012");
  queue.push(data);

  VarLenEntry<const std::byte> entry = queue.front();

  auto it1 = entry.begin();
  auto it2 = entry.begin();
  PW_TEST_EXPECT_EQ(it1, it2);

  ++it2;
  PW_TEST_EXPECT_NE(it1, it2);

  it1 += 1;
  PW_TEST_EXPECT_EQ(it1, it2);

  PW_TEST_EXPECT_NE(it1, entry.end());
  PW_TEST_EXPECT_NE(entry.end(), it2);
});

PW_CONSTEXPR_TEST_IF_CLANG(VarLenEntryIterator, Mutable, {
  std::byte buffer[10] = {};
  pw::VarLenEntryQueue queue(buffer);

  auto data = pw::bytes::String("ABCDE");
  queue.push(data);

  VarLenEntry<std::byte> entry = queue.front();

  auto it = entry.begin();
  *it = std::byte{'Z'};
  *(it + 1) = std::byte{'Y'};
  *(it + 4) = std::byte{'X'};

  std::byte copy_buffer[5] = {};
  entry.copy(copy_buffer, sizeof(copy_buffer));
  PW_TEST_EXPECT_EQ(copy_buffer[0], std::byte('Z'));
  PW_TEST_EXPECT_EQ(copy_buffer[1], std::byte('Y'));
  PW_TEST_EXPECT_EQ(copy_buffer[2], std::byte('C'));
  PW_TEST_EXPECT_EQ(copy_buffer[3], std::byte('D'));
  PW_TEST_EXPECT_EQ(copy_buffer[4], std::byte('X'));
});

}  // namespace
