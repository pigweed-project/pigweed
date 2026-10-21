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

#include "pw_containers/intrusive_queue.h"

#include <cstddef>
#include <iterator>
#include <utility>

#include "pw_unit_test/framework.h"

namespace {

class Item : public pw::IntrusiveQueue<Item>::Item {
 public:
  constexpr Item() = default;
  constexpr Item(int number) : number_(number) {}

  Item(Item&& other) : Item() { *this = std::move(other); }
  Item& operator=(Item&& other) {
    number_ = other.number_;
    return *this;
  }

  int GetNumber() const { return number_; }
  void SetNumber(int num) { number_ = num; }

  bool operator==(const Item& other) const { return number_ == other.number_; }
  bool operator<(const Item& other) const { return number_ < other.number_; }

 private:
  int number_ = 0;
};

using Queue = pw::IntrusiveQueue<Item>;

TEST(IntrusiveQueueTest, PushBackAndFront) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);

  queue.push_back(i2);
  queue.push_back(i3);
  queue.push_front(i1);

  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.front().GetNumber(), 1);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.pop_front();
  EXPECT_EQ(queue.front().GetNumber(), 2);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.pop_front();
  EXPECT_EQ(queue.front().GetNumber(), 3);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.pop_front();
  EXPECT_TRUE(queue.empty());
}

TEST(IntrusiveQueueTest, ConstructInitializers) {
  Item i1(1);
  Item i2(2);
  Item i3(3);

  Queue queue({&i1, &i2, &i3});

  EXPECT_EQ(queue.front().GetNumber(), 1);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.clear();
  EXPECT_TRUE(queue.empty());
}

TEST(IntrusiveQueueTest, InsertAfter_UpdatesTail) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);

  queue.push_back(i1);

  // Insert after tail
  auto it_inserted = queue.insert_after(queue.begin(), i2);
  EXPECT_EQ(queue.back().GetNumber(), 2);
  EXPECT_EQ(it_inserted->GetNumber(), 2);

  // Insert multiple after tail
  Item items[] = {Item(4), Item(5)};
  queue.insert_after(it_inserted, {&items[0], &items[1]});
  EXPECT_EQ(queue.back().GetNumber(), 5);

  queue.clear();
}

TEST(IntrusiveQueueTest, EraseAfter_UpdatesTail) {
  Queue queue;
  Item i1(1);
  Item i2(2);

  queue.push_back(i1);
  queue.push_back(i2);

  // Erase the tail element
  queue.erase_after(queue.begin());
  EXPECT_EQ(queue.back().GetNumber(), 1);

  queue.erase_after(queue.before_begin());
  EXPECT_TRUE(queue.empty());

  queue.clear();
}

TEST(IntrusiveQueueTest, Remove_UpdatesTail) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);

  queue.push_back(i1);
  queue.push_back(i2);
  queue.push_back(i3);

  EXPECT_EQ(queue.back().GetNumber(), 3);

  // Remove middle item
  bool removed = queue.remove(i2);
  EXPECT_TRUE(removed);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  // Remove tail item
  removed = queue.remove(i3);
  EXPECT_TRUE(removed);
  EXPECT_EQ(queue.back().GetNumber(), 1);

  // Remove front item
  removed = queue.remove(i1);
  EXPECT_TRUE(removed);
  EXPECT_TRUE(queue.empty());

  queue.clear();
}

TEST(IntrusiveQueueTest, Swap) {
  Queue queue1;
  Item i1(1);
  queue1.push_back(i1);

  Queue queue2;
  Item i2(2);
  queue2.push_back(i2);

  queue1.swap(queue2);

  EXPECT_EQ(queue1.front().GetNumber(), 2);
  EXPECT_EQ(queue1.back().GetNumber(), 2);
  EXPECT_EQ(queue2.front().GetNumber(), 1);
  EXPECT_EQ(queue2.back().GetNumber(), 1);

  Queue queue3;  // empty
  queue1.swap(queue3);

  EXPECT_TRUE(queue1.empty());
  EXPECT_FALSE(queue3.empty());
  EXPECT_EQ(queue3.front().GetNumber(), 2);
  EXPECT_EQ(queue3.back().GetNumber(), 2);

  queue1.clear();
  queue2.clear();
  queue3.clear();
}

TEST(IntrusiveQueueTest, RemoveIf_UpdatesTail) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);
  Item i4(4);

  queue.push_back(i1);
  queue.push_back(i2);
  queue.push_back(i3);
  queue.push_back(i4);

  EXPECT_EQ(queue.back().GetNumber(), 4);

  // Remove even numbers (2 and 4)
  size_t removed = queue.remove_if(
      [](const Item& item) { return item.GetNumber() % 2 == 0; });

  EXPECT_EQ(removed, 2u);
  EXPECT_EQ(queue.back().GetNumber(), 3);
  EXPECT_EQ(queue.front().GetNumber(), 1);

  // Remove 1
  removed =
      queue.remove_if([](const Item& item) { return item.GetNumber() == 1; });
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(queue.front().GetNumber(), 3);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.clear();
}

TEST(IntrusiveQueueTest, Move) {
  Queue queue1;
  Item i1(1);
  Item i2(2);
  queue1.push_back(i1);
  queue1.push_back(i2);

  // Move constructor
  Queue queue2(std::move(queue1));
  EXPECT_TRUE(queue1.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(queue2.empty());
  EXPECT_EQ(queue2.front().GetNumber(), 1);
  EXPECT_EQ(queue2.back().GetNumber(), 2);

  // Move assignment
  Queue queue3;
  Item i3(3);
  queue3.push_back(i3);

  queue3 = std::move(queue2);
  EXPECT_TRUE(queue2.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(queue3.empty());
  EXPECT_EQ(queue3.front().GetNumber(), 1);
  EXPECT_EQ(queue3.back().GetNumber(), 2);

  queue3.clear();
}

TEST(IntrusiveQueueTest, Assign) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);

  // Assign range
  Item* items[] = {&i1, &i2, &i3};
  queue.assign(std::begin(items), std::end(items));

  EXPECT_EQ(queue.front().GetNumber(), 1);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  // Assign list
  Item i4(4);
  queue.assign({&i4});
  EXPECT_EQ(queue.front().GetNumber(), 4);
  EXPECT_EQ(queue.back().GetNumber(), 4);

  queue.clear();
}

TEST(IntrusiveQueueTest, Iterators) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  queue.push_back(i1);
  queue.push_back(i2);

  // Non-const iterators
  int count = 1;
  for (Item& item : queue) {
    EXPECT_EQ(item.GetNumber(), count++);
  }

  // Const iterators
  const Queue& const_queue = queue;
  count = 1;
  for (const Item& item : const_queue) {
    EXPECT_EQ(item.GetNumber(), count++);
  }

  // cbegin/cend
  count = 1;
  for (auto it = queue.cbegin(); it != queue.cend(); ++it) {
    EXPECT_EQ(it->GetNumber(), count++);
  }

  // before_begin
  auto it = queue.before_begin();
  ++it;
  EXPECT_EQ(it, queue.begin());

  queue.clear();
}

TEST(IntrusiveQueueTest, InsertAfter_Range) {
  Queue queue;
  Item i1(1);
  queue.push_back(i1);

  Item i2(2);
  Item i3(3);
  Item* items[] = {&i2, &i3};

  // Insert range after begin
  auto it =
      queue.insert_after(queue.begin(), std::begin(items), std::end(items));
  EXPECT_EQ(queue.back().GetNumber(), 3);
  EXPECT_EQ(it->GetNumber(), 3);

  // Verify contents: 1, 2, 3
  auto it_check = queue.begin();
  EXPECT_EQ(it_check->GetNumber(), 1);
  ++it_check;
  EXPECT_EQ(it_check->GetNumber(), 2);
  ++it_check;
  EXPECT_EQ(it_check->GetNumber(), 3);

  queue.clear();
}

TEST(IntrusiveQueueTest, InsertAfter_BeforeBegin) {
  Queue queue;
  Item i1(1);

  // Insert after before_begin into empty list
  auto it = queue.insert_after(queue.before_begin(), i1);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.front().GetNumber(), 1);
  EXPECT_EQ(queue.back().GetNumber(), 1);
  EXPECT_EQ(it->GetNumber(), 1);

  Item i2(2);
  queue.insert_after(queue.before_begin(), i2);
  EXPECT_EQ(queue.front().GetNumber(), 2);
  EXPECT_EQ(queue.back().GetNumber(), 1);  // 1 is still back

  queue.clear();
}

TEST(IntrusiveQueueTest, EraseAfter_Range) {
  Queue queue;
  Item i1(1);
  Item i2(2);
  Item i3(3);
  Item i4(4);

  queue.push_back(i1);
  queue.push_back(i2);
  queue.push_back(i3);
  queue.push_back(i4);

  // Erase from i1 to end (excludes i1)
  // erase_after(begin(), end()) -> erases i2, i3, i4
  auto it = queue.erase_after(queue.begin(), queue.end());
  EXPECT_EQ(queue.back().GetNumber(), 1);
  EXPECT_EQ(it, queue.end());

  queue.clear();
  queue.push_back(i1);
  queue.push_back(i2);
  queue.push_back(i3);

  // Erase middle (erase i2)
  auto it_mid = queue.begin();  // i1
  auto it_next = it_mid;
  ++it_next;
  ++it_next;  // i3
  queue.erase_after(it_mid, it_next);

  EXPECT_EQ(queue.front().GetNumber(), 1);
  EXPECT_EQ(queue.back().GetNumber(), 3);

  queue.clear();
}

TEST(IntrusiveQueueTest, ConstAccess) {
  Queue queue;
  Item i1(1);
  queue.push_back(i1);

  const Queue& const_queue = queue;
  EXPECT_EQ(const_queue.front().GetNumber(), 1);
  EXPECT_EQ(const_queue.back().GetNumber(), 1);

  queue.clear();
}

TEST(IntrusiveQueueTest, MaxSize) {
  Queue queue;
  EXPECT_GT(queue.max_size(), 0u);
}

}  // namespace
