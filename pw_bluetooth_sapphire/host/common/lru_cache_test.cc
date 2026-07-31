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

#include "pw_bluetooth_sapphire/internal/host/common/lru_cache.h"

#include <memory>
#include <string>

#include "pw_unit_test/framework.h"

namespace bt {
namespace {

TEST(LruCacheTest, BasicPutAndGet) {
  LruCache<int, std::string> cache(3);
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(0u, cache.size());
  EXPECT_EQ(3u, cache.max_size());

  cache.put(1, "one");
  EXPECT_FALSE(cache.empty());
  EXPECT_EQ(1u, cache.size());
  EXPECT_TRUE(cache.contains(1));
  EXPECT_FALSE(cache.contains(2));

  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ("one", val->get());

  auto peek_val = cache.peek(1);
  ASSERT_TRUE(peek_val.has_value());
  EXPECT_EQ("one", peek_val->get());
}

TEST(LruCacheTest, UpdateExistingKey) {
  LruCache<int, std::string> cache(2);
  cache.put(1, "one");
  EXPECT_EQ(1u, cache.size());

  cache.put(1, "uno");
  EXPECT_EQ(1u, cache.size());

  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ("uno", val->get());
}

TEST(LruCacheTest, EvictsLeastRecentlyUsed) {
  LruCache<int, int> cache(3);
  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);
  EXPECT_EQ(3u, cache.size());
  EXPECT_TRUE(cache.contains(1));

  // Inserting a 4th item should evict the oldest item (key 1).
  cache.put(4, 400);
  EXPECT_EQ(3u, cache.size());
  EXPECT_FALSE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
  EXPECT_TRUE(cache.contains(4));
}

TEST(LruCacheTest, GetUpdatesLruOrder) {
  LruCache<int, int> cache(3);
  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);

  // Access key 1, making it the most recently used. Now key 2 is least recently
  // used.
  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(100, val->get());

  cache.put(4, 400);
  EXPECT_EQ(3u, cache.size());
  EXPECT_TRUE(cache.contains(1));
  EXPECT_FALSE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
  EXPECT_TRUE(cache.contains(4));
}

TEST(LruCacheTest, PutExistingUpdatesLruOrder) {
  LruCache<int, int> cache(3);
  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);

  // Re-inserting key 1 updates its value and makes it most recently used.
  cache.put(1, 101);

  cache.put(4, 400);
  EXPECT_EQ(3u, cache.size());
  EXPECT_TRUE(cache.contains(1));
  EXPECT_FALSE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
  EXPECT_TRUE(cache.contains(4));

  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(101, val->get());
}

TEST(LruCacheTest, PeekDoesNotUpdateLruOrder) {
  LruCache<int, int> cache(3);
  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);

  // Peek key 1. This should not alter LRU ordering, so key 1 remains least
  // recently used.
  auto val = cache.peek(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(100, val->get());

  cache.put(4, 400);
  EXPECT_EQ(3u, cache.size());
  EXPECT_FALSE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
  EXPECT_TRUE(cache.contains(4));
}

TEST(LruCacheTest, Remove) {
  LruCache<int, std::string> cache(3);
  cache.put(1, "one");
  cache.put(2, "two");
  EXPECT_EQ(2u, cache.size());

  EXPECT_TRUE(cache.remove(1));
  EXPECT_EQ(1u, cache.size());
  EXPECT_FALSE(cache.contains(1));

  EXPECT_FALSE(cache.remove(1));
  EXPECT_EQ(1u, cache.size());
}

TEST(LruCacheTest, Clear) {
  LruCache<int, int> cache(3);
  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);
  EXPECT_EQ(3u, cache.size());

  cache.clear();
  EXPECT_EQ(0u, cache.size());
  EXPECT_TRUE(cache.empty());
  EXPECT_FALSE(cache.contains(1));
  EXPECT_FALSE(cache.contains(2));
  EXPECT_FALSE(cache.contains(3));
}

TEST(LruCacheTest, MoveOnlyValueType) {
  LruCache<int, std::unique_ptr<int>> cache(2);
  cache.put(1, std::make_unique<int>(10));
  cache.put(2, std::make_unique<int>(20));
  EXPECT_EQ(2u, cache.size());

  auto val = cache.get(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(10, *(val->get()));

  cache.put(3, std::make_unique<int>(30));
  EXPECT_FALSE(cache.contains(2));
  EXPECT_TRUE(cache.contains(1));
  EXPECT_TRUE(cache.contains(3));
}

}  // namespace
}  // namespace bt
