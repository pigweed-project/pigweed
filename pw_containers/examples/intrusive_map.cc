// Copyright 2024 The Pigweed Authors
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

#include "pw_containers/intrusive_map.h"

#include <array>
#include <cstdint>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-intrusive_map]

struct Book : public pw::IntrusiveMap<uint32_t, Book>::Pair {
 private:
  using Pair = pw::IntrusiveMap<uint32_t, Book>::Pair;

 public:
  Book(const char* name, uint32_t oclc) : Pair(oclc), name_(name) {}
  const char* name() const { return name_; }

 private:
  const char* name_;
};

std::array<Book, 8> books = {{
    {"A Tale of Two Cities", 20848014u},
    {"The Little Prince", 182537909u},
    {"The Alchemist", 26857452u},
    {"Harry Potter and the Philosopher's Stone", 44795766u},
    {"And Then There Were None", 47032439u},
    {"Dream of the Red Chamber", 20692970u},
    {"The Hobbit", 1827184u},
    {"Alice's Adventures in Wonderland", 5635965u},
}};

pw::IntrusiveMap<uint32_t, Book> library(books.begin(), books.end());

void VisitLibrary(pw::IntrusiveMap<uint32_t, Book>& book_bag) {
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
      Book& book = *iter;
      library.erase(iter);
      book_bag.insert(book);
    }
  }
}

// DOCSTAG: [pw_containers-intrusive_map]

}  // namespace examples

namespace {

TEST(IntrusiveMapExampleTest, VisitLibrary) {
  examples::Book book = {"One Hundred Years of Solitude", 17522865u};
  pw::IntrusiveMap<uint32_t, examples::Book> book_bag;
  book_bag.insert(book);

  examples::VisitLibrary(book_bag);
  auto iter = book_bag.begin();
  EXPECT_STREQ((iter++)->name(), "The Hobbit");
  EXPECT_STREQ((iter++)->name(), "Harry Potter and the Philosopher's Stone");
  EXPECT_EQ(iter, book_bag.end());

  // Remove books before items go out scope.
  book_bag.clear();
  examples::library.clear();
}

}  // namespace
