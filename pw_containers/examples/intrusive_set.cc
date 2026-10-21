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

#include "pw_containers/intrusive_set.h"

#include <array>
#include <cstring>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-intrusive_set]

class Book : public pw::IntrusiveSet<Book>::Item {
 private:
  using Item = pw::IntrusiveSet<Book>::Item;

 public:
  explicit Book(const char* name) : name_(name) {}
  const char* name() const { return name_; }
  bool operator<(const Book& rhs) const {
    return strcmp(name_, rhs.name()) < 0;
  }

 private:
  const char* name_;
};

std::array<Book, 8> books = {
    Book("A Tale of Two Cities"),
    Book("The Little Prince"),
    Book("The Alchemist"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("And Then There Were None"),
    Book("Dream of the Red Chamber"),
    Book("The Hobbit"),
    Book("Alice's Adventures in Wonderland"),
};

pw::IntrusiveSet<Book> library(books.begin(), books.end());

void VisitLibrary(pw::IntrusiveSet<Book>& book_bag) {
  // Return any books we previously checked out.
  library.merge(book_bag);

  // Pick out some new books to read to the kids, but only if they're available.
  std::array<const char*, 3> titles = {
      "The Hobbit",
      "Curious George",
      "Harry Potter and the Philosopher's Stone",
  };
  for (const char* title : titles) {
    Book requested(title);
    auto iter = library.find(requested);
    if (iter != library.end()) {
      Book& book = *iter;
      library.erase(iter);
      book_bag.insert(book);
    }
  }
}

// DOCSTAG: [pw_containers-intrusive_set]

}  // namespace examples

namespace {

TEST(IntrusiveMapExampleTest, VisitLibrary) {
  examples::Book book("One Hundred Years of Solitude");
  pw::IntrusiveSet<examples::Book> book_bag;
  book_bag.insert(book);

  examples::VisitLibrary(book_bag);
  auto iter = book_bag.begin();
  EXPECT_STREQ((iter++)->name(), "Harry Potter and the Philosopher's Stone");
  EXPECT_STREQ((iter++)->name(), "The Hobbit");
  EXPECT_EQ(iter, book_bag.end());

  // Remove books before items go out scope.
  book_bag.clear();
  examples::library.clear();
}

}  // namespace
