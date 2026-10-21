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

#include "pw_containers/inline_var_len_entry_queue.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <variant>
#include <vector>

#include "pw_containers/test/generic_var_len_entry_queue_testing.h"
#include "pw_containers_private/inline_var_len_entry_queue_test_oracle.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace {

struct PushOverwrite {
  std::string_view data;
};
struct Push {
  std::string_view data;
};
struct TryPush {
  std::string_view data;
  bool expected;
};
struct Pop {};
struct Clear {};
struct SizeEquals {
  size_t expected;
};

using TestStep =
    std::variant<PushOverwrite, Push, TryPush, Pop, Clear, SizeEquals>;

// Copies an entry, which might be wrapped, to a single std::vector.
std::vector<std::byte> ReadEntry(const pw_InlineVarLenEntryQueue_Iterator& it) {
  auto entry = pw_InlineVarLenEntryQueue_GetEntry(&it);
  std::vector<std::byte> value(entry.size_1 + entry.size_2);
  EXPECT_EQ(value.size(),
            pw_InlineVarLenEntryQueue_Entry_Copy(
                &entry, value.data(), entry.size_1 + entry.size_2));
  return value;
}

// Declares a test that performs a series of operations on the C and C++
// versions of InlineVarLenEntryQueue and the "oracle" class, and checks that
// they match after every step.
template <size_t kMaxEntrySize>
void DataDrivenTest(pw::span<const TestStep> program) {
  pw::InlineVarLenEntryQueue<kMaxEntrySize> cpp_queue;
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(c_queue, kMaxEntrySize);
  pw::containers::InlineVarLenEntryQueueTestOracle oracle(kMaxEntrySize);

  /* Check the queue sizes */
  static_assert(sizeof(cpp_queue) == sizeof(c_queue));
  ASSERT_EQ(cpp_queue.raw_storage().data(),
            reinterpret_cast<const std::byte*>(&cpp_queue));
  ASSERT_EQ(cpp_queue.raw_storage().size_bytes(),
            pw_InlineVarLenEntryQueue_RawStorageSizeBytes(c_queue));

  for (const TestStep& step : program) {
    /* Take the action */
    if (auto ow = std::get_if<PushOverwrite>(&step); ow != nullptr) {
      cpp_queue.push_overwrite(pw::as_bytes(pw::span(ow->data)));
      pw_InlineVarLenEntryQueue_PushOverwrite(
          c_queue, ow->data.data(), static_cast<uint32_t>(ow->data.size()));
      oracle.push_overwrite(pw::as_bytes(pw::span(ow->data)));
    } else if (auto push = std::get_if<Push>(&step); push != nullptr) {
      cpp_queue.push(pw::as_bytes(pw::span(push->data)));
      pw_InlineVarLenEntryQueue_Push(
          c_queue, push->data.data(), static_cast<uint32_t>(push->data.size()));
      oracle.push(pw::as_bytes(pw::span(push->data)));
    } else if (auto try_push = std::get_if<TryPush>(&step);
               try_push != nullptr) {
      ASSERT_EQ(try_push->expected,
                cpp_queue.try_push(pw::as_bytes(pw::span(try_push->data))));
      ASSERT_EQ(try_push->expected,
                pw_InlineVarLenEntryQueue_TryPush(
                    c_queue,
                    try_push->data.data(),
                    static_cast<uint32_t>(try_push->data.size())));
      if (try_push->expected) {
        oracle.push(pw::as_bytes(pw::span(try_push->data)));
      }
    } else if (std::holds_alternative<Pop>(step)) {
      cpp_queue.pop();
      pw_InlineVarLenEntryQueue_Pop(c_queue);
      oracle.pop();
    } else if (auto size = std::get_if<SizeEquals>(&step); size != nullptr) {
      const size_t actual = cpp_queue.size();
      ASSERT_EQ(actual, pw_InlineVarLenEntryQueue_Size(c_queue));
      ASSERT_EQ(oracle.size(), actual);
      ASSERT_EQ(size->expected, actual);
    } else if (std::holds_alternative<Clear>(step)) {
      cpp_queue.clear();
      pw_InlineVarLenEntryQueue_Clear(c_queue);
      oracle.clear();
    } else {
      FAIL() << "Unhandled case";
    }
    /* Check sizes */
    ASSERT_EQ(cpp_queue.size(), oracle.size());
    ASSERT_EQ(cpp_queue.size_bytes(), oracle.size_bytes());
    ASSERT_EQ(cpp_queue.max_size_bytes(), oracle.max_size_bytes());

    ASSERT_EQ(pw_InlineVarLenEntryQueue_Size(c_queue), oracle.size());
    ASSERT_EQ(pw_InlineVarLenEntryQueue_SizeBytes(c_queue),
              oracle.size_bytes());
    ASSERT_EQ(pw_InlineVarLenEntryQueue_MaxSizeBytes(c_queue),
              oracle.max_size_bytes());

    /* Compare the contents */
    auto oracle_it = oracle.begin();
    auto c_queue_it = pw_InlineVarLenEntryQueue_Begin(c_queue);

    const auto c_queue_end = pw_InlineVarLenEntryQueue_End(c_queue);
    uint32_t entries_compared = 0;

    for (auto entry : cpp_queue) {
      entries_compared += 1;

      ASSERT_EQ(*oracle_it, ReadEntry(c_queue_it));
      ASSERT_EQ(*oracle_it, std::vector<std::byte>(entry.begin(), entry.end()));

      ASSERT_NE(oracle_it, oracle.end());
      ASSERT_FALSE(
          pw_InlineVarLenEntryQueue_Iterator_Equal(&c_queue_it, &c_queue_end));

      ++oracle_it;
      pw_InlineVarLenEntryQueue_Iterator_Advance(&c_queue_it);
    }
    ASSERT_EQ(entries_compared, oracle.size());
    ASSERT_TRUE(
        pw_InlineVarLenEntryQueue_Iterator_Equal(&c_queue_it, &c_queue_end));
    ASSERT_EQ(oracle_it, oracle.end());
  }
}

#define DATA_DRIVEN_TEST(steps, max_entry_size)                                \
  TEST(InlineVarLenEntryQueue,                                                 \
       DataDrivenTest_##steps##_MaxSizeBytes##max_entry_size) {                \
    DataDrivenTest<max_entry_size>({steps, sizeof(steps) / sizeof(TestStep)}); \
  }                                                                            \
  static_assert(true, "use a semicolon")

constexpr TestStep kPop[] = {
    SizeEquals{0},
    PushOverwrite{""},
    SizeEquals{1},
    Pop{},
    SizeEquals{0},
};

DATA_DRIVEN_TEST(kPop, 0);  // Only holds one empty entry.
DATA_DRIVEN_TEST(kPop, 1);
DATA_DRIVEN_TEST(kPop, 6);

constexpr TestStep kOverwriteLargeEntriesWithSmall[] = {
    PushOverwrite{"12345"},
    PushOverwrite{"abcde"},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{""},
    SizeEquals{6},
    Pop{},
    Pop{},
    Pop{},
    Pop{},
    Pop{},
    Pop{},
    SizeEquals{0},
};
DATA_DRIVEN_TEST(kOverwriteLargeEntriesWithSmall, 5);
DATA_DRIVEN_TEST(kOverwriteLargeEntriesWithSmall, 6);
DATA_DRIVEN_TEST(kOverwriteLargeEntriesWithSmall, 7);

constexpr TestStep kOverwriteVaryingSizes012[] = {
    PushOverwrite{""},   PushOverwrite{""},   PushOverwrite{""},
    PushOverwrite{""},   PushOverwrite{""},   PushOverwrite{"1"},
    PushOverwrite{"2"},  PushOverwrite{""},   PushOverwrite{"3"},
    PushOverwrite{"4"},  PushOverwrite{""},   PushOverwrite{"5"},
    PushOverwrite{"6"},  PushOverwrite{"ab"}, PushOverwrite{"cd"},
    PushOverwrite{""},   PushOverwrite{"ef"}, PushOverwrite{"gh"},
    PushOverwrite{"ij"},
};
DATA_DRIVEN_TEST(kOverwriteVaryingSizes012, 2);
DATA_DRIVEN_TEST(kOverwriteVaryingSizes012, 3);

constexpr TestStep kOverwriteVaryingSizesUpTo4[] = {
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{"1"},
    PushOverwrite{"2"},
    PushOverwrite{"3"},
    PushOverwrite{"ab"},
    PushOverwrite{"cd"},
    PushOverwrite{"ef"},
    PushOverwrite{"123"},
    PushOverwrite{"456"},
    PushOverwrite{"789"},
    PushOverwrite{"abcd"},
    PushOverwrite{"efgh"},
    PushOverwrite{"ijkl"},
    TryPush{"uhoh", false},
    Pop{},
    SizeEquals{0},
};
DATA_DRIVEN_TEST(kOverwriteVaryingSizesUpTo4, 4);
DATA_DRIVEN_TEST(kOverwriteVaryingSizesUpTo4, 5);
DATA_DRIVEN_TEST(kOverwriteVaryingSizesUpTo4, 6);

constexpr char kBigEntryBytes[196]{};

template <size_t kSizeBytes>
constexpr std::string_view kBigEntry(kBigEntryBytes, kSizeBytes);

constexpr TestStep kTwoBytePrefix[] = {
    PushOverwrite{kBigEntry<128>},
    PushOverwrite{kBigEntry<128>},
    PushOverwrite{kBigEntry<127>},
    PushOverwrite{kBigEntry<128>},
    PushOverwrite{kBigEntry<127>},
    SizeEquals{1},
    Pop{},
    SizeEquals{0},
};
DATA_DRIVEN_TEST(kTwoBytePrefix, 128);
DATA_DRIVEN_TEST(kTwoBytePrefix, 129);

constexpr TestStep kClear[] = {
    Push{"abcdefg"},
    PushOverwrite{""},
    PushOverwrite{""},
    PushOverwrite{"a"},
    PushOverwrite{"b"},
    Clear{},
    SizeEquals{0},
    Clear{},
};
DATA_DRIVEN_TEST(kClear, 7);
DATA_DRIVEN_TEST(kClear, 100);

constexpr TestStep kTryPushMaxSize5[] = {
    TryPush{"", true},
    TryPush{"", true},
    TryPush{"", true},
    TryPush{"", true},
    TryPush{"", true},
    TryPush{"", true},  // max_size_bytes() of 5 => up to 6 empty entries
    TryPush{"", false},
    TryPush{"1", false},
    Clear{},
    TryPush{"12345", true},
    TryPush{"", false},
};
DATA_DRIVEN_TEST(kTryPushMaxSize5, 5);

constexpr TestStep kPushPopLargeEntry[] = {
    Push{kBigEntry<196>},
    TryPush{kBigEntry<196>, false},
    Pop{},
    Push{kBigEntry<196>},
    TryPush{"", true},
    Pop{},
    TryPush{"1", true},
    TryPush{kBigEntry<196>, true},
    TryPush{"12", true},
    Pop{},
    Pop{},
    Pop{},
    TryPush{kBigEntry<196>, true},
    TryPush{kBigEntry<196>, false},
};
DATA_DRIVEN_TEST(kPushPopLargeEntry, 255);
DATA_DRIVEN_TEST(kPushPopLargeEntry, 256);
DATA_DRIVEN_TEST(kPushPopLargeEntry, 257);

TEST(InlineVarLenEntryQueue, DeclareMacro) {
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(queue, 123);

  constexpr size_t kArraySizeBytes =
      123 + 1 /*prefix*/ + 1 /* end */ + 3 /* round up */ +
      PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32 * 4;
  static_assert(sizeof(queue) == kArraySizeBytes);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_RawStorageSizeBytes(queue),
            kArraySizeBytes - 3 /* padding isn't included */);

  EXPECT_EQ(pw_InlineVarLenEntryQueue_MaxSizeBytes(queue), 123u);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_SizeBytes(queue), 0u);
  EXPECT_TRUE(pw_InlineVarLenEntryQueue_Empty(queue));
}

TEST(InlineVarLenEntryQueue, InitializeExistingBuffer) {
  constexpr size_t kArraySize =
      10 + PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32;
  uint32_t queue[kArraySize];
  pw_InlineVarLenEntryQueue_Init(queue, kArraySize);

  EXPECT_EQ(pw_InlineVarLenEntryQueue_RawStorageSizeBytes(queue),
            sizeof(queue));
  EXPECT_EQ(pw_InlineVarLenEntryQueue_MaxSizeBytes(queue),
            sizeof(uint32_t) * 10u - 1 /*prefix*/ - 1 /*end*/);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_SizeBytes(queue), 0u);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_Size(queue), 0u);
  EXPECT_TRUE(pw_InlineVarLenEntryQueue_Empty(queue));
}

TEST(InlineVarLenEntryQueue, MaxSizeElement) {
  // Test max size elements for a few sizes. Commented out statements fail an
  // assert because the elements are too large.
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(q16, 126);
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(q17, 127);
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(q18, 128);
  PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(q19, 129);

  pw_InlineVarLenEntryQueue_PushOverwrite(q16, kBigEntryBytes, 126);
  pw_InlineVarLenEntryQueue_PushOverwrite(q17, kBigEntryBytes, 126);
  pw_InlineVarLenEntryQueue_PushOverwrite(q18, kBigEntryBytes, 126);
  pw_InlineVarLenEntryQueue_PushOverwrite(q19, kBigEntryBytes, 126);

  // pw_InlineVarLenEntryQueue_PushOverwrite(q16, kBigEntryBytes, 127);
  pw_InlineVarLenEntryQueue_PushOverwrite(q17, kBigEntryBytes, 127);
  pw_InlineVarLenEntryQueue_PushOverwrite(q18, kBigEntryBytes, 127);
  pw_InlineVarLenEntryQueue_PushOverwrite(q19, kBigEntryBytes, 127);

  // pw_InlineVarLenEntryQueue_PushOverwrite(q16, kBigEntryBytes, 128);
  // pw_InlineVarLenEntryQueue_PushOverwrite(q17, kBigEntryBytes, 128);
  pw_InlineVarLenEntryQueue_PushOverwrite(q18, kBigEntryBytes, 128);
  pw_InlineVarLenEntryQueue_PushOverwrite(q19, kBigEntryBytes, 128);

  // pw_InlineVarLenEntryQueue_PushOverwrite(q16, kBigEntryBytes, 129);
  // pw_InlineVarLenEntryQueue_PushOverwrite(q17, kBigEntryBytes, 129);
  // pw_InlineVarLenEntryQueue_PushOverwrite(q18, kBigEntryBytes, 129);
  pw_InlineVarLenEntryQueue_PushOverwrite(q19, kBigEntryBytes, 129);

  EXPECT_EQ(pw_InlineVarLenEntryQueue_Size(q16), 1u);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_Size(q17), 1u);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_Size(q18), 1u);
  EXPECT_EQ(pw_InlineVarLenEntryQueue_Size(q19), 1u);
}

TEST(InlineVarLenEntryQueueClass, InitializeExistingBuffer) {
  constexpr size_t kArraySize =
      10 + PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32;
  uint32_t queue_array[kArraySize]{50, 50, 99};
  pw::InlineVarLenEntryQueue<>& queue =
      pw::InlineVarLenEntryQueue<>::Init(queue_array, kArraySize);

  EXPECT_EQ(queue.raw_storage().data(),
            reinterpret_cast<const std::byte*>(queue_array));
  EXPECT_EQ(queue.raw_storage().size_bytes(), sizeof(queue_array));
  EXPECT_EQ(queue.max_size_bytes(),
            sizeof(uint32_t) * 10u - 1 /*prefix*/ - 1 /*end*/);
  EXPECT_EQ(queue.size_bytes(), 0u);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_TRUE(queue.empty());
}

TEST(InlineVarLenEntryQueueClass, Construct_Constexpr) {
  constexpr pw::InlineVarLenEntryQueue<127> queue(pw::kConstexpr);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.max_size(), 128u);
  EXPECT_EQ(queue.size(), 0u);
}

// Include tests of GenericVarLenEntryQueue, using a BasicInlineVarLenEntryQueue
// factory.
class BasicInlineVarLenEntryQueueTest
    : public pw::containers::test::GenericVarLenEntryQueueTest<
          BasicInlineVarLenEntryQueueTest> {
 public:
  template <typename T, size_t kMaxSizeBytes>
  pw::BasicInlineVarLenEntryQueue<T, kMaxSizeBytes> MakeQueue() {
    return pw::BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>();
  }
};
PW_GENERIC_VAR_LEN_ENTRY_QUEUE_TESTS(BasicInlineVarLenEntryQueueTest);

}  // namespace
