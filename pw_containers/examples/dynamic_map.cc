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

#include "pw_containers/dynamic_map.h"

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-dynamic_map]

struct Book {
  Book(const char* n, uint32_t o) : name(n), oclc(o) {}

  const char* name;
  uint32_t oclc;
};

void PopulateLibrary(pw::DynamicMap<uint32_t, Book>& library) {
  library.emplace(20848014u, "A Tale of Two Cities", 20848014u);
  library.emplace(182537909u, "The Little Prince", 182537909u);
  library.emplace(26857452u, "The Alchemist", 26857452u);
  library.emplace(
      44795766u, "Harry Potter and the Philosopher's Stone", 44795766u);
  library.emplace(47032439u, "And Then There Were None", 47032439u);
  library.emplace(20692970u, "Dream of the Red Chamber", 20692970u);
  library.emplace(1827184u, "The Hobbit", 1827184u);
  library.emplace(5635965u, "Alice's Adventures in Wonderland", 5635965u);
}

void VisitLibrary(pw::DynamicMap<uint32_t, Book>& library,
                  pw::DynamicMap<uint32_t, Book>& book_bag) {
  // Return any books we previously checked out.
  library.merge(book_bag);

  // Pick out some new books to read to the kids, but only if they're available.
  std::array<uint32_t, 3> oclcs = {
      1827184u,   // The Hobbit
      11914189u,  // Curious George
      44795766u,  // Harry Potter
  };

  for (uint32_t oclc : oclcs) {
    auto iter = library.find(oclc);
    if (iter != library.end()) {
      if (book_bag.try_insert(*iter)) {
        library.erase(iter);
      }
    }
  }
}

// DOCSTAG: [pw_containers-dynamic_map]

}  // namespace examples

namespace {

TEST(DynamicMapExampleTest, VisitLibrary) {
  pw::allocator::test::AllocatorForTest<4096> allocator;

  pw::DynamicMap<uint32_t, examples::Book> library(allocator);
  pw::DynamicMap<uint32_t, examples::Book> book_bag(allocator);

  examples::PopulateLibrary(library);
  book_bag.emplace(17522865u, "One Hundred Years of Solitude", 17522865u);

  examples::VisitLibrary(library, book_bag);

  auto iter = book_bag.begin();
  ASSERT_NE(iter, book_bag.end());
  EXPECT_STREQ((iter++)->second.name, "The Hobbit");

  ASSERT_NE(iter, book_bag.end());
  EXPECT_STREQ((iter++)->second.name,
               "Harry Potter and the Philosopher's Stone");

  EXPECT_EQ(iter, book_bag.end());
}

}  // namespace
