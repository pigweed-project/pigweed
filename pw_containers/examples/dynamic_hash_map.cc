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

#include "pw_containers/dynamic_hash_map.h"

#include <array>
#include <cstdint>

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-dynamic_hash_map]

struct Book {
  Book(const char* n, uint32_t o) : name(n), oclc(o) {}

  const char* name;
  uint32_t oclc;
};

void PopulateLibrary(pw::DynamicHashMap<uint32_t, Book>& library) {
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

void VisitLibrary(pw::DynamicHashMap<uint32_t, Book>& library,
                  pw::DynamicHashMap<uint32_t, Book>& book_bag) {
  // Return any books we previously checked out.
  // The merge function moves elements from book_bag into library if the
  // key doesn't already exist in library.
  library.merge(book_bag);

  // Pick out some new books to read to the kids, but only if they're available.
  std::array<uint32_t, 3> oclcs = {
      1827184u,   // The Hobbit
      11914189u,  // Curious George (Not in library)
      44795766u,  // Harry Potter
  };

  for (uint32_t oclc : oclcs) {
    auto iter = library.find(oclc);
    if (iter != library.end()) {
      // Move the book from the library into our bag.
      // *iter refers to a pair<const Key, Value>.
      if (book_bag.try_insert(*iter)) {
        library.erase(iter);
      }
    }
  }
}

// DOCSTAG: [pw_containers-dynamic_hash_map]

}  // namespace examples

namespace {

TEST(DynamicHashMapExampleTest, VisitLibrary) {
  pw::allocator::test::AllocatorForTest<4096> allocator;

  // Initialize maps with the allocator.
  pw::DynamicHashMap<uint32_t, examples::Book> library(allocator);
  pw::DynamicHashMap<uint32_t, examples::Book> book_bag(allocator);

  examples::PopulateLibrary(library);

  // Simulate having a book already checked out in the bag.
  book_bag.emplace(17522865u, "One Hundred Years of Solitude", 17522865u);

  examples::VisitLibrary(library, book_bag);

  // Verify the contents of the book bag.
  // "One Hundred Years of Solitude" was returned to the library in the merge()
  // step and was NOT checked back out.
  // "The Hobbit" and "Harry Potter" were checked out.

  EXPECT_EQ(book_bag.size(), 2u);
  EXPECT_TRUE(book_bag.contains(1827184u));   // The Hobbit
  EXPECT_TRUE(book_bag.contains(44795766u));  // Harry Potter

  // Verify "One Hundred Years of Solitude" is now in the library.
  EXPECT_TRUE(library.contains(17522865u));

  // Verify the checked-out books were removed from the library.
  EXPECT_FALSE(library.contains(1827184u));
  EXPECT_FALSE(library.contains(44795766u));

  // Verify Curious George was not added (it wasn't in the library).
  EXPECT_FALSE(book_bag.contains(11914189u));
}

}  // namespace
