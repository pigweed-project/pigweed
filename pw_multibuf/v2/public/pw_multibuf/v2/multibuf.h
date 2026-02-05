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

// NOTE: MultiBuf version 2 is in a nearly stable state, but may still change.
// The Pigweed team will announce breaking changes in the API. It is strongly
// recommended to coordinate with the Pigweed team if you want to start using
// this code.

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <type_traits>

#include "pw_allocator/allocator.h"
#include "pw_allocator/null_allocator.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_multibuf/v2/chunks.h"
#include "pw_multibuf/v2/internal/byte_iterator.h"
#include "pw_multibuf/v2/internal/entry.h"
#include "pw_multibuf/v2/observer.h"
#include "pw_multibuf/v2/properties.h"

namespace pw::multibuf::v2 {

// Forward declarations.
template <Property...>
class BasicMultiBuf;

/// @submodule{pw_multibuf,v2}

// Type aliases of the BasicMultiBuf interface types for each different
// combination of propeties, listed here for easier discoverability.

/// Basic MultiBuf interface with mutable data.
using FlatMultiBuf = BasicMultiBuf<>;

/// Basic MultiBuf interface with read-only data.
using FlatConstMultiBuf = BasicMultiBuf<Property::kConst>;

/// MultiBuf interface with mutable data and the option of adding layered data
/// views.
using MultiBuf = BasicMultiBuf<Property::kLayerable>;

/// MultiBuf interface with read-only data and the option of adding layered data
/// views.
using ConstMultiBuf = BasicMultiBuf<Property::kConst, Property::kLayerable>;

/// Basic MultiBuf interface with mutable data that notifies its observer, if
/// set, on change.
using TrackedFlatMultiBuf = BasicMultiBuf<Property::kObservable>;

/// Basic MultiBuf interface with read-only data that notifies its observer, if
/// set, on change.
using TrackedFlatConstMultiBuf =
    BasicMultiBuf<Property::kConst, Property::kObservable>;

/// Basic MultiBuf interface with mutable data that notifies its observer, if
/// set, on change. It has the option of adding layered data views.
using TrackedMultiBuf =
    BasicMultiBuf<Property::kLayerable, Property::kObservable>;

/// Basic MultiBuf interface with read-only data that notifies its observer, if
/// set, on change. It has the option of adding layered data views.
using TrackedConstMultiBuf = BasicMultiBuf<Property::kConst,
                                           Property::kLayerable,
                                           Property::kObservable>;

namespace internal {

class GenericMultiBuf;

template <typename>
class Instance;

}  // namespace internal

/// Logical byte sequence representing a sequence of memory buffers.
///
/// MultiBufs have been designed with network data processing in mind. They
/// facilitate assembling and disassembling multiple buffers to and from
/// a single sequence. This class refers to these individual spans of bytes as
/// `Chunks`.
///
/// Small amounts of this data, such as protocol headers, can be accessed using
/// methods such as ``Get``, ``Visit``, and the byte iterators. These methods
/// avoid copying unless strictly necessary, such as when operating over chunks
/// that are not contiguous in memory.
///
/// Larger amounts of data, such as application data, can be accessed using
/// methods such as ``CopyFrom``, ``CopyTo``, and the chunk iterators.
///
/// MultiBufs are defined with zero or more properties which add or constrain
/// additional behavior:
///
/// - \em Property::kConst: A const MultiBuf contains read-only data. The
///   structure of the MultiBuf may still be changed, but the data within the
///   chunks may not. Certain methods that modify or allow mutable access to the
///   data, such as the non-const ``operator[]`` and ``CopyFrom``, are not
///   available when this property is set.
/// - \em Property::kLayerable: A layerable MultiBuf can add or remove "layers".
///   Each layer acts as a subspan of the layer or MultiBuf below it. These
///   layers can be used to represent encapsulated protocols, such as TCP
///   segments within IP packets spread across multiple Ethernet frames.
/// - \em Property::kObservable: And observable MultiBuf can have an observer
///   set on it that it will notify whenever bytes or layers are added or
///   removed.
///
/// Type aliases are provided for standard combinations of these properties.
///
/// A MultiBuf may be converted to another with different properties using the
/// ``as`` method, provided those properties are compatible. This allows writing
/// method and function signatures that only specify the necessary behavior. For
/// example:
///
/// @code{.cpp}
/// extern void AdjustLayers(LayerableMultiBuf& mb);
///
/// MyMultiBufInstance mb = InitMyMultiBufInstance();
/// AdjustLayers(mb.as<LayerableMultiBuf>());
/// @endcode
///
/// In order to provide for such conversions, this class only represents the
/// \em interface of a particular MultiBuf type, and not its instantiation.
/// To create a concrete instantiation of ``BasicMultiBuf<kProperties>``, use
/// ``Instance<BasicMultiBuf<kProperties>>``, as described below.
///
/// MultiBufs are designed to be built either "bottom-up" or "top-down":
///
/// - A \em "bottom-up" approach may concatenate together several layerable
///   MultiBufs representing lower-level protocol messages, and then add layers
///   to present the amalgamated payloads as a higher-level protocol or
///   application message.
/// - A \em "top-down" approach may take a higher-level protocol or application
///   message and insert headers and footers into it in order to represent a
///   sequence of lower-level protocol messages. Alternatively, another approach
///   would be to successively remove lower-level protocol payloads from the
///   higher-level message and add headers and footers to those.
///
/// The "bottom-up" approach also provides the concept of message "fragments".
/// Whenever a layer is added, a MultiBuf considers the new top layer to be a
/// "fragment". These boundaries are preserved when appending additional data,
/// and can be used to break the MultiBuf back up into lower-level messages when
/// writing data to the network.
///
/// For example, consider a TCP/IP example. You could use a "bottom-up",
/// layerable MultiBuf to access and manipulate TCP data. In the following:
///   * "TCP n" represents the n-th TCP segment of a stream.
///   * "IP m.n" represents the n-th fragment of the m-th IP packet.
///   * "Ethernet n" represents the n-th Ethernet frame.
///
/// @code
/// Layer 3:  +----+ TCP 0 +----+ TCP 0 +----+ TCP 0 +----+ TCP 1 +----+ TCP 1 +
/// Layer 2:  +--+  IP 0.0 +--+  IP 0.1 +--+  IP 0.2 +--+  IP 1.0 +--+  IP 1.1 +
/// Layer 1:  + Ethernet 0 + Ethernet 1 + Ethernet 2 + Ethernet 3 + Ethernet 4 +
///           + 0x5CAFE000 + 0x5CAFE400 + 0x5CAFE800 + 0x5CAFEC00 + 0x5CAFF000 +
/// @endcode
///
/// Alterantively, you could use a "top-down", unlayered MultiBuf to represent
/// same. The following might be the result of adding statically owned headers
/// and footers to a portion of the application data:
///
/// @code
/// No layer: + Eth header + IP header  + TCP header + App data   + Eth footer +
///           + 0x5AAA0000 + 0x5AAA0020 + 0x5AAA0050 + 0x5CAFEC00 + 0x5AAA0040 +
/// @endcode
///
/// @tparam   kProperties   Zero or more ``Property`` values. These must not be
///                         duplicated, and must appear in the order specified
///                         by that type.
template <Property... kProperties>
class BasicMultiBuf {
 protected:
  using Deque = DynamicDeque<internal::Entry>;
  using GenericMultiBuf = internal::GenericMultiBuf;

 public:
  /// Returns whether the MultiBuf data is immutable.
  [[nodiscard]] static constexpr bool is_const() {
    return ((kProperties == Property::kConst) || ...);
  }

  /// Returns whether additional views can be layered on the MultiBuf.
  [[nodiscard]] static constexpr bool is_layerable() {
    return ((kProperties == Property::kLayerable) || ...);
  }

  /// Returns whether an observer can be registered on the MultiBuf.
  [[nodiscard]] static constexpr bool is_observable() {
    return ((kProperties == Property::kObservable) || ...);
  }

  using size_type = typename Deque::size_type;
  using difference_type = typename Deque::difference_type;
  using iterator =
      internal::ByteIterator<size_type, internal::ChunkMutability::kMutable>;
  using const_iterator =
      internal::ByteIterator<size_type, internal::ChunkMutability::kConst>;
  using pointer = iterator::pointer;
  using const_pointer = const_iterator::pointer;
  using reference = iterator::reference;
  using const_reference = const_iterator::reference;
  using value_type = std::conditional_t<is_const(),
                                        const_iterator::value_type,
                                        iterator::value_type>;

  using ChunksType = internal::Chunks<Deque>;
  using ConstChunksType = internal::ConstChunks<Deque>;

  /// An instantiation of a `MultiBuf`.
  ///
  /// `BasicMultiBuf` represents the interface of a particular MultiBuf type.
  /// It stores no state, and cannot be instantiated directly. Instead, this
  /// type can be used to create variables and members of a particular MultiBuf
  /// type.
  ///
  /// These can then be "deferenced" to be passed to routines that take a
  /// parameter of the same MultiBuf type, or converted to a different type
  /// using `as`, e.g.
  ///
  /// @code{.cpp}
  /// extern void AdjustLayers(LayerableMultiBuf&);
  /// extern void DoTheThing(MyMultiBuf&);
  ///
  /// MyMultiBuf::Instance mb = InitMyMultiBufInstance();
  /// AdjustLayers(mb->as<LayerableMultiBuf>());
  /// DoTheThing(*mb);
  /// @endcode
  using Instance = internal::Instance<BasicMultiBuf>;

  // Interfaces are not copyable or movable; copy and move `Instance`s instead.

  BasicMultiBuf(const BasicMultiBuf&) { InvalidCopyOrMove<>(); }

  template <Property... kOtherProperties>
  BasicMultiBuf(const BasicMultiBuf<kOtherProperties...>&) {
    InvalidCopyOrMove<>();
  }

  BasicMultiBuf& operator=(const BasicMultiBuf&) { InvalidCopyOrMove<>(); }

  template <Property... kOtherProperties>
  BasicMultiBuf& operator=(const BasicMultiBuf<kOtherProperties...>&) {
    InvalidCopyOrMove<>();
  }

  BasicMultiBuf(BasicMultiBuf&&) { InvalidCopyOrMove<>(); }

  BasicMultiBuf& operator=(BasicMultiBuf&&) { InvalidCopyOrMove<>(); }

  // Conversions

  template <typename OtherMultiBuf>
  constexpr OtherMultiBuf& as() & {
    internal::AssertIsConvertible<BasicMultiBuf, OtherMultiBuf>();
    return generic().template as<OtherMultiBuf>();
  }

  template <typename OtherMultiBuf>
  constexpr const OtherMultiBuf& as() const& {
    internal::AssertIsConvertible<BasicMultiBuf, OtherMultiBuf>();
    return generic().template as<OtherMultiBuf>();
  }

  template <typename OtherMultiBuf>
  constexpr OtherMultiBuf&& as() && {
    internal::AssertIsConvertible<BasicMultiBuf, OtherMultiBuf>();
    return std::move(generic().template as<OtherMultiBuf>());
  }

  template <typename OtherMultiBuf>
  constexpr const OtherMultiBuf&& as() const&& {
    internal::AssertIsConvertible<BasicMultiBuf, OtherMultiBuf>();
    return std::move(generic().template as<OtherMultiBuf>());
  }

  template <
      typename OtherMultiBuf,
      typename = internal::EnableIfConvertible<BasicMultiBuf, OtherMultiBuf>>
  constexpr operator OtherMultiBuf&() & {
    return as<OtherMultiBuf>();
  }

  template <
      typename OtherMultiBuf,
      typename = internal::EnableIfConvertible<BasicMultiBuf, OtherMultiBuf>>
  constexpr operator const OtherMultiBuf&() const& {
    return as<OtherMultiBuf>();
  }

  template <
      typename OtherMultiBuf,
      typename = internal::EnableIfConvertible<BasicMultiBuf, OtherMultiBuf>>
  constexpr operator OtherMultiBuf&&() && {
    return std::move(as<OtherMultiBuf>());
  }

  template <
      typename OtherMultiBuf,
      typename = internal::EnableIfConvertible<BasicMultiBuf, OtherMultiBuf>>
  constexpr operator const OtherMultiBuf&&() const&& {
    return std::move(as<OtherMultiBuf>());
  }

  // Accessors

  /// Returns whether the MultiBuf is empty, i.e. whether it has no chunks or
  /// fragments.
  constexpr bool empty() const { return generic().empty(); }

  /// Returns whether the MultiBuf is at capacity, i.e. whether it cannot add
  /// more chunks or fragments without allocating additional memory. This is
  /// similar to the standard `full()` method for containers, except that this
  /// object can dynamically grow.
  constexpr bool at_capacity() const { return generic().at_capacity(); }

  /// Returns the size of a MultiBuf in bytes, which is the sum of the lengths
  /// of the views that make up its topmost layer.
  constexpr size_t size() const { return generic().size(); }

  /// @name at
  /// Returns a reference to the byte at specified index.
  ///
  /// @warning Do not use addresses of returned references for ranges! The
  /// underlying memory is not guaranteed to be contiguous, so statements such
  /// as `std::memcpy(data.bytes(), &multibuf[0], data.size())` may corrupt
  /// memory.
  ///
  /// Use `CopyTo` or `Get` to read ranges of bytes.
  ///
  /// @{
  template <bool kMutable = !is_const()>
  std::enable_if_t<kMutable, reference> at(size_t index) {
    return *(begin() + static_cast<int>(index));
  }
  const_reference at(size_t index) const {
    return *(begin() + static_cast<int>(index));
  }

  template <bool kMutable = !is_const()>
  std::enable_if_t<kMutable, reference> operator[](size_t index) {
    return at(index);
  }
  const_reference operator[](size_t index) const { return at(index); }
  /// @}

  /// @name Chunks
  /// Returns a chunk-iterable view of the MultiBuf.
  ///
  /// This can be used in a range-based for-loop, e.g.
  ///
  /// @code{.cpp}
  /// for (ConstByteSpan chunk : multibuf.ConstChunks()) {
  ///   Read(chunk);
  /// }
  /// @endcode
  /// @{
  template <bool kMutable = !is_const()>
  constexpr std::enable_if_t<kMutable, ChunksType> Chunks() {
    return generic().Chunks();
  }
  constexpr ConstChunksType Chunks() const { return generic().ConstChunks(); }
  constexpr ConstChunksType ConstChunks() const {
    return generic().ConstChunks();
  }
  /// @}

  // Iterators.

  /// @name begin
  /// Returns an iterator to the start of the MultiBuf's bytes.
  ///
  /// @warning Iterator-based algorithms such as `std::copy` may perform worse
  /// than expected due to overhead of advancing iterators across multiple,
  /// potentially noncontiguous memory regions.
  ///
  /// Use `CopyTo` or `Get`, or `Visit` to read ranges of bytes.
  ///
  /// @{
  template <bool kMutable = !is_const()>
  constexpr std::enable_if_t<kMutable, iterator> begin() {
    return generic().begin();
  }
  constexpr const_iterator begin() const { return cbegin(); }
  constexpr const_iterator cbegin() const { return generic().cbegin(); }
  /// @}

  /// @name end
  /// Returns an iterator past the end of the MultiBuf's bytes.
  ///
  /// @warning Iterator-based algorithms such as `std::copy` may perform worse
  /// than expected due to overhead of advancing iterators across multiple,
  /// potentially noncontiguous memory regions.
  ///
  /// Use `CopyTo` or `Get` to read ranges of bytes.
  ///
  /// @{
  template <bool kMutable = !is_const()>
  constexpr std::enable_if_t<kMutable, iterator> end() {
    return generic().end();
  }
  constexpr const_iterator end() const { return cend(); }
  constexpr const_iterator cend() const { return generic().cend(); }
  /// @}

  // Other methods

  /// Attempts to reserves memory to hold metadata for the given number of total
  /// chunks. Returns whether the memory was successfully allocated.
  [[nodiscard]] bool TryReserveChunks(size_t num_chunks) {
    return generic().TryReserveChunks(num_chunks);
  }

  // Mutators

  /// Attempts to modify this object to be able to insert the given MultiBuf,
  /// and returns whether successful.
  ///
  /// It is an error to call this method with an invalid iterator or
  /// incompatible MultiBuf, if applicable.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves the
  /// object unchanged. Otherwise, returns true.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    mb      MultiBuf to be inserted.
  template <Property... kOtherProperties>
  [[nodiscard]] bool TryReserveForInsert(
      const_iterator pos, const BasicMultiBuf<kOtherProperties...>& mb);

  /// Attempts to modify this object to be able to insert a chunk at the given
  /// position, and returns whether successful.
  ///
  /// It is an error to call this method with an invalid iterator.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves the
  /// object unchanged. Otherwise, returns true.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  [[nodiscard]] bool TryReserveForInsert(const_iterator pos);

  /// Insert memory before the given iterator.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForInsert`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    mb      MultiBuf to be inserted.
  template <Property... kOtherProperties>
  void Insert(const_iterator pos, BasicMultiBuf<kOtherProperties...>&& mb);

  /// Insert memory before the given iterator.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForInsert`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    bytes   Unowned memory to be inserted.
  template <
      int&... kExplicitGuard,
      typename T,
      typename =
          std::enable_if_t<std::is_constructible_v<ConstByteSpan, T>, int>>
  void Insert(const_iterator pos, const T& bytes);

  /// @name Insert
  /// Insert memory before the given iterator.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForInsert`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    bytes   Owned memory to be inserted.
  /// @{
  void Insert(const_iterator pos, UniquePtr<std::byte[]>&& bytes);
  void Insert(const_iterator pos, UniquePtr<const std::byte[]>&& bytes);
  /// @}

  /// @name Insert
  /// Insert memory before the given iterator.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForInsert`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    bytes   Shared memory to be inserted.
  /// @{
  void Insert(const_iterator pos, const SharedPtr<std::byte[]>& bytes) {
    Insert(pos, bytes, 0);
  }
  void Insert(const_iterator pos, const SharedPtr<const std::byte[]>& bytes) {
    Insert(pos, bytes, 0);
  }
  /// @}

  /// @name Insert
  /// Insert memory before the given iterator.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForInsert`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    pos     Location to insert memory within the MultiBuf.
  /// @param    bytes   Shared memory to be inserted.
  /// @param    offset  Used to denote a subspan of `bytes`.
  /// @param    length  Used to denote a subspan of `bytes`.
  /// @{
  void Insert(const_iterator pos,
              const SharedPtr<std::byte[]>& bytes,
              size_t offset,
              size_t length = dynamic_extent);
  void Insert(const_iterator pos,
              const SharedPtr<const std::byte[]>& bytes,
              size_t offset,
              size_t length = dynamic_extent);
  /// @}

  /// Attempts to modify this object to be able to append the given MultiBuf to
  /// the end of this object.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves the
  /// object unchanged. Otherwise, returns true.
  ///
  /// @param    mb      MultiBuf to be inserted.
  template <Property... kOtherProperties>
  [[nodiscard]] bool TryReserveForPushBack(
      const BasicMultiBuf<kOtherProperties...>& mb);

  /// Attempts to modify this object to be able to append a chunk to the end of
  /// this object.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves the
  /// object unchanged. Otherwise, returns true.
  [[nodiscard]] bool TryReserveForPushBack();

  /// Moves bytes to the end of this object.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForPushBack`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    mb      MultiBuf to be inserted.
  template <Property... kOtherProperties>
  void PushBack(BasicMultiBuf<kOtherProperties...>&& mb);

  /// Moves memory to the end of this object.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForPushBack`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    bytes   Unowned memory to be inserted.
  template <
      int&... kExplicitGuard,
      typename T,
      typename =
          std::enable_if_t<std::is_constructible_v<ConstByteSpan, T>, int>>
  void PushBack(const T& bytes);

  /// @name PushBack
  /// Moves memory to the end of this object.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForPushBack`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    bytes   Owned memory to be inserted.
  /// @{
  void PushBack(UniquePtr<std::byte[]>&& bytes);
  void PushBack(UniquePtr<const std::byte[]>&& bytes);
  /// @}

  /// @name PushBack
  /// Moves memory to the end of this object.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForPushBack`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    bytes   Shared memory to be inserted.
  /// @{
  void PushBack(const SharedPtr<std::byte[]>& bytes) { PushBack(bytes, 0); }
  void PushBack(const SharedPtr<const std::byte[]>& bytes) {
    PushBack(bytes, 0);
  }
  /// @}

  /// @name PushBack
  /// Moves memory to the end of this object.
  ///
  /// It is a fatal error if this method cannot allocate space for necessary
  /// metadata. See also `TryReserveForPushBack`, which can be used to try to
  /// pre-allocate the needed space without crashing.
  ///
  /// @param    bytes   Shared memory to be inserted.
  /// @param    offset  Used to denote a subspan of `bytes`.
  /// @param    length  Used to denote a subspan of `bytes`.
  /// @{
  void PushBack(const SharedPtr<std::byte[]>& bytes,
                size_t offset,
                size_t length = dynamic_extent);
  void PushBack(const SharedPtr<const std::byte[]>& bytes,
                size_t offset,
                size_t length = dynamic_extent);
  /// @}

  /// Returns whether the given range can be removed.
  ///
  /// A range may not be valid to `Remove` if it does not fall within the
  /// MultiBuf.
  ///
  /// @param    pos     Location from which to remove memory from the MultiBuf.
  /// @param    size    Amount of memory to remove.
  [[nodiscard]] bool IsRemovable(const_iterator pos, size_t size) const {
    return generic().IsRemovable(pos, size);
  }

  /// Removes if a range of bytes from this object.
  ///
  /// The range given by `pos` and `size` MUST be removable, as described by
  /// `IsRemovable`: It MUST fall within this MultiBuf, and not split "owned"
  /// chunks.
  ///
  /// This method may fail if unable to allocate metadata for a new MultiBuf.
  ///
  /// On successful completion, this method will return a MultiBuf populated
  /// with entries corresponding to the removed memory range.
  ///
  /// On failure, the original MultiBuf is unmodified.
  ///
  /// @param    pos     Location from which to remove memory from the MultiBuf.
  /// @param    size    Amount of memory to remove.
  ///
  /// @returns @Result{the MultiBuf containing the removed chunks}
  /// * @RESOURCE_EXHAUSTED: Failed to allocate memory for the new %MultiBuf's
  ///   metadata.
  Result<Instance> Remove(const_iterator pos, size_t size);

  /// Removes the first fragment from this object and returns it.
  ///
  /// Without any layers, each chunk is a fragment. Layered MultiBufs may group
  /// multiple chunks into a single fragment.
  ///
  /// It is an error to call this method when the MultiBuf is empty.
  ///
  /// @returns @Result{the fragment in a new MultiBuf}
  /// * @RESOURCE_EXHAUSTED: Attempting to reserve space for the new MultiBuf
  ///   failed.
  Result<Instance> PopFrontFragment();

  // clang-format off
  /// Removes if a range of bytes from this object.
  ///
  /// The range given by `pos` and `size` MUST fall within this MultiBuf.
  ///
  /// This method may fail if space for additional metadata is needed but cannot
  /// be allocated.
  ///
  /// On successful completion, this method will return a valid iterator
  /// pointing to the memory following that which was discarded.
  ///
  /// On failure, the original MultiBuf is unmodified.
  ///
  /// "Owned" chunks, i.e. those added using a `UniquePtr`, which are fully
  /// discarded as a result of this call will be deallocated.
  ///
  /// @param    pos     Location from which to discard memory from the MultiBuf.
  /// @param    size    Amount of memory to discard.
  ///
  /// @returns @Result{an iterator pointing to the memory after the discarded range}
  /// * @RESOURCE_EXHAUSTED:  Failed to allocate memory for the new %MultiBuf's
  ///   metadata.
  // clang-format on
  Result<const_iterator> Discard(const_iterator pos, size_t size) {
    return generic().Discard(pos, size);
  }

  /// Returns whether the given iterator refers to a location within an "owned"
  /// chunk, that is, memory that was added as a `UniquePtr`.
  ///
  /// @param    pos     Location within the MultiBuf of the memory to release.
  [[nodiscard]] bool IsReleasable(const_iterator pos) const {
    return generic().IsReleasable(pos);
  }

  /// Removes a memory allocation from this object and releases ownership of it.
  ///
  /// The location given by `pos` and MUST be releasable, as described by
  /// `IsReleasable`: It MUST fall within an "owned" chunk.
  ///
  /// This method returns a `UniquePtr` which owns the removed memory.
  ///
  /// The entire owned chunk containing the location indicated by the iterator
  /// will be removed and returned. An iterator to the middle of an owned chunk
  /// will result in some bytes before the iterator being removed.
  ///
  /// @param    pos     Location within the MultiBuf of the memory to release.
  UniquePtr<value_type[]> Release(const_iterator pos);

  /// Returns whether the given iterator refers to a location within a "shared"
  /// chunk, that is, memory that was added as a `SharedPtr`.
  ///
  /// @param    pos     Location within the MultiBuf of the memory to share.
  [[nodiscard]] bool IsShareable(const_iterator pos) const {
    return generic().IsShareable(pos);
  }

  /// Returns the shared memory at the given location.
  ///
  /// The location given by `pos` and MUST be shareable, as described by
  /// `IsShareable`: It MUST fall within a "shared" chunk.
  ///
  /// This method returns a `SharedPtr` which shares ownership of the indicated
  /// memory. the memory will not be freed until all shared pointers to it go
  /// out of scope.
  ///
  /// The returned pointer will reference the entire shared chunk containing the
  /// location indicated by the iterator. The shared pointer for an iterator to
  /// the middle of a shared chunk will include some bytes before the iterator.
  ///
  /// @param    pos     Location within the MultiBuf of the memory to share.
  SharedPtr<value_type[]> Share(const_iterator pos);

  /// Writes data from the MultiBuf at the given `offset` to `dst`.
  ///
  /// @param    dst     Span to copy data to. Its length determines the
  ///                   maximum number of bytes that may be copied.
  /// @param    offset  Offset from the start of the MultiBuf to start copying
  ///                   from.
  ///
  /// @returns          The number of bytes copied.
  size_t CopyTo(ByteSpan dst, size_t offset = 0) const {
    return generic().CopyTo(dst, offset);
  }

  /// Writes data from `src` to the MultiBuf at the given `offset`.
  ///
  /// @param    src     Span to copy data from. Its length determines the
  ///                   maximum number of bytes that may be copied.
  /// @param    offset  Offset from the start of the MultiBuf to start copying
  ///                   to.
  ///
  /// @returns          The number of bytes copied.
  size_t CopyFrom(ConstByteSpan src, size_t offset = 0) {
    static_assert(!is_const(),
                  "`CopyFrom` may only be called on mutable MultiBufs");
    return generic().CopyFrom(src, offset);
  }

  /// Returns a byte span containing data at the given `offset`.
  ///
  /// If the data is contiguous, a view to it is returned directly. Otherwise,
  /// it is copied from the non-contiguous buffers into the provided span, which
  /// is then returned.
  ///
  /// As a result, this method should only be used on small regions of data,
  /// e.g. packet headers.
  ///
  /// @param    copy    A buffer that may be used to hold data if the requested
  ///                   region is non-contiguous. Its length determines the
  ///                   maximum number of bytes that may be copied.
  /// @param    offset  Offset from the start of the MultiBuf to start copying
  ///                   from.
  ///
  /// @returns          A span of bytes for the requested range.
  ConstByteSpan Get(ByteSpan copy, size_t offset = 0) const {
    return generic().Get(copy, offset);
  }

  /// Passes a byte span containing data at the given `offset` to a `visitor`.
  ///
  /// If the data is contiguous, the `visitor` is called on it directly.
  /// Otherwise, data is copied from the non-contiguous buffers into the
  /// provided span, which is then passed to `visitor`.
  ///
  /// @param    visitor A callable object, such as a function pointer or lambda,
  ///                   that can be called on a `ConstByteSpan`.
  /// @param    copy    A buffer that may be used to hold data if the requested
  ///                   region is non-contiguous. Its length determines the
  ///                   maximum number of bytes that may be copied.
  /// @param    offset  Offset from the start of the MultiBuf to start copying
  ///                   from.
  ///
  /// @returns          The result of calling `visitor` on the requested range.
  template <int&... kExplicitGuard, typename Visitor>
  auto Visit(Visitor visitor, ByteSpan copy, size_t offset) {
    return visitor(Get(copy, offset));
  }

  /// Releases all memory regions from this object.
  ///
  /// If this object has a deallocator, it will be used to free the memory
  /// owned by this object. When this method returns, the MultiBuf
  /// will be restored to an initial, empty state.
  void Clear() { generic().Clear(); }

  /// Removes unused capacity from the internal deque.
  ///
  /// Owned memory r
  /// This object's memory context  has a deallocator, it will be used to free
  /// the memory owned by this object. When this method returns, the MultiBuf
  /// will be restored to an initial, empty state.
  void ShrinkToFit() { generic().ShrinkToFit(); }

  // Observable methods.

  /// Returns the observer for this MultiBuf, or null if none has been set.
  ///
  /// Observers are notified whenever fragments or layers are added or removed
  /// from the MultiBuf.
  constexpr Observer* observer() const {
    static_assert(is_observable(),
                  "`observer` may only be called on observable MultiBufs");
    return generic().observer_;
  }

  /// Sets the observer for this MultiBuf.
  ///
  /// Passing null effectively clears the observer from the MultiBuf.
  ///
  /// Observers are notified whenever fragments or layers are added or removed
  /// from the MultiBuf.
  void set_observer(Observer* observer) {
    static_assert(is_observable(),
                  "`set_observer` may only be called on observable MultiBufs");
    generic().observer_ = observer;
  }

  // Layerable methods.

  /// Returns the number of fragments in the top layer.
  ///
  /// Whenever a new layer is added, its boundary is marked and it is treated as
  /// a single fragment of a larger message or packet. These boundaries markers
  /// are preserved by `Insert` and `PushBack`. They can be used to delineate
  /// how much memory to return when `PopFront` is called.
  size_type NumFragments() const {
    static_assert(is_layerable(),
                  "`NumFragments` may only be called on layerable MultiBufs");
    return generic().NumFragments();
  }

  /// Returns the number layers in the MultiBuf.
  ///
  /// This will always be at least 1.
  constexpr size_type NumLayers() const {
    static_assert(is_layerable(),
                  "`NumLayers` may only be called on layerable MultiBufs");
    return generic().NumLayers();
  }

  /// Attempts to reserves memory to hold metadata for the given number of
  /// layers and chunks.
  ///
  /// For example, assume you know your MultiBuf needs to hold layers for
  /// Ethernet, and IP. If separate chunks for the headers, the payload,
  /// and the Ethernet footer, then you can preallocate the structure by calling
  /// `TryReserveLayers(2, 3).
  [[nodiscard]] bool TryReserveLayers(size_t num_layers,
                                      size_t num_chunks = 1) {
    return generic().TryReserveLayers(num_layers, num_chunks);
  }

  /// Adds a layer.
  ///
  /// Each layer provides a span-like view of memory. An empty MultiBuf has no
  /// layers, while a non-empty MultiBuf has at least one. Additional layers
  /// provide a subspan-like view of the layer beneath it. This is useful in
  /// representing nested protocols, where the payloads of one level make up the
  /// messages of the next level.
  ///
  /// This will modify the apparent byte sequence to be a view of the previous
  /// top layer.
  ///
  /// The range given by `offset` and `length` MUST fall within this MultiBuf.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves the
  /// object unchanged. Otherwise, returns true.
  ///
  /// @param[in]  offset  Offset from the start of the previous top layer for
  ///                     the new top layer.
  /// @param[in]  length  Length of the new top layer.
  [[nodiscard]] bool AddLayer(size_t offset, size_t length = dynamic_extent) {
    static_assert(is_layerable(),
                  "`AddLayer` may only be called on layerable MultiBufs");
    return generic().AddLayer(offset, length);
  }

  /// Marks the top layer as "sealed", preventing it from being resized or
  /// popped.
  void SealTopLayer() {
    static_assert(is_layerable(),
                  "`SealTopLayer` may only be called on layerable MultiBufs");
    return generic().SealTopLayer();
  }

  /// Clears the "sealed" flag from the top layer, allowing it to be resized or
  /// popped.
  void UnsealTopLayer() {
    static_assert(is_layerable(),
                  "`UnsealTopLayer` may only be called on layerable MultiBufs");
    return generic().UnsealTopLayer();
  }

  /// Returns whether the "sealed" flag is set in the top layer.
  [[nodiscard]] bool IsTopLayerSealed() {
    static_assert(
        is_layerable(),
        "`IsTopLayerSealed` may only be called on layerable MultiBufs");
    return generic().IsTopLayerSealed();
  }

  /// Shortens the length of the current top layer.
  ///
  /// `length` MUST be less than or equal to the MultiBuf's current size.
  /// It is an error to call this method when `NumLayers()` < 2.
  ///
  /// Crashes if the top layer is sealed.
  ///
  /// @param[in]  length  New length of the top layer.
  void TruncateTopLayer(size_t length) {
    static_assert(
        is_layerable(),
        "`TruncateTopLayer` may only be called on layerable MultiBufs");
    generic().TruncateTopLayer(length);
  }

  /// Writes data from `src` to the MultiBuf and sizes the top layer to match.
  ///
  /// `src` must fit within the current top layer.
  ///
  /// @param[in]  src   Span to copy data from. Its length determines the
  ///                   size of the new top layer.
  void SetTopLayer(ConstByteSpan src) {
    static_assert(!is_const() && is_layerable(),
                  "`SetTopLayer` may only be called on mutable, layerable "
                  "MultiBufs");
    PW_ASSERT(src.size() <= size());
    CopyFrom(src);
    TruncateTopLayer(src.size());
  }

  /// Removes the top layer.
  ///
  /// After this call, the layer beneath the top layer will be the new top
  /// layer.
  ///
  /// It is an error to call this method when `NumLayers()` < 2.
  ///
  /// Crashes if the top layer is sealed.
  void PopLayer() {
    static_assert(is_layerable(),
                  "`PopLayer` may only be called on layerable MultiBufs");
    generic().PopLayer();
  }

 protected:
  constexpr BasicMultiBuf() { internal::PropertiesAreValid(); }

 private:
  template <bool kValidCopyOrMove = false>
  static constexpr void InvalidCopyOrMove() {
    static_assert(kValidCopyOrMove,
                  "Only copies and moves from `BasicMultiBuf<...>::Instance`"
                  "to `BasicMultiBuf<...>&` or another "
                  "`BasicMultiBuf<...>::Instance` are valid.");
  }

  template <Property...>
  friend class BasicMultiBuf;

  constexpr GenericMultiBuf& generic() {
    return static_cast<GenericMultiBuf&>(*this);
  }
  constexpr const GenericMultiBuf& generic() const {
    return static_cast<const GenericMultiBuf&>(*this);
  }
};

/// @}

/// @}

namespace internal {

// A generic MultiBuf implementation that provides the functionality of any
// `BasicMultiBuf<kProperties>` type.
//
// This class should not be instantiated directly. Instead, use `Instance` as
// described below. This is the base class for all `Instance` types, and derives
// from every supported `BasicMultiBuf` type. It implements the MultiBuf
// behavior in one type, and allows for performing conversions from `Instance`s
// and `BasicMultiBuf`s to `BasicMultiBuf`s with different yet compatible
// propreties.
class GenericMultiBuf final
    : private BasicMultiBuf<>,
      private BasicMultiBuf<Property::kObservable>,
      private BasicMultiBuf<Property::kLayerable>,
      private BasicMultiBuf<Property::kConst>,
      private BasicMultiBuf<Property::kConst, Property::kObservable>,
      private BasicMultiBuf<Property::kConst, Property::kLayerable>,
      private BasicMultiBuf<Property::kLayerable, Property::kObservable>,
      private BasicMultiBuf<Property::kConst,
                            Property::kLayerable,
                            Property::kObservable> {
 private:
  using ControlBlock = allocator::internal::ControlBlock;
  using typename BasicMultiBuf<>::ChunksType;
  using typename BasicMultiBuf<>::ConstChunksType;
  using typename BasicMultiBuf<>::Deque;

 public:
  using typename BasicMultiBuf<>::const_iterator;
  using typename BasicMultiBuf<>::const_pointer;
  using typename BasicMultiBuf<>::const_reference;
  using typename BasicMultiBuf<>::difference_type;
  using typename BasicMultiBuf<>::iterator;
  using typename BasicMultiBuf<>::pointer;
  using typename BasicMultiBuf<>::reference;
  using typename BasicMultiBuf<>::size_type;

  ~GenericMultiBuf() { Clear(); }

  // Not copyable.
  GenericMultiBuf(const GenericMultiBuf&) = delete;
  GenericMultiBuf& operator=(const GenericMultiBuf&) = delete;

  // Movable.
  GenericMultiBuf(GenericMultiBuf&& other)
      : GenericMultiBuf(other.deque_.get_allocator()) {
    *this = std::move(other);
  }
  GenericMultiBuf& operator=(GenericMultiBuf&& other);

 private:
  template <::pw::multibuf::v2::Property...>
  friend class ::pw::multibuf::v2::BasicMultiBuf;

  template <typename>
  friend class Instance;

  /// Constructs an empty MultiBuf.
  constexpr explicit GenericMultiBuf(Allocator& allocator)
      : deque_(allocator) {}

  template <typename MultiBufType>
  constexpr MultiBufType& as() {
    return *this;
  }

  template <typename MultiBufType>
  constexpr const MultiBufType& as() const {
    return *this;
  }

  // Accessors.

  /// @copydoc BasicMultiBuf<>::empty
  constexpr bool empty() const { return deque_.empty(); }

  /// @copydoc BasicMultiBuf<>::at_capacity
  constexpr bool at_capacity() const {
    return deque_.size() == deque_.capacity();
  }

  /// @copydoc BasicMultiBuf<>::size
  constexpr size_t size() const {
    return static_cast<size_t>(cend() - cbegin());
  }

  // Iterators.

  constexpr ChunksType Chunks() {
    return ChunksType(deque_, entries_per_chunk_);
  }
  constexpr ConstChunksType ConstChunks() const {
    return ConstChunksType(deque_, entries_per_chunk_);
  }

  constexpr iterator begin() { return iterator(Chunks().begin(), 0); }
  constexpr const_iterator cbegin() const {
    return const_iterator(ConstChunks().cbegin(), 0);
  }

  constexpr iterator end() { return iterator(Chunks().end(), 0); }
  constexpr const_iterator cend() const {
    return const_iterator(ConstChunks().cend(), 0);
  }

  // Mutators.

  /// @copydoc BasicMultiBuf<>::TryReserveChunks
  [[nodiscard]] bool TryReserveChunks(size_t num_chunks) {
    return TryReserveLayers(NumLayers(), num_chunks);
  }

  /// @copydoc BasicMultiBuf<>::TryReserveForInsert
  /// @{
  [[nodiscard]] bool TryReserveForInsert(const_iterator pos,
                                         const GenericMultiBuf& mb);
  [[nodiscard]] bool TryReserveForInsert(const_iterator pos);
  /// @}

  /// @copydoc BasicMultiBuf<>::Insert
  /// @{
  void Insert(const_iterator pos, GenericMultiBuf&& mb);
  void Insert(const_iterator pos, ConstByteSpan bytes);
  void Insert(const_iterator pos,
              ConstByteSpan bytes,
              size_t offset,
              size_t length,
              Deallocator* deallocator);
  void Insert(const_iterator pos,
              ConstByteSpan bytes,
              size_t offset,
              size_t length,
              ControlBlock* control_block);
  /// @}

  /// @copydoc BasicMultiBuf<>::IsRemovable
  [[nodiscard]] constexpr bool IsRemovable(const_iterator pos,
                                           size_t size) const {
    return pos != cend() && size != 0 &&
           size <= static_cast<size_t>(cend() - pos);
  }

  /// @copydoc BasicMultiBuf<>::Remove
  Result<GenericMultiBuf> Remove(const_iterator pos, size_t size);

  /// @copydoc BasicMultiBuf<>::PopFrontFragment
  Result<GenericMultiBuf> PopFrontFragment();

  /// @copydoc BasicMultiBuf<>::Discard
  Result<const_iterator> Discard(const_iterator pos, size_t size);

  /// @copydoc BasicMultiBuf<>::IsReleasable
  [[nodiscard]] bool IsReleasable(const_iterator pos) const;

  /// @copydoc BasicMultiBuf<>::Release
  UniquePtr<std::byte[]> Release(const_iterator pos);

  /// @copydoc BasicMultiBuf<>::IsShareable
  [[nodiscard]] bool IsShareable(const_iterator pos) const;

  /// @copydoc BasicMultiBuf<>::Share
  SharedPtr<std::byte[]> Share(const_iterator pos);

  /// @copydoc BasicMultiBuf<>::CopyTo
  size_t CopyTo(ByteSpan dst, size_t offset) const {
    return CopyToImpl(dst, offset, 0);
  }

  /// @copydoc BasicMultiBuf<>::CopyFrom
  size_t CopyFrom(ConstByteSpan src, size_t offset);

  /// @copydoc BasicMultiBuf<>::Get
  ConstByteSpan Get(ByteSpan copy, size_t offset) const;

  /// @copydoc BasicMultiBuf<>::Clear
  void Clear();

  /// @copydoc BasicMultiBuf<>::ShrinkToFit
  void ShrinkToFit();

  // Layerable methods.

  /// @copydoc BasicMultiBuf<>::NumFragments
  size_type NumFragments() const;

  /// @copydoc BasicMultiBuf<>::NumLayers
  constexpr size_type NumLayers() const {
    return entries_per_chunk_ - Entry::kMinEntriesPerChunk + 1;
  }

  /// @copydoc BasicMultiBuf<>::TryReserveLayers
  [[nodiscard]] bool TryReserveLayers(size_t num_layers, size_t num_chunks = 1);

  /// @copydoc BasicMultiBuf<>::AddLayer
  [[nodiscard]] bool AddLayer(size_t offset, size_t length = dynamic_extent);

  /// @copydoc BasicMultiBuf<>::SealTopLayer
  void SealTopLayer();

  /// @copydoc BasicMultiBuf<>::UnsealTopLayer
  void UnsealTopLayer();

  /// @copydoc BasicMultiBuf<>::TruncateTopLayer
  void TruncateTopLayer(size_t length);

  /// @copydoc BasicMultiBuf<>::PopLayer
  void PopLayer();

  // Implementation methods.
  //
  // These methods are used to implement the methods above, and should not be
  // called directly by BasicMultiBuf<>.

  /// Asserts that [`offset`, `offset + range`) falls within [0, `size`)].
  ///
  /// Returns `size` - `offset` if `length` is `dynamic_extent`; otherwise
  /// returns `length`.
  static size_t CheckRange(size_t offset, size_t length, size_t size);

  /// Returns the number of chunks in the MultiBuf.
  constexpr size_type num_chunks() const {
    return deque_.size() / entries_per_chunk_;
  }

  /// Returns the index to the data entry of a given chunk.
  constexpr size_type memory_context_index(size_type chunk) const {
    return Entry::memory_context_index(chunk, entries_per_chunk_);
  }

  /// Returns the index to the data entry of a given chunk.
  constexpr size_type data_index(size_type chunk) const {
    return Entry::data_index(chunk, entries_per_chunk_);
  }

  /// Returns the index to the base view entry of a given chunk.
  constexpr size_type base_view_index(size_type chunk) const {
    return Entry::base_view_index(chunk, entries_per_chunk_);
  }

  /// Returns the index to a view entry of a given chunk.
  constexpr size_type view_index(size_type chunk, size_type layer) const {
    return Entry::view_index(chunk, entries_per_chunk_, layer);
  }

  /// Returns the index to the top view entry of a given chunk.
  constexpr size_type top_view_index(size_type chunk) const {
    return Entry::top_view_index(chunk, entries_per_chunk_);
  }

  /// Returns the memory backing the chunk at the given index.
  constexpr std::byte* GetData(size_type chunk) const {
    return deque_[data_index(chunk)].data;
  }

  /// Returns whether the memory backing the chunk at the given index is owned
  /// by this object.
  constexpr bool IsOwned(size_type chunk) const {
    return deque_[base_view_index(chunk)].base_view.owned;
  }

  /// Returns whether the memory backing the chunk at the given index is shared
  /// with other objects.
  constexpr bool IsShared(size_type chunk) const {
    return deque_[base_view_index(chunk)].base_view.shared;
  }

  /// Returns whether the chunk is part of a sealed layer.
  constexpr bool IsSealed(size_type chunk) const {
    return entries_per_chunk_ == Entry::kMinEntriesPerChunk
               ? false
               : deque_[top_view_index(chunk)].view.sealed;
  }

  /// Returns whether the chunk represents a fragment boundary.
  constexpr bool IsBoundary(size_type chunk) const {
    return entries_per_chunk_ == Entry::kMinEntriesPerChunk
               ? true
               : deque_[top_view_index(chunk)].view.boundary;
  }

  /// Returns the absolute offset of the given layer of the chunk.
  ///
  /// `layer` must be in the range [1, NumLayers()].
  constexpr size_type GetOffset(size_type chunk, uint16_t layer) const {
    return layer == 1 ? deque_[base_view_index(chunk)].base_view.offset
                      : deque_[view_index(chunk, layer)].view.offset;
  }

  /// Returns the offset of the view of the chunk.
  constexpr size_type GetOffset(size_type chunk) const {
    return GetOffset(chunk, NumLayers());
  }

  /// Returns the offset relative to the layer below of the view of the chunk.
  constexpr size_type GetRelativeOffset(size_type chunk) const {
    uint16_t layer = NumLayers();
    if (layer == 1) {
      return GetOffset(chunk, layer);
    }
    return GetOffset(chunk, layer) - GetOffset(chunk, layer - 1);
  }

  /// Returns the length of the view of the chunk at the given index.
  constexpr size_type GetLength(size_type chunk) const {
    return entries_per_chunk_ == Entry::kMinEntriesPerChunk
               ? deque_[base_view_index(chunk)].base_view.length
               : deque_[top_view_index(chunk)].view.length;
  }

  /// Returns the available view of a chunk.
  constexpr ByteSpan GetView(size_type chunk) const {
    return ByteSpan(GetData(chunk) + GetOffset(chunk), GetLength(chunk));
  }

  /// Converts an iterator into a chunk index and byte offset.
  ///
  /// The given iterator must be valid.
  std::pair<size_type, size_type> GetChunkAndOffset(const_iterator pos) const;

  /// Attempts to allocate a control block and convert an owned chunk into a
  /// shared one.
  [[nodiscard]] bool TryConvertToShared(size_type chunk);

  /// Attempts to allocate room for an additional `num_entries` of metadata.
  ///
  /// If the added entries would split a chunk, an extra chunk will be needed to
  /// hold the split portion.
  [[nodiscard]] bool TryReserveEntries(size_type num_entries,
                                       bool split = false);

  /// Inserts the given number of empty chunk into the deque at the given
  /// position, and returns the chunk index to the start of the chunk.
  size_type InsertChunks(const_iterator pos, size_type num_chunks);

  /// Inserts entries representing the given data into the deque at the given
  /// position, and returns the deque index to the start of the entries.
  size_type Insert(const_iterator pos,
                   ConstByteSpan bytes,
                   size_t offset,
                   size_t length);

  /// Sets the base entries of a given `out_chunk` in the `out_deque` to match
  /// the given `chunk`.
  ///
  /// This method should not be called directly. Call `SplitBefore` or
  /// `SplitAfter` instead.
  ///
  /// "Owned" chunks, i.e. those added using a `UniquePtr`, can be split between
  /// different chunks of a single MultiBuf, but it is an error to try to split
  /// them between different MultiBufs.
  void SplitBase(size_type chunk, Deque& out_deque, size_type out_chunk);

  /// Sets the given `out_chunk` in the `out_deque` to match the portion of the
  /// given `chunk` that comes before the given `split`.
  ///
  /// See the note on `SplitBase` about "owned" chunks. It is an error to try to
  /// split owned chunks between different MultiBufs.
  void SplitBefore(size_type chunk,
                   size_type split,
                   Deque& out_deque,
                   size_type out_chunk);

  /// Like SplitBefore, but with `out_deque` and `out_chunk` defaulting to
  /// `deque_` and `chunk`, respectively.
  void SplitBefore(size_type chunk, size_type split);

  /// Sets the given `out_chunk` in the `out_deque` to match the portion of the
  /// given `chunk` that comes after the given `split`.
  ///
  /// See the note on `SplitBase` about "owned" chunks. It is an error to try to
  /// split owned chunks between different MultiBufs.
  void SplitAfter(size_type chunk,
                  size_type split,
                  Deque& out_deque,
                  size_type out_chunk);

  /// Like SplitAfter, but with `out_deque` and `out_chunk` defaulting to
  /// `deque_` and `chunk`, respectively.
  void SplitAfter(size_type chunk, size_type split);

  /// Attempts to reserve space need to remove a range of bytes.
  ///
  /// If the range falls within a chunk, it may result in additional entries, as
  /// a single chunk is replaced by a prefix and a suffix.
  ///
  /// If `out` is provided, it will also try to reserve space in that object to
  /// received the removed entries.
  ///
  /// If unable to allocate space for the metadata, returns false and leaves
  /// this object unchanged. Otherwise, returns true.
  [[nodiscard]] bool TryReserveForRemove(const_iterator pos,
                                         size_t size,
                                         GenericMultiBuf* out);

  /// Moves entries for a given range of bytes from this object to another.
  ///
  /// It is an error to call this method without calling `TryReserveForRemove`
  /// first.
  void MoveRange(const_iterator pos, size_t size, GenericMultiBuf& out);

  /// Clears any chunks that fall completely within the given range.
  ///
  /// Owned chunks within the range will be deallocated.
  void ClearRange(const_iterator pos, size_t size);

  /// Removes the entries corresponding to the given range from this object.
  ///
  /// It is an error to call this method without calling `TryReserveForRemove`
  /// first.
  void EraseRange(const_iterator pos, size_t size);

  /// Copies `dst.size()` bytes from `offset` to `dst`, using the chunk hint,
  /// `start`.
  size_t CopyToImpl(ByteSpan dst, size_t offset, size_type start) const;

  /// Returns whether the top layer is sealed.
  [[nodiscard]] bool IsTopLayerSealed() const;

  /// Modifies the top layer to represent the range [`offset`, `offset + range`)
  /// of the second-from-top layer.
  void SetLayer(size_t offset, size_t length);

  // Describes the memory and views to that in a one-dimensional sequence.
  // Every `entries_per_chunk_`-th entry holds a pointer to memory, each entry
  // for a given offset less than `entries_per_chunk_` after that entry is a
  // view that is part of the same "layer" in the MultiBuf.
  //
  // This base type will always have 0 or 2 layers, but derived classes may add
  // more.
  //
  // For example, a MultiBuf that has had 3 `Chunk`s and two additional layers
  // added would have a `deque_` resembling the following:
  //
  //          buffer 0:         buffer 1:         buffer 2:
  // layer 3: deque_[0x3].view  deque_[0x7].view  deque_[0xB].view
  // layer 2: deque_[0x2].view  deque_[0x6].view  deque_[0xA].view
  // layer 1: deque_[0x1].view  deque_[0x5].view  deque_[0x9].view
  // layer 0: deque_[0x0].data  deque_[0x4].data  deque_[0x8].data
  Deque deque_;

  // Number of entries per chunk in this MultiBuf.
  size_type entries_per_chunk_ = Entry::kMinEntriesPerChunk;

  /// Optional subscriber to notifications about adding and removing bytes and
  /// layers.
  Observer* observer_ = nullptr;
};

/// An instantiation of a MultiBuf.
///
/// ``BasicMultiBuf`` represents the interface of a particular MultiBuf type.
/// It stores no state, and cannot be instantiated directly. Instead, this type
/// can be used to create variables and members of a particular MultiBuf type.
///
/// These can then be "deferenced" to be passed to routines that take a
/// parameter of the same MultiBuf type, or converted to a different type using
/// ``as``, e.g.
///
/// @code{.cpp}
/// extern void AdjustLayers(LayerableMultiBuf&);
/// extern void DoTheThing(MyMultiBuf&);
///
/// MyMultiBufInstance mb = InitMyMultiBufInstance();
/// AdjustLayers(mb->as<LayerableMultiBuf>());
/// DoTheThing(*mb);
/// @endcode
template <typename MultiBufType>
class Instance {
 public:
  constexpr explicit Instance(Allocator& allocator) : base_(allocator) {}

  constexpr Instance(Instance&&) = default;
  constexpr Instance& operator=(Instance&&) = default;

  // Provide a more helpful compile-time error when a user tries to
  // copy-construct an interface type.
  template <Property... kProperties>
  constexpr Instance(const BasicMultiBuf<kProperties...>&)
      : base_(allocator::GetNullAllocator()) {
    MoveOnly<>();
  }

  // Provide a more helpful compile-time error when a user tries to copy-assign
  // an interface type.
  template <Property... kProperties>
  constexpr Instance& operator=(const BasicMultiBuf<kProperties...>&) {
    MoveOnly<>();
  }

  constexpr Instance(MultiBufType&& mb)
      : base_(std::move(static_cast<GenericMultiBuf&>(mb))) {}

  constexpr Instance& operator=(MultiBufType&& mb) {
    base_ = std::move(static_cast<GenericMultiBuf&>(mb));
    return *this;
  }

  template <Property... kProperties>
  constexpr Instance(BasicMultiBuf<kProperties...>&& mb)
      : base_(std::move(static_cast<internal::GenericMultiBuf&>(mb))) {
    internal::AssertIsConvertible<BasicMultiBuf<kProperties...>,
                                  MultiBufType>();
  }

  template <Property... kProperties>
  constexpr Instance& operator=(BasicMultiBuf<kProperties...>&& mb) {
    internal::AssertIsConvertible<BasicMultiBuf<kProperties...>,
                                  MultiBufType>();
    base_ = std::move(static_cast<internal::GenericMultiBuf&>(mb));
    return *this;
  }

  constexpr MultiBufType* operator->() { return &base_.as<MultiBufType>(); }
  constexpr const MultiBufType* operator->() const {
    return &base_.as<MultiBufType>();
  }

  constexpr MultiBufType& operator*() & { return base_.as<MultiBufType>(); }
  constexpr const MultiBufType& operator*() const& {
    return base_.as<MultiBufType>();
  }

  constexpr MultiBufType&& operator*() && {
    return std::move(base_.as<MultiBufType>());
  }
  constexpr const MultiBufType&& operator*() const&& {
    return std::move(base_.as<MultiBufType>());
  }

  constexpr operator MultiBufType&() & { return base_.as<MultiBufType>(); }
  constexpr operator const MultiBufType&() const& {
    return base_.as<MultiBufType>();
  }

  constexpr operator MultiBufType&&() && {
    return std::move(base_.as<MultiBufType>());
  }
  constexpr operator const MultiBufType&&() const&& {
    return std::move(base_.as<MultiBufType>());
  }

 private:
  // Helper functions to provide a more helpful compile-time error when a this
  // type is used incorrectly.
  template <bool kMoveOnly = false>
  static constexpr void MoveOnly() {
    static_assert(kMoveOnly,
                  "Instances can only be created from existing MultiBufs using "
                  "move-construction or move-assignment.");
  }

  GenericMultiBuf base_;
};

}  // namespace internal

// Template method implementations.

template <Property... kProperties>
bool BasicMultiBuf<kProperties...>::TryReserveForInsert(const_iterator pos) {
  return generic().TryReserveForInsert(pos);
}

template <Property... kProperties>
template <Property... kOtherProperties>
bool BasicMultiBuf<kProperties...>::TryReserveForInsert(
    const_iterator pos, const BasicMultiBuf<kOtherProperties...>& mb) {
  internal::AssertIsConvertible<BasicMultiBuf<kOtherProperties...>,
                                BasicMultiBuf>();
  return generic().TryReserveForInsert(pos,
                                       static_cast<const GenericMultiBuf&>(mb));
}

template <Property... kProperties>
template <Property... kOtherProperties>
void BasicMultiBuf<kProperties...>::Insert(
    const_iterator pos, BasicMultiBuf<kOtherProperties...>&& mb) {
  internal::AssertIsConvertible<BasicMultiBuf<kOtherProperties...>,
                                BasicMultiBuf>();
  generic().Insert(pos, std::move(mb.generic()));
}

template <Property... kProperties>
template <int&... kExplicitGuard, typename T, typename>
void BasicMultiBuf<kProperties...>::Insert(const_iterator pos, const T& bytes) {
  using data_ptr_type = decltype(std::data(std::declval<T&>()));
  static_assert(std::is_same_v<data_ptr_type, std::byte*> || is_const(),
                "Cannot `Insert` read-only bytes into mutable MultiBuf");
  generic().Insert(pos, bytes);
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::Insert(const_iterator pos,
                                           UniquePtr<std::byte[]>&& bytes) {
  ConstByteSpan chunk(bytes.get(), bytes.size());
  generic().Insert(pos, chunk, 0, bytes.size(), bytes.deallocator());
  bytes.Release();
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::Insert(
    const_iterator pos, UniquePtr<const std::byte[]>&& bytes) {
  static_assert(is_const(),
                "Cannot `Insert` read-only bytes into mutable MultiBuf");
  ConstByteSpan chunk(bytes.get(), bytes.size());
  generic().Insert(pos, chunk, 0, bytes.size(), bytes.deallocator());
  bytes.Release();
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::Insert(const_iterator pos,
                                           const SharedPtr<std::byte[]>& bytes,
                                           size_t offset,
                                           size_t length) {
  ConstByteSpan chunk(bytes.get(), bytes.size());
  generic().Insert(pos, chunk, offset, length, bytes.control_block());
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::Insert(
    const_iterator pos,
    const SharedPtr<const std::byte[]>& bytes,
    size_t offset,
    size_t length) {
  static_assert(is_const(),
                "Cannot `Insert` read-only bytes into mutable MultiBuf");
  ConstByteSpan chunk(bytes.get(), bytes.size());
  generic().Insert(pos, chunk, offset, length, bytes.control_block());
}

template <Property... kProperties>
template <Property... kOtherProperties>
bool BasicMultiBuf<kProperties...>::TryReserveForPushBack(
    const BasicMultiBuf<kOtherProperties...>& mb) {
  return generic().TryReserveForInsert(end(),
                                       static_cast<const GenericMultiBuf&>(mb));
}

template <Property... kProperties>
bool BasicMultiBuf<kProperties...>::TryReserveForPushBack() {
  return generic().TryReserveForInsert(end());
}

template <Property... kProperties>
template <Property... kOtherProperties>
void BasicMultiBuf<kProperties...>::PushBack(
    BasicMultiBuf<kOtherProperties...>&& mb) {
  Insert(end(), std::move(mb));
}

template <Property... kProperties>
template <int&... kExplicitGuard, typename T, typename>
void BasicMultiBuf<kProperties...>::PushBack(const T& bytes) {
  using data_ptr_type = decltype(std::data(std::declval<T&>()));
  static_assert(std::is_same_v<data_ptr_type, std::byte*> || is_const(),
                "Cannot `PushBack` read-only bytes into mutable MultiBuf");
  Insert(end(), bytes);
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::PushBack(UniquePtr<std::byte[]>&& bytes) {
  Insert(end(), std::move(bytes));
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::PushBack(
    UniquePtr<const std::byte[]>&& bytes) {
  static_assert(is_const(),
                "Cannot `PushBack` read-only bytes into mutable MultiBuf");
  Insert(end(), std::move(bytes));
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::PushBack(
    const SharedPtr<std::byte[]>& bytes, size_t offset, size_t length) {
  Insert(end(), bytes, offset, length);
}

template <Property... kProperties>
void BasicMultiBuf<kProperties...>::PushBack(
    const SharedPtr<const std::byte[]>& bytes, size_t offset, size_t length) {
  static_assert(is_const(),
                "Cannot `PushBack` read-only bytes into mutable MultiBuf");
  Insert(end(), bytes, offset, length);
}

template <Property... kProperties>
Result<internal::Instance<BasicMultiBuf<kProperties...>>>
BasicMultiBuf<kProperties...>::Remove(const_iterator pos, size_t size) {
  auto result = generic().Remove(pos, size);
  if (!result.ok()) {
    return result.status();
  }
  return Instance(std::move(*result));
}

template <Property... kProperties>
Result<internal::Instance<BasicMultiBuf<kProperties...>>>
BasicMultiBuf<kProperties...>::PopFrontFragment() {
  auto result = generic().PopFrontFragment();
  if (!result.ok()) {
    return result.status();
  }
  return Instance(std::move(*result));
}

template <Property... kProperties>
UniquePtr<typename BasicMultiBuf<kProperties...>::value_type[]>
BasicMultiBuf<kProperties...>::Release(const_iterator pos) {
  UniquePtr<std::byte[]> bytes = generic().Release(pos);
  if constexpr (is_const()) {
    UniquePtr<const std::byte[]> const_bytes(
        bytes.get(), bytes.size(), *(bytes.deallocator()));
    bytes.Release();
    return const_bytes;
  } else {
    return bytes;
  }
}

template <Property... kProperties>
SharedPtr<typename BasicMultiBuf<kProperties...>::value_type[]>
BasicMultiBuf<kProperties...>::Share(const_iterator pos) {
  return SharedPtr<value_type[]>(generic().Share(pos));
}

}  // namespace pw::multibuf::v2
