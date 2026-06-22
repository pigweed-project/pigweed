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
#include <optional>
#include <tuple>

#include "pw_bytes/span.h"
#include "pw_multibuf/v1_adapter/chunk.h"
#include "pw_multibuf/v1_adapter/internal/traits.h"
#include "pw_multibuf/v2/chunks.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_status/status_with_size.h"

namespace pw::multibuf::v1_adapter {

/// @submodule{pw_multibuf,v1_adapter}

/// A `Chunk`-oriented view of a `MultiBuf` that mimics `v1::MultiBufChunks`.
///
/// This type mimics the public API of `v1::MultiBuf` that derives from the
/// privately inherited `v1::MultiBufChunks`, rather than the API of
/// `v1::MultiBufChunks` itself.
///
/// This function can be used as a drop-in replacement for `v1::MultiBuf` while
/// migrating to using pw_multibuf/v2.
class MultiBufChunks {
 private:
  using Entry = v2::internal::Entry;
  using Mutability = v2::internal::Mutability;
  using Deque = Entry::Deque;

  /// A possibly const `std::forward_iterator` over the `Chunk`s of a
  /// `MultiBuf`.
  template <Mutability kMutability>
  class Iterator {
   public:
    using size_type = Deque::size_type;
    using difference_type = Deque::difference_type;
    using value_type = std::
        conditional_t<kMutability == Mutability::kConst, const Chunk, Chunk>;
    using reference = value_type&;
    using pointer = value_type*;
    using iterator_category = std::forward_iterator_tag;

    constexpr Iterator() = default;

    // Support converting non-const iterators to const_iterators.
    operator Iterator<Mutability::kConst>() const {
      return mbv2_ == nullptr ? Iterator<Mutability::kConst>()
                              : Iterator<Mutability::kConst>(*mbv2_, index_);
    }

    constexpr reference operator*() const {
      PW_ASSERT(chunk_.has_value());
      return const_cast<reference>(*chunk_);
    }
    constexpr pointer operator->() const {
      PW_ASSERT(chunk_.has_value());
      return const_cast<pointer>(&(*chunk_));
    }

    constexpr Iterator& operator++() {
      ++index_;
      Update();
      return *this;
    }

    constexpr Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    constexpr bool operator==(const Iterator& other) const {
      return mbv2_ == other.mbv2_ && index_ == other.index_;
    }

    constexpr bool operator!=(const Iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class MultiBufChunks;

    constexpr Iterator(const v2::internal::GenericMultiBuf& mbv2,
                       size_type chunk)
        : mbv2_(&mbv2), index_(chunk) {
      PW_ASSERT(chunk <= mbv2_->num_chunks());
      Update();
    }

    // Updates the chunk to match the current index.
    constexpr void Update();

    const v2::internal::GenericMultiBuf* mbv2_ = nullptr;
    size_type index_ = 0;
    std::optional<Chunk> chunk_;
  };

 public:
  using iterator = Iterator<Mutability::kMutable>;
  using const_iterator = Iterator<Mutability::kConst>;

  ~MultiBufChunks() { Release(); }

  MultiBufChunks(MultiBufChunks&& other) = default;
  MultiBufChunks& operator=(MultiBufChunks&& other) = default;

  /// Returns the v2 MultiBuf used to implement both the v1 and v2 APIs.
  ///
  /// This can be useful for methods that are only in the v1 API of this type
  /// due to name collisions, such as `empty()`, `size()`, and `Chunks()`.
  ///
  /// @code{.cpp}
  ///   for (auto chunk : mb.Chunks() {
  ///     // Iterate over raw chunks of memory as in v1.
  ///   }
  ///   for (auto chunk : mb.v2().Chunks() {
  ///     // Iterate over non-empty chunks of contiguous memory as in v2.
  ///   }
  /// @endcode
  constexpr v2::TrackedMultiBuf* v2() {
    return mbv2_.has_value() ? &(**mbv2_) : nullptr;
  }
  constexpr const v2::TrackedMultiBuf* v2() const {
    return mbv2_.has_value() ? &(**mbv2_) : nullptr;
  }

  /// @copydoc ::v1::MultiBufChunks::size
  constexpr size_t size() const;

  Chunk& front() {
    first_ = *begin();
    return *first_;
  }

  Chunk& back() {
    const auto* mbv2 = generic_v2();
    last_ = *iterator(*mbv2, mbv2->num_chunks() - 1);
    return *last_;
  }

  iterator begin() const {
    const auto* mbv2 = generic_v2();
    return mbv2 == nullptr ? iterator() : iterator(*mbv2, 0);
  }
  iterator end() const {
    const auto* mbv2 = generic_v2();
    return mbv2 == nullptr ? iterator() : iterator(*mbv2, mbv2->num_chunks());
  }

  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

 protected:
  constexpr MultiBufChunks() = default;

  constexpr const v2::internal::GenericMultiBuf* generic_v2() const {
    return mbv2_.has_value() ? &(mbv2_->base_) : nullptr;
  }
  constexpr v2::internal::GenericMultiBuf* generic_v2() {
    return mbv2_.has_value() ? &(mbv2_->base_) : nullptr;
  }

  v2::TrackedMultiBuf* Assign(Allocator& allocator) {
    mbv2_ = v2::TrackedMultiBuf::Instance(allocator);
    return &(**mbv2_);
  }

  template <v2::Property... kProperties>
  constexpr v2::TrackedMultiBuf* Assign(
      v2::BasicMultiBuf<kProperties...>&& mb) {
    mbv2_ = std::move(mb);
    return &(**mbv2_);
  }

  template <typename MultiBufType>
  constexpr v2::TrackedMultiBuf* Assign(
      v2::internal::Instance<MultiBufType>&& mbi) {
    mbv2_ = std::move(*mbi);
    return &(**mbv2_);
  }

  void Release() noexcept { mbv2_.reset(); }

 private:
  std::optional<v2::TrackedMultiBuf::Instance> mbv2_;
  std::optional<Chunk> first_;
  std::optional<Chunk> last_;
};

/// A byte buffer optimized for zero-copy data transfer that mimics
/// `v1::MultiBuf`.
///
/// This function can be used as a drop-in replacement for `v1::MultiBuf` while
/// migrating to using pw_multibuf/v2.
///
/// Internally, this object wraps a `v2::MultiBuf` and exposes a portion of its
/// API. In particular, it exposes the `TryReserveChunks()` method, which
/// fallibly allocates space for chunks. Infallible v1 methods like
/// `PushBackChunk()` will assert on allocation failure.
///
/// As a result, once a component has switched to using this type, it is
/// strongly recommended to follow up by refactoring to provide an allocator to
/// `v1_adapter::MultiBuf` at construction and reserving memory for chunks
/// before inserting them.
class MultiBuf final : private MultiBufChunks {
 private:
  using Deque = v2::MultiBuf::Deque;

 public:
  using size_type = v2::MultiBuf::size_type;
  using difference_type = v2::MultiBuf::difference_type;
  using iterator = v2::MultiBuf::iterator;
  using const_iterator = v2::MultiBuf::const_iterator;
  using pointer = v2::MultiBuf::pointer;
  using const_pointer = v2::MultiBuf::const_pointer;
  using reference = v2::MultiBuf::reference;
  using const_reference = v2::MultiBuf::const_reference;
  using value_type = v2::MultiBuf::value_type;

  MultiBuf(const MultiBuf& other) = delete;
  MultiBuf& operator=(const MultiBuf& other) = delete;

  MultiBuf(MultiBuf&& other) noexcept = default;
  MultiBuf& operator=(MultiBuf&& other) noexcept = default;

  ~MultiBuf() = default;

  // v1 API ////////////////////////////////////////////////////////////////////

  /// @copydoc v1::MultiBuf::FromChunk
  static MultiBuf FromChunk(OwnedChunk&& chunk);

  constexpr MultiBuf() = default;

  /// @copydoc v1::MultiBuf::Release
  void Release() noexcept { MultiBufChunks::Release(); }

  /// @copydoc v1::MultiBuf::size
  [[nodiscard]] constexpr size_t size() const {
    const auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->size() : 0;
  }

  /// @copydoc v1::MultiBuf::empty
  [[nodiscard]] constexpr bool empty() const {
    // `v2::MultiBuf::empty()` returns true for multibufs that only have one or
    // more empty chunks, so use `size()` instead.
    // NOLINTNEXTLINE(readability-container-size-empty)
    return size() == 0;
  }

  /// @copydoc v1::MultiBuf::IsContiguous
  [[nodiscard]] bool IsContiguous() const {
    return ContiguousSpan().has_value();
  }

  /// @copydoc v1::MultiBuf::ContiguousSpan
  std::optional<ByteSpan> ContiguousSpan();
  std::optional<ConstByteSpan> ContiguousSpan() const;

  /// @copydoc v1::MultiBuf::begin
  constexpr iterator begin() {
    auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->begin() : iterator();
  }
  constexpr const_iterator begin() const {
    const auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->begin() : const_iterator();
  }
  constexpr const_iterator cbegin() const {
    const auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->cbegin() : const_iterator();
  }

  /// @copydoc v1::MultiBuf::end
  constexpr iterator end() {
    auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->end() : iterator();
  }
  constexpr const_iterator end() const {
    const auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->end() : const_iterator();
  }
  constexpr const_iterator cend() const {
    const auto* mbv2 = v2();
    return mbv2 != nullptr ? mbv2->end() : const_iterator();
  }

  /// @copydoc v1::MultiBuf::ClaimPrefix
  [[nodiscard]] bool ClaimPrefix(size_t bytes_to_claim);

  /// @copydoc v1::MultiBuf::ClaimSuffix
  [[nodiscard]] bool ClaimSuffix(size_t bytes_to_claim);

  /// @copydoc v1::MultiBuf::DiscardPrefix
  void DiscardPrefix(size_t bytes_to_discard);

  /// @copydoc v1::MultiBuf::Slice
  void Slice(size_t begin, size_t end);

  /// @copydoc v1::MultiBuf::Truncate
  void Truncate(size_t len);

  /// @copydoc v1::MultiBuf::TruncateAfter
  void TruncateAfter(iterator pos);

  /// @copydoc v1::MultiBuf::TakePrefix
  std::optional<MultiBuf> TakePrefix(size_t bytes_to_take);

  /// @copydoc v1::MultiBuf::TakeSuffix
  std::optional<MultiBuf> TakeSuffix(size_t bytes_to_take);

  /// @copydoc v1::MultiBuf::PushPrefix
  void PushPrefix(MultiBuf&& front);

  /// @copydoc v1::MultiBuf::PushSuffix
  void PushSuffix(MultiBuf&& tail);

  /// @copydoc v1::MultiBuf::CopyTo
  StatusWithSize CopyTo(ByteSpan dest, size_t position = 0) const;

  /// @copydoc v1::MultiBuf::CopyFrom
  StatusWithSize CopyFrom(ConstByteSpan source, size_t position = 0);

  /// @copydoc v1::MultiBuf::CopyFromAndTruncate
  StatusWithSize CopyFromAndTruncate(ConstByteSpan source, size_t position = 0);

  /// @copydoc v1::MultiBuf::PushFrontChunk
  void PushFrontChunk(OwnedChunk&& chunk);

  /// @copydoc v1::MultiBuf::PushBackChunk
  void PushBackChunk(OwnedChunk&& chunk);

  /// @copydoc v1::MultiBuf::TakeFrontChunk
  OwnedChunk TakeFrontChunk() {
    return std::get<OwnedChunk>(TakeChunk(Chunks().begin()));
  }

  /// @copydoc v1::MultiBuf::InsertChunk
  MultiBufChunks::iterator InsertChunk(MultiBufChunks::iterator position,
                                       OwnedChunk&& chunk);

  /// @copydoc v1::MultiBuf::TakeChunk
  std::tuple<MultiBufChunks::iterator, OwnedChunk> TakeChunk(
      MultiBufChunks::iterator position);

  /// @copydoc v1::MultiBuf::Chunks
  constexpr MultiBufChunks& Chunks() { return *this; }

  /// @copydoc v1::MultiBuf::Chunks
  constexpr const MultiBufChunks& Chunks() const { return *this; }

  /// @copydoc v1::MultiBuf::ConstChunks
  constexpr const MultiBufChunks& ConstChunks() const { return *this; }

  // v2 API ////////////////////////////////////////////////////////////////////

  explicit MultiBuf(Allocator& allocator) { Assign(allocator); }

  template <v2::Property... kProperties>
  constexpr MultiBuf(v2::BasicMultiBuf<kProperties...>&& mb) {
    *this = std::move(mb);
  }

  template <v2::Property... kProperties>
  constexpr MultiBuf& operator=(v2::BasicMultiBuf<kProperties...>&& mb) {
    Assign(std::move(mb));
    return *this;
  }

  template <typename MultiBufType>
  constexpr MultiBuf(v2::internal::Instance<MultiBufType>&& mbi) {
    *this = std::move(mbi);
  }

  template <typename MultiBufType>
  constexpr MultiBuf& operator=(v2::internal::Instance<MultiBufType>&& mbi) {
    Assign(std::move(mbi));
    return *this;
  }

  constexpr v2::TrackedMultiBuf* operator->() {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return mbv2;
  }
  constexpr const v2::TrackedMultiBuf* operator->() const {
    const auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return mbv2;
  }

  constexpr v2::TrackedMultiBuf& operator*() & {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return *mbv2;
  }
  constexpr const v2::TrackedMultiBuf& operator*() const& {
    const auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return *mbv2;
  }

  constexpr v2::TrackedMultiBuf&& operator*() && {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return std::move(*mbv2);
  }
  constexpr const v2::TrackedMultiBuf&& operator*() const&& {
    const auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return std::move(*mbv2);
  }

  template <typename OtherMultiBuf,
            typename = v2::internal::EnableIfConvertible<v2::TrackedMultiBuf,
                                                         OtherMultiBuf>>
  constexpr operator OtherMultiBuf&() & {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return *mbv2;
  }

  template <typename OtherMultiBuf,
            typename = v2::internal::EnableIfConvertible<v2::TrackedMultiBuf,
                                                         OtherMultiBuf>>
  constexpr operator const OtherMultiBuf&() const& {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return *mbv2;
  }

  template <typename OtherMultiBuf,
            typename = v2::internal::EnableIfConvertible<v2::TrackedMultiBuf,
                                                         OtherMultiBuf>>
  constexpr operator OtherMultiBuf&&() && {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return std::move(*mbv2);
  }

  template <typename OtherMultiBuf,
            typename = v2::internal::EnableIfConvertible<v2::TrackedMultiBuf,
                                                         OtherMultiBuf>>
  constexpr operator const OtherMultiBuf&&() const&& {
    auto* mbv2 = v2();
    PW_ASSERT(mbv2 != nullptr);
    return std::move(*mbv2);
  }

 private:
  /// Converts a chunks iterator to a byte iterator.
  iterator ToByteIterator(const MultiBufChunks::iterator& position);

  /// Converts a byte iterator to a chunks iterator.
  MultiBufChunks::iterator ToChunksIterator(const const_iterator& position);

  size_t offset_ = 0;
};

/// @endsubmodule

// Template and constexpr method implementations.

template <v2::internal::Mutability kMutability>
constexpr void MultiBufChunks::Iterator<kMutability>::Update() {
  // Check if this iterator is valid.
  if (mbv2_ == nullptr || index_ >= mbv2_->num_chunks()) {
    chunk_ = std::nullopt;
    return;
  }

  // Check if the last chunk of the v2 multibuf is empty.
  auto pos = mbv2_->MakeIterator(index_);
  if (pos == mbv2_->cend()) {
    chunk_ = Chunk(mbv2_->get_allocator(), SharedPtr<std::byte[]>());
    return;
  }

  // Make a Chunk that corresponds to the v2 chunk.
  chunk_ = Chunk(mbv2_->get_allocator(), mbv2_->Share(pos));
  size_type start = mbv2_->GetOffset(index_);
  size_type end = start + mbv2_->GetLength(index_);
  chunk_->Slice(start, end);
}

constexpr size_t MultiBufChunks::size() const {
  const auto* mbv2 = generic_v2();
  if (mbv2 == nullptr) {
    return 0;
  }
  size_t size = 0;
  for (Entry::size_type i = 0; i < mbv2->num_chunks(); ++i) {
    if (mbv2->GetLength(i) != 0) {
      ++size;
    }
  }
  return size;
}

}  // namespace pw::multibuf::v1_adapter
