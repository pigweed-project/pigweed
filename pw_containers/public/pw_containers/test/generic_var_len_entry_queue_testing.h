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
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "pw_unit_test/framework.h"

/// Includes tests for types derived from GenericVarLenEntryQueue.
///
/// To use, define a type that derives `GenericVarLenEntryQueueTest` and
/// provides a factory method named `MakeQueue` for the queue of the desired
/// type. This method should be templated on the byte-like value type and the
/// max size in bytes of a single entry. Pass this type to the macro.
///
/// @code{.cpp}
/// class MyQueueTest : public GenericVarLenEntryQueueTest<MyQueueTest> {
///  public:
///   template <typename T, size_t kMaxSizeBytes>
///   MyQueue<T> MakeQueue() { return MyQueue<T>(kMaxSizeBytes); }
/// }
/// PW_GENERIC_VAR_LEN_ENTRY_QUEUE_TESTS(MyQueueTest);
/// @endcode
#define PW_GENERIC_VAR_LEN_ENTRY_QUEUE_TESTS(TestFixture)                      \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, Iterate)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              IterateOverwrittenElements)      \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              MaxSizeOneBytePrefix)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              MaxSizeTwoBytePrefix)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, ConstEntry)         \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, ModifyEntry)        \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(                                 \
      TestFixture, WrappedEntryIteratorPlusAndPlusEquals)                      \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(                                 \
      TestFixture, NonWrappedEntryIteratorPlusAndPlusEquals)                   \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              ModifyMultipleEntries)           \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, CopyEntries)        \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              CopyEntries_DestinationTooSmall) \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              CopyEntries_EmptySource)         \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              CopyEntriesOverwrite)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, MoveEntries)        \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              MoveEntries_DestinationTooSmall) \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              MoveEntriesOverwrite)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture,                     \
                                              ContiguousRawStorage)            \
  _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, Dering)             \
  static_assert(true, "use a semicolon")

#define _PW_GENERIC_VAR_LEN_ENTRY_QUEUE_INVOKE_TEST(TestFixture, TestCase) \
  TEST_F(TestFixture, TestCase) { TestCase(); }

namespace pw::containers::test {

/// A set of unit tests for queues derived from GenericVarLenEntryQueue.
///
/// This class uses the CRTP pattern to allow derived types to specify how to
/// make the queues to be tested.
///
/// Callers shouldn't need to use this class directly; see
/// PW_GENERIC_VAR_LEN_ENTRY_QUEUE_TESTS instead.
template <typename Derived>
class GenericVarLenEntryQueueTest : public ::testing::Test {
 private:
  constexpr Derived& derived() { return *static_cast<Derived*>(this); }

 protected:
  static constexpr const char* kStrings[] = {
      "Haart",
      "Sandro",
      "",
      "Gelu",
      "Solmyr",
  };

  void Iterate() {
    auto queue = derived().template MakeQueue<char, 32>();
    for (const char* string : kStrings) {
      queue.push(std::string_view(string));
    }

    uint32_t i = 0;
    for (auto entry : queue) {
      char value[8]{};
      entry.copy(value, sizeof(value));
      EXPECT_STREQ(value, kStrings[i++]);
    }
    ASSERT_EQ(i, 5u);
  }

  void IterateOverwrittenElements() {
    auto queue = derived().template MakeQueue<char, 6>();
    for (const char* string : kStrings) {
      queue.push_overwrite(std::string_view(string));
    }

    ASSERT_EQ(queue.size(), 1u);

    for (auto entry : queue) {
      char value[8]{};
      EXPECT_EQ(6u, entry.copy(value, sizeof(value)));
      EXPECT_STREQ(value, "Solmyr");
    }
  }

  void MaxSizeOneBytePrefix() {
    auto queue = derived().template MakeQueue<std::byte, 127>();
    EXPECT_EQ(queue.max_size(), 128u);

    while (queue.try_push({})) {
    }
    EXPECT_EQ(queue.size(), queue.max_size());
    EXPECT_EQ(queue.size_bytes(), 0u);
  }

  void MaxSizeTwoBytePrefix() {
    auto queue = derived().template MakeQueue<std::byte, 128>();
    EXPECT_EQ(queue.max_size(), 130u);

    while (queue.try_push({})) {
    }
    EXPECT_EQ(queue.size(), queue.max_size());
    EXPECT_EQ(queue.size_bytes(), 0u);
  }

  void ConstEntry() {
    auto queue = derived().template MakeQueue<char, 5>();
    queue.push("12");  // Split the next entry across the end.
    queue.push_overwrite(std::string_view("ABCDE"));

    typename decltype(queue)::const_value_type front = queue.front();

    ASSERT_EQ(front.size(), 5u);
    EXPECT_EQ(front[0], 'A');
    EXPECT_EQ(front[1], 'B');
    EXPECT_EQ(front[2], 'C');
    EXPECT_EQ(front[3], 'D');
    EXPECT_EQ(front[4], 'E');

    EXPECT_EQ(front.at(0), 'A');
    EXPECT_EQ(front.at(1), 'B');
    EXPECT_EQ(front.at(2), 'C');
    EXPECT_EQ(front.at(3), 'D');
    EXPECT_EQ(front.at(4), 'E');

    const auto [span_1, span_2] = front.contiguous_data();
    EXPECT_EQ(span_1.size(), 2u);
    EXPECT_EQ(std::memcmp(span_1.data(), "AB", 2u), 0);
    EXPECT_EQ(span_2.size(), 3u);
    EXPECT_EQ(std::memcmp(span_2.data(), "CDE", 3u), 0);

    const char* expected_ptr = "ABCDE";
    for (char c : front) {
      EXPECT_EQ(*expected_ptr, c);
      ++expected_ptr;
    }

    // Check the iterators with std::copy and std::equal.
    char value[6] = {};
    std::copy(front.begin(), front.end(), value);
    EXPECT_STREQ(value, "ABCDE");

    EXPECT_TRUE(std::equal(front.begin(), front.end(), "ABCDE"));
  }

  void ModifyEntry() {
    auto queue = derived().template MakeQueue<char, 5>();
    queue.push("12");  // Split the next entry across the end.
    queue.push_overwrite(std::string_view("ABCDE"));

    typename decltype(queue)::value_type front = queue.front();

    ASSERT_EQ(front.size(), 5u);
    EXPECT_EQ(std::exchange(front[0], 'a'), 'A');
    EXPECT_EQ(std::exchange(front[1], 'b'), 'B');
    EXPECT_EQ(std::exchange(front[2], 'c'), 'C');
    EXPECT_EQ(std::exchange(front[3], 'd'), 'D');
    EXPECT_EQ(std::exchange(front[4], 'e'), 'E');

    EXPECT_EQ(std::exchange(front.at(0), 'A'), 'a');
    EXPECT_EQ(std::exchange(front.at(1), 'B'), 'b');
    EXPECT_EQ(std::exchange(front.at(2), 'C'), 'c');
    EXPECT_EQ(std::exchange(front.at(3), 'D'), 'd');
    EXPECT_EQ(std::exchange(front.at(4), 'E'), 'e');

    const auto [span_1, span_2] = front.contiguous_data();
    EXPECT_EQ(span_1.size(), 2u);
    EXPECT_EQ(std::memcmp(span_1.data(), "AB", 2u), 0);
    std::fill(span_1.begin(), span_1.end(), '?');
    EXPECT_EQ(std::memcmp(span_1.data(), "??", 2u), 0);

    EXPECT_EQ(span_2.size(), 3u);
    std::fill(span_2.begin(), span_2.end(), '#');
    EXPECT_EQ(std::memcmp(span_2.data(), "###", 3u), 0);

    const char* expected_ptr = "??###";
    for (char c : front) {
      EXPECT_EQ(*expected_ptr, c);
      ++expected_ptr;
    }

    // Check the iterators with std::copy, std::fill, and std::equal.
    std::string_view data("1234");
    std::copy(data.begin(), data.end(), ++front.begin());
    EXPECT_TRUE(std::equal(front.begin(), front.end(), "?1234"));

    ASSERT_EQ(front.front(), '?');
    front.front() = '!';
    EXPECT_EQ(front.front(), '!');

    ASSERT_EQ(front.back(), '4');
    front.back() = '!';
    EXPECT_EQ(front.back(), '!');
  }

  void WrappedEntryIteratorPlusAndPlusEquals() {
    auto queue = derived().template MakeQueue<char, 5>();
    queue.push(std::string_view("12"));  // Split the next entry across the end.
    queue.push_overwrite(std::string_view("ABCDE"));

    auto entry = queue.front();
    auto it = entry.begin();

    EXPECT_EQ(*(it + 0), 'A');
    EXPECT_EQ(*(it + 1), 'B');
    EXPECT_EQ(*(it + 2), 'C');
    EXPECT_EQ(*(it + 3), 'D');
    EXPECT_EQ(*(it + 4), 'E');

    EXPECT_EQ(*(0 + it), 'A');
    EXPECT_EQ(*(4 + it), 'E');

    auto it2 = it;
    it2 += 2;
    EXPECT_EQ(*it2, 'C');
    it2 += 2;
    EXPECT_EQ(*it2, 'E');
  }

  void NonWrappedEntryIteratorPlusAndPlusEquals() {
    auto queue = derived().template MakeQueue<char, 10>();
    queue.push(std::string_view("0123456789"));

    auto entry = queue.front();
    auto it = entry.begin();

    EXPECT_EQ(*(it + 0), '0');
    EXPECT_EQ(*(it + 5), '5');
    EXPECT_EQ(*(5 + it), '5');

    auto it2 = it;
    it2 += 3;
    EXPECT_EQ(*it2, '3');
    it2 += 4;
    EXPECT_EQ(*it2, '7');
  }

  void ModifyMultipleEntries() {
    auto queue = derived().template MakeQueue<char, 7>();
    queue.push(std::string_view("ab"));
    queue.push(std::string_view("CDE"));
    ASSERT_EQ(queue.size(), 2u);

    auto it = queue.begin();
    (*it)[0] = 'v';
    (*it)[1] = 'w';

    ++it;
    (*it)[0] = 'X';
    (*it)[2] = 'Z';

    it = queue.begin();
    EXPECT_EQ(it->size(), 2u);
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "vw"));

    ++it;
    ASSERT_EQ(it->size(), 3u);
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "XDZ"));

    EXPECT_EQ(++it, queue.end());
  }

  void CopyEntries() {
    auto src_queue = derived().template MakeQueue<char, 16>();
    src_queue.push("one");
    src_queue.push("two");
    src_queue.push("three");

    auto dst_queue = derived().template MakeQueue<char, 32>();
    dst_queue.push("existing");
    CopyVarLenEntries(src_queue, dst_queue);

    // Verify destination
    ASSERT_EQ(dst_queue.size(), 4u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "existing"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "one"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "two"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "three"));

    // Verify source is unchanged
    ASSERT_EQ(src_queue.size(), 3u);
    auto src_it = src_queue.begin();
    EXPECT_TRUE(std::equal(src_it->begin(), src_it->end(), "one"));
    ++src_it;
    EXPECT_TRUE(std::equal(src_it->begin(), src_it->end(), "two"));
    ++src_it;
    EXPECT_TRUE(std::equal(src_it->begin(), src_it->end(), "three"));
  }

  void CopyEntries_DestinationTooSmall() {
    auto src_queue = derived().template MakeQueue<char, 32>();
    src_queue.push("12345");
    src_queue.push("6789012345");
    src_queue.push("abc");

    auto dst_queue = derived().template MakeQueue<char, 16>();
    dst_queue.push("existing");  // 8 bytes data, 1 prefix = 9 bytes used
    CopyVarLenEntries(src_queue, dst_queue);

    // "12345" (5 bytes data, 1 prefix) fits.
    // "6789012345" (10 bytes data, 1 prefix) does not fit.
    ASSERT_EQ(dst_queue.size(), 2u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "existing"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "12345"));
    ++it;
    EXPECT_EQ(it, dst_queue.end());
  }

  void CopyEntries_EmptySource() {
    auto src_queue = derived().template MakeQueue<char, 32>();

    auto dst_queue = derived().template MakeQueue<char, 32>();
    dst_queue.push("existing");
    CopyVarLenEntries(src_queue, dst_queue);

    ASSERT_EQ(dst_queue.size(), 1u);
    EXPECT_TRUE(std::equal(
        dst_queue.front().begin(), dst_queue.front().end(), "existing"));
  }

  void CopyEntriesOverwrite() {
    auto src_queue = derived().template MakeQueue<char, 32>();
    src_queue.push("new1");
    src_queue.push("new2");

    auto dst_queue = derived().template MakeQueue<char, 20>();
    dst_queue.push("old1");
    dst_queue.push("old2");
    dst_queue.push("old3");
    CopyVarLenEntriesOverwrite(src_queue, dst_queue);

    // "old1" and "old2" should be dropped to make space.
    ASSERT_EQ(dst_queue.size(), 3u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "old3"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "new1"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "new2"));

    // Verify source is unchanged
    ASSERT_EQ(src_queue.size(), 2u);
  }

  void MoveEntries() {
    // Push and pop an entry to force wrapping.
    auto src_queue = derived().template MakeQueue<char, 16>();
    src_queue.push("placeholder");
    src_queue.pop();
    src_queue.push("one");
    src_queue.push("two");
    src_queue.push("three");

    auto dst_queue = derived().template MakeQueue<char, 32>();
    dst_queue.push("existing");
    MoveVarLenEntries(src_queue, dst_queue);

    // Verify destination
    ASSERT_EQ(dst_queue.size(), 4u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "existing"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "one"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "two"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "three"));

    // Verify source is now empty
    ASSERT_TRUE(src_queue.empty());
  }

  void MoveEntries_DestinationTooSmall() {
    auto src_queue = derived().template MakeQueue<char, 32>();
    src_queue.push("12345");
    src_queue.push("6789012345");
    src_queue.push("abc");

    auto dst_queue = derived().template MakeQueue<char, 16>();
    dst_queue.push("existing");
    MoveVarLenEntries(src_queue, dst_queue);

    // Verify destination has the entries that fit
    ASSERT_EQ(dst_queue.size(), 2u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "existing"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "12345"));
    ++it;
    EXPECT_EQ(it, dst_queue.end());

    // Verify source has the remaining entries
    ASSERT_EQ(src_queue.size(), 2u);
    it = src_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "6789012345"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "abc"));
    ++it;
    EXPECT_EQ(it, src_queue.end());
  }

  void MoveEntriesOverwrite() {
    auto src_queue = derived().template MakeQueue<char, 32>();
    src_queue.push("new1");
    src_queue.push("new2");

    auto dst_queue = derived().template MakeQueue<char, 20>();
    dst_queue.push("old1");
    dst_queue.push("old2");
    dst_queue.push("old3");
    MoveVarLenEntriesOverwrite(src_queue, dst_queue);

    // "old1" and "old2" should be dropped.
    ASSERT_EQ(dst_queue.size(), 3u);
    auto it = dst_queue.begin();
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "old3"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "new1"));
    ++it;
    EXPECT_TRUE(std::equal(it->begin(), it->end(), "new2"));

    // Verify source is empty
    ASSERT_TRUE(src_queue.empty());
  }

  void ContiguousRawStorage() {
    auto queue = derived().template MakeQueue<char, 32>();

    auto [first1, second1] = queue.contiguous_raw_storage();
    EXPECT_TRUE(first1.empty());
    EXPECT_TRUE(second1.empty());
    EXPECT_EQ(queue.encoded_size_bytes(), first1.size() + second1.size());

    // Each string is 6 chars long (with a null terminartor), and has a one byte
    // prefix.
    queue.push("item1");
    queue.push("item2");
    queue.push("item3");
    queue.push("item4");
    auto [first2, second2] = queue.contiguous_raw_storage();
    EXPECT_EQ(queue.size(), 4u);
    EXPECT_EQ(first2.size(), 4u * (1u + 6u));
    EXPECT_TRUE(second2.empty());
    EXPECT_EQ(queue.encoded_size_bytes(), first2.size() + second2.size());

    // 3 of the original entries remain, while 1 entry is wrapped.
    queue.push_overwrite("item5");
    auto [first3, second3] = queue.contiguous_raw_storage();
    EXPECT_EQ(queue.size(), 4u);
    EXPECT_GE(first3.size(), 3u * (1u + 6u));
    EXPECT_LE(second3.size(), 1u * (1u + 6u));
    EXPECT_EQ(queue.encoded_size_bytes(), first3.size() + second3.size());
  }

  void Dering() {
    auto queue = derived().template MakeQueue<char, 32>();

    // Deringing when empty is a no-op.
    auto deringed1 = queue.dering();
    EXPECT_TRUE(deringed1.empty());

    // Each string is 6 chars long (with a null terminartor), and has a one byte
    // prefix.
    queue.push("item1");
    queue.push("item2");
    queue.push("item3");
    queue.push("item4");
    auto [first1, second1] = queue.contiguous_raw_storage();
    ASSERT_FALSE(first1.empty());
    EXPECT_TRUE(second1.empty());

    // Deringing when contiguous is a no-op.
    auto deringed2 = queue.dering();
    EXPECT_EQ(deringed2.data(), first1.data());
    EXPECT_EQ(deringed2.size(), first1.size());

    // 3 of the original entries remain, while 1 entry is wrapped.
    queue.push_overwrite("item5");
    auto [first3, second3] = queue.contiguous_raw_storage();
    ASSERT_FALSE(first3.empty());
    ASSERT_FALSE(second3.empty());
    EXPECT_NE(first3.data(), first1.data());
    size_t len = first3.size() + second3.size();

    // Deringing when wrapped makes the data contiguous.
    auto deringed3 = queue.dering();
    EXPECT_EQ(deringed3.data(), first1.data());
    EXPECT_EQ(deringed3.size(), len);
  }
};

}  // namespace pw::containers::test
