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

// DOCSTAG: [pw_allocator-examples-pmr]
#include <map>
#include <string>
#include <string_view>

#include "pw_allocator/allocator.h"
#include "pw_allocator/pmr_allocator.h"

namespace examples {

class LibraryIndex {
 public:
  using StringType = ::std::pmr::string;
  using MapType = ::std::pmr::multimap<StringType, StringType>;
  using Iterator = typename MapType::iterator;

  LibraryIndex(pw::Allocator& allocator)
      : allocator_(allocator), by_author_(allocator_) {}

  void AddBook(std::string_view title, std::string_view author) {
    by_author_.emplace(author, title);
  }

  std::pair<Iterator, Iterator> FindByAuthor(std::string_view author) {
    return by_author_.equal_range(StringType(author, allocator_));
  }

 private:
  pw::allocator::PmrAllocator allocator_;
  MapType by_author_;
};

}  // namespace examples
// DOCSTAG: [pw_allocator-examples-pmr]

// Unit tests.
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<4096>;

TEST(PmrExample, FindBookByAuthor) {
  AllocatorForTest allocator;
  examples::LibraryIndex books(allocator);

  // Books which sold over 60M copies.
  // inclusive-language: disable
  books.AddBook("A Tale of Two Cities", "Charles Dickens");
  books.AddBook("Le Petit Prince", "Antoine de Saint-Exupery");
  books.AddBook("O Alquimista", "Paulo Coelho");
  books.AddBook("Harry Potter and the Philosopher's Stone", "J. K. Rowling");
  books.AddBook("And Then There Were None", "Agatha Christie");
  books.AddBook("Dream of the Red Chamber", "Cao Xueqin");
  books.AddBook("The Hobbit", "J. R. R. Tolkien");
  books.AddBook("She: A History of Adventure", "H. Rider Haggard");
  books.AddBook("The Da Vinci Code", "Dan Brown");
  books.AddBook("Harry Potter and the Chamber of Secrets", "J. K. Rowling");
  books.AddBook("Harry Potter and the Prisoner of Azkaban", "J. K. Rowling");
  books.AddBook("Harry Potter and the Goblet of Fire", "J. K. Rowling");
  books.AddBook("Harry Potter and the Order of the Phoenix", "J. K. Rowling");
  books.AddBook("Harry Potter and the Half-Blood Prince", "J. K. Rowling");
  books.AddBook("Harry Potter and the Deathly Hallows", "J. K. Rowling");
  books.AddBook("The Catcher in the Rye", "J. D. Salinger");
  // inclusive-language: enable

  examples::LibraryIndex::Iterator begin, end;
  std::tie(begin, end) = books.FindByAuthor("J. K. Rowling");
  EXPECT_EQ(std::distance(begin, end), 7);
}

}  // namespace
