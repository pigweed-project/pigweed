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

#include "pw_containers/intrusive_multiset.h"

#include <array>
#include <cstring>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-intrusive_multiset]

class Book : public pw::IntrusiveMultiSet<Book>::Item {
 private:
  using Item = pw::IntrusiveMultiSet<Book>::Item;

 public:
  explicit Book(const char* name) : name_(name) {}
  const char* name() const { return name_; }
  bool operator<(const Book& rhs) const {
    return strcmp(name_, rhs.name()) < 0;
  }

 private:
  const char* name_;
};

std::array<Book, 12> books = {
    Book("The Little Prince"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("Harry Potter and the Philosopher's Stone"),
    Book("The Hobbit"),
    Book("The Hobbit"),
    Book("The Hobbit"),
    Book("The Hobbit"),
    Book("Alice's Adventures in Wonderland"),
    Book("Alice's Adventures in Wonderland"),
};
pw::IntrusiveMultiSet<Book> library(books.begin(), books.end());

void VisitLibrary(pw::IntrusiveMultiSet<Book>& book_bag) {
  // Pick out some new books to read to the kids, but only if they're available.
  std::array<const char*, 3> titles = {
      "The Hobbit",
      "Alice's Adventures in Wonderland",
      "The Little Prince",
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

// DOCSTAG: [pw_containers-intrusive_multiset]

}  // namespace examples

namespace {

TEST(IntrusiveMultiSetExampleTest, VisitLibrary) {
  pw::IntrusiveMultiSet<examples::Book> book_bag1;
  examples::VisitLibrary(book_bag1);

  pw::IntrusiveMultiSet<examples::Book> book_bag2;
  examples::VisitLibrary(book_bag2);

  pw::IntrusiveMultiSet<examples::Book> book_bag3;
  examples::VisitLibrary(book_bag3);

  auto iter = book_bag1.begin();
  ASSERT_NE(iter, book_bag1.end());
  EXPECT_STREQ((iter++)->name(), "Alice's Adventures in Wonderland");
  ASSERT_NE(iter, book_bag1.end());
  EXPECT_STREQ((iter++)->name(), "The Hobbit");
  ASSERT_NE(iter, book_bag1.end());
  EXPECT_STREQ((iter++)->name(), "The Little Prince");
  EXPECT_EQ(iter, book_bag1.end());
  book_bag1.clear();

  iter = book_bag2.begin();
  ASSERT_NE(iter, book_bag2.end());
  EXPECT_STREQ((iter++)->name(), "Alice's Adventures in Wonderland");
  ASSERT_NE(iter, book_bag2.end());
  EXPECT_STREQ((iter++)->name(), "The Hobbit");
  EXPECT_EQ(iter, book_bag2.end());
  book_bag2.clear();

  iter = book_bag3.begin();
  ASSERT_NE(iter, book_bag3.end());
  EXPECT_STREQ((iter++)->name(), "The Hobbit");
  EXPECT_EQ(iter, book_bag3.end());
  book_bag3.clear();

  examples::library.clear();
}

}  // namespace
