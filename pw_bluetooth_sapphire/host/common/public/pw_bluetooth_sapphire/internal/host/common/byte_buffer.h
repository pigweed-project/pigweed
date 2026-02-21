// Copyright 2023 The Pigweed Authors
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
#include <cpp-type/member_pointer_traits.h>
#include <cpp-type/to_std_array.h>
#include <pw_assert/assert.h>
#include <pw_assert/check.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "pw_allocator/unique_ptr.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_span/span.h"

namespace bt {

class BufferView;
class MutableBufferView;
class MutableByteBuffer;

// Interface for buffer implementations with various allocation schemes.
class ByteBuffer {
 public:
  using const_iterator = const uint8_t*;
  using iterator = const_iterator;
  using value_type = uint8_t;

  virtual ~ByteBuffer() = default;

  // Returns a pointer to the beginning of this buffer. The return value is
  // undefined if the buffer has size 0.
  virtual const uint8_t* data() const = 0;

  // Returns the number of bytes contained in this buffer.
  virtual size_t size() const = 0;

  // Returns true if size() == 0, false otherwise
  bool empty() const { return size() == 0; }

  // Returns a BufferView that points to the region of this buffer starting at
  // |pos| of |size| bytes. If |size| is larger than the size of this BufferView
  // then the returned region will contain all bytes in this buffer starting at
  // |pos|.
  //
  // For example:
  //
  //  // Get a view of all of |my_buffer|.
  //  const BufferView view = my_buffer.view();
  //
  //  // Get a view of the first 5 bytes in |my_buffer| (assuming |my_buffer| is
  //  // large enough).
  //  view = my_buffer.view(0, 5);
  //
  //  // Get a view of |my_buffer| starting at the second byte.
  //  view = my_buffer.view(2);
  //
  //
  // WARNING: A BufferView is only valid as long as the buffer that it points to
  // is valid. Care should be taken to ensure that a BufferView does not outlive
  // its backing buffer.
  BufferView view(size_t pos = 0,
                  size_t size = std::numeric_limits<std::size_t>::max()) const;

  // Same as view(), but returns a span instead of a BufferView.
  pw::span<const std::byte> subspan(
      size_t pos = 0,
      size_t size = std::numeric_limits<std::size_t>::max()) const;

  // Copies all bytes of this buffer into |out_buffer|. |out_buffer| must be
  // large enough to accommodate the result of this operation.
  void Copy(MutableByteBuffer* out_buffer) const;

  // Copies |size| bytes of this buffer into |out_buffer| starting at offset
  // |pos|. |out_buffer| must be large enough to accommodate the result of this
  // operation.
  void Copy(MutableByteBuffer* out_buffer, size_t pos, size_t size) const;

  // Creates a new std::string that contains a printable representation of a
  // range of this buffer starting at |pos|. The string is checked to see if it
  // is UTF-8. If not, each byte in the range to be converted is checked to see
  // if it is printable ASCII. If so, the character is used as is. If not, it is
  // replaced by '.'. The returned std::string will have size |size| + 1 to fit
  // a terminating '\0'.
  std::string Printable(size_t pos, size_t size) const;

  // Iterator functions.
  iterator begin() const { return cbegin(); }
  iterator end() const { return cend(); }
  virtual const_iterator cbegin() const = 0;
  virtual const_iterator cend() const = 0;

  // Read-only random access operator.
  inline const uint8_t& operator[](size_t pos) const {
    PW_CHECK(pos < size(), "invalid offset (pos = %zu)", pos);
    return data()[pos];
  }

  // Creates an object of type T with the first sizeof(T) bytes of the buffer as
  // its representation (per definition at ISO/IEC 14882:2017(E) § 6.9
  // [basic.types] ¶ 4.4). The user is responsible for checking that the first
  // sizeof(T) bytes represent a valid instance of T. If T is an array type, the
  // return value will be a std::array with the same element type and extents.
  //
  // This or ReadMember should always be used in place of reinterpret_cast on
  // raw pointers because of dangerous UB related to object lifetimes and
  // alignment issues (see fxbug.dev/42123294). Moreover, this will perform
  // bounds checking on the data being read.
  template <typename T>
  [[nodiscard]] auto To() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "unsafe to copy representation");
    static_assert(std::is_default_constructible_v<T>);
    using OutType = std::remove_cv_t<bt_lib_cpp_type::ToStdArrayT<T>>;

    // This is value-initialized in order to construct objects that have const
    // members. The consideration for modifying the object through its
    // representation even if the constituent types are cv-qualified is based on
    // the potent rules for memcpy'ing "underlying bytes" at ISO/IEC
    // 14882:2017(E) § 6.9 [basic.types] ¶ 4.2–4.3.
    OutType out{};
    CopyRaw(/*dst_data=*/std::addressof(out),
            /*dst_capacity=*/sizeof(out),
            /*src_offset=*/0,
            /*copy_size=*/sizeof(out));
    return out;
  }

  // Given a pointer to a member of a class, interpret the underlying buffer as
  // a representation of the class and return a copy of the member, with bounds
  // checking for reading the representation. Array elements (including
  // multi-dimensional) will be returned as std::array. The buffer is allowed to
  // be larger than T. The user is responsible for checking that the first
  // sizeof(T) bytes represent a valid instance of T.
  //
  // Example:
  //   struct Foo { float bar[3]; int baz; char qux[]; };
  //   buffer.ReadMember<&Foo::bar>();  // OK, returns std::array<float, 3>
  //   buffer.ReadMember<&Foo::baz>();  // OK, returns int
  //   buffer.ReadMember<&Foo::qux>();  // Asserts, use
  //   ReadMember<&Foo::qux>(index) instead
  //
  // This functions similarly to C-style type punning at address
  //   |buffer.data() + offsetof(Foo, bar)|
  template <auto PointerToMember>
  auto ReadMember() const {
    using ClassT = typename bt_lib_cpp_type::MemberPointerTraits<
        PointerToMember>::ClassType;
    PW_CHECK(sizeof(ClassT) <= this->size(),
             "insufficient buffer (class size: %zu, buffer size: %zu)",
             sizeof(ClassT),
             this->size());
    using MemberT = typename bt_lib_cpp_type::MemberPointerTraits<
        PointerToMember>::MemberType;
    if constexpr (std::is_array_v<MemberT>) {
      static_assert(
          std::extent_v<MemberT> > 0,
          "use indexed overload of ReadMember for flexible array members");
    }
    using ReturnType = std::remove_cv_t<bt_lib_cpp_type::ToStdArrayT<MemberT>>;

    // std::array is required to be an aggregate that's list-initialized per
    // ISO/IEC 14882:2017(E) § 26.3.7.1 [array.overview] ¶ 2, so its layout's
    // initial run is identical to a raw array.
    static_assert(sizeof(MemberT) <= sizeof(ReturnType));
    static_assert(std::is_trivially_copyable_v<MemberT>,
                  "unsafe to copy representation");
    static_assert(std::is_trivially_copyable_v<ReturnType>,
                  "unsafe to copy representation");
    ReturnType out{};
    const size_t offset =
        bt_lib_cpp_type::MemberPointerTraits<PointerToMember>::offset();
    CopyRaw(/*dst_data=*/std::addressof(out),
            /*dst_capacity=*/sizeof(out),
            /*src_offset=*/offset,
            /*copy_size=*/sizeof(MemberT));
    return out;
  }

  // Given a pointer to an array (or smart array) member of a class, interpret
  // the underlying buffer as a representation of the class and return a copy of
  // the member's |index - 1|-th element, with bounds checking for the indexing
  // and reading representation bytes. Multi-dimensional arrays will return
  // array elements as std::array. The buffer is allowed to be larger than T.
  // The user is responsible for checking that the first sizeof(T) bytes
  // represent a valid instance of T.
  //
  // Example:
  //   struct Foo { float bar[3]; int baz; char qux[]; };
  //   buffer.ReadMember<&Foo::bar>(2);  // OK
  //   buffer.ReadMember<&Foo::qux>(3);  // OK, checked against buffer.size()
  //   buffer.ReadMember<&Foo::bar>(3);  // Asserts because out-of-bounds on
  //   Foo::bar
  //
  // This functions similarly to C-style type punning at address
  //   |buffer.data() + offsetof(Foo, bar) + index * sizeof(bar[0])|
  // but performs bounds checking and returns a valid type-punned object.
  template <auto PointerToMember>
  auto ReadMember(size_t index) const {
    // From the ReadMember<&Foo::bar>(2) example, ClassT = Foo
    using ClassT = typename bt_lib_cpp_type::MemberPointerTraits<
        PointerToMember>::ClassType;
    PW_CHECK(sizeof(ClassT) <= this->size(),
             "insufficient buffer (class size: %zu, buffer size: %zu)",
             sizeof(ClassT),
             this->size());

    // From the ReadMember<&Foo::bar>(2) example, MemberT = float[3]
    using MemberT = typename bt_lib_cpp_type::MemberPointerTraits<
        PointerToMember>::MemberType;
    static_assert(std::is_trivially_copyable_v<MemberT>,
                  "unsafe to copy representation");

    // From the ReadMember<&Foo::bar>(2) example, MemberAsStdArrayT =
    // std::array<float, 3>
    using MemberAsStdArrayT = bt_lib_cpp_type::ToStdArrayT<MemberT>;

    // Check array bounds
    constexpr size_t kArraySize = std::tuple_size_v<MemberAsStdArrayT>;
    const size_t base_offset =
        bt_lib_cpp_type::MemberPointerTraits<PointerToMember>::offset();
    if constexpr (kArraySize > 0) {
      // std::array is required to be an aggregate that's list-initialized per
      // ISO/IEC 14882:2017(E) § 26.3.7.1 [array.overview] ¶ 2, so we can rely
      // on the initial run of its layout, but in the technically possible but
      // unlikely case that it contains additional bytes, we can't use its size
      // for array indexing calculations.
      static_assert(sizeof(MemberAsStdArrayT) == sizeof(MemberT));
      PW_CHECK(index < kArraySize,
               "index past array bounds (index: %zu, array size: %zu)",
               index,
               kArraySize);
    } else {
      // Allow flexible array members (at the end of structs) that have zero
      // length
      PW_CHECK(base_offset == sizeof(ClassT), "read from zero-length array");
    }

    // From the ReadMember<&Foo::bar>(2) example, ElementT = float
    using ElementT = std::remove_cv_t<typename MemberAsStdArrayT::value_type>;
    static_assert(std::is_trivially_copyable_v<ElementT>,
                  "unsafe to copy representation");
    const size_t offset = base_offset + index * sizeof(ElementT);
    ElementT element{};
    CopyRaw(/*dst_data=*/std::addressof(element),
            /*dst_capacity=*/sizeof(ElementT),
            /*src_offset=*/offset,
            /*copy_size=*/sizeof(ElementT));
    return element;
  }

  bool operator==(const ByteBuffer& other) const {
    if (size() != other.size()) {
      return false;
    }
    if (empty()) {
      return other.empty();
    }
    return (memcmp(data(), other.data(), size()) == 0);
  }

  // Returns the contents of this buffer as a C++ string-like object without
  // copying its contents.
  std::string_view AsString() const;

  // Returns a string in hexadecimal format.
  std::string AsHexadecimal() const;

  // Returns the contents of this buffer as a C++ string after copying its
  // contents.
  std::string ToString(bool as_hex = false) const;

  // Returns a copy of the contents of this buffer in a std::vector.
  std::vector<uint8_t> ToVector() const;

 private:
  void CopyRaw(void* dst_data,
               size_t dst_capacity,
               size_t src_offset,
               size_t copy_size) const;
};

using ByteBufferPtr = std::unique_ptr<ByteBuffer>;

// Mutable extension to the ByteBuffer interface. This provides methods that
// allows direct mutable access to the underlying buffer.
class MutableByteBuffer : public ByteBuffer {
 public:
  ~MutableByteBuffer() override = default;

  // Returns a pointer to the beginning of this buffer. The return value is
  // undefined if the buffer has size 0.
  virtual uint8_t* mutable_data() = 0;

  // Random access operator that allows mutations.
  inline uint8_t& operator[](size_t pos) {
    PW_CHECK(pos < size(), "invalid offset (pos = %zu)", pos);
    return mutable_data()[pos];
  }

  // Read-only random access operator. Required because there is no overload
  // resolution from derived to base classes - without this, |const
  // MutableByteBuffer|s cannot use operator[].
  uint8_t operator[](size_t pos) const { return ByteBuffer::operator[](pos); }

  // Converts the underlying buffer to a mutable reference to the given type,
  // with bounds checking. The buffer is allowed to be larger than T. The user
  // is responsible for checking that the first sizeof(T) bytes represents a
  // valid instance of T.
  template <typename T>
  T* AsMutable() {
    static_assert(std::is_trivially_copyable_v<T>);
    PW_CHECK(size() >= sizeof(T));
    return reinterpret_cast<T*>(mutable_data());
  }

  // Writes the contents of |data| into this buffer starting at |pos|.
  inline void Write(const ByteBuffer& data, size_t pos = 0) {
    Write(data.data(), data.size(), pos);
  }

  // Writes |size| octets of data starting from |data| into this buffer starting
  // at |pos|. |data| must point to a valid piece of memory if |size| is
  // non-zero. If |size| is zero, then this operation is a NOP.
  void Write(const uint8_t* data, size_t size, size_t pos = 0);

  // Writes the byte interpretation of |data| at |pos|, overwriting the octets
  // from pos to pos + sizeof(T).
  // There must be enough space in the buffer to write T.
  // If T is an array of known bounds, the entire array will be written.
  template <typename T>
  void WriteObj(const T& data, size_t pos = 0) {
    // ByteBuffers are (mostly?) not TriviallyCopyable, but check this first for
    // the error to be useful.
    static_assert(!std::is_base_of_v<ByteBuffer, T>,
                  "ByteBuffer passed to WriteObj; use Write");
    static_assert(!std::is_pointer_v<T>,
                  "Pointer passed to WriteObj, deref or use Write");
    static_assert(std::is_trivially_copyable_v<T>,
                  "Unsafe to peek byte representation");
    Write(reinterpret_cast<const uint8_t*>(&data), sizeof(T), pos);
  }

  // Behaves exactly like ByteBuffer::View but returns the result in a
  // MutableBufferView instead.
  //
  // WARNING: A BufferView is only valid as long as the buffer that it points to
  // is valid. Care should be taken to ensure that a BufferView does not outlive
  // its backing buffer.
  MutableBufferView mutable_view(
      size_t pos = 0, size_t size = std::numeric_limits<std::size_t>::max());

  // Same as mutable_view(), but returns a mutable span instead of a
  // MutableBufferView.
  pw::span<std::byte> mutable_subspan(
      size_t pos = 0, size_t size = std::numeric_limits<std::size_t>::max());

  // Sets the contents of the buffer to 0s.
  void SetToZeros() { Fill(0); }

  // Fills the contents of the buffer with the given value.
  virtual void Fill(uint8_t value) = 0;
};

using MutableByteBufferPtr = std::unique_ptr<MutableByteBuffer>;

// A ByteBuffer with static storage duration. Instances of this class are
// copyable. Due to the static buffer storage duration, move semantics work the
// same way as copy semantics, i.e. moving an instance will copy the buffer
// contents.
template <size_t BufferSize>
class StaticByteBuffer : public MutableByteBuffer {
 public:
  // Create a buffer of size |BufferSize|. The buffer bytes will be initialized
  // to 0x00.
  StaticByteBuffer() {
    static_assert(BufferSize, "|BufferSize| must be non-zero");
  }
  ~StaticByteBuffer() override = default;

  // Variadic template constructor to initialize a StaticByteBuffer using a
  // parameter pack e.g.:
  //
  //   StaticByteBuffer foo(0x00, 0x01, 0x02);
  //   StaticByteBuffer<3> foo(0x00, 0x01, 0x02);
  //
  // The class's |BufferSize| template parameter, if explicitly provided, will
  // be checked against the number of initialization elements provided.
  //
  // All types castable to uint8_t can be used without casting (including class
  // enums) for brevity but care must be taken not to exceed uint8_t range
  // limits.
  //
  //   StaticByteBuffer foo(-257);  // -257 has type int and will likely convert
  //   to uint8_t{0xff}
  template <typename... T>
  constexpr explicit StaticByteBuffer(T... bytes)
      : buffer_{{static_cast<uint8_t>(bytes)...}} {
    static_assert(BufferSize, "|BufferSize| must be non-zero");
    static_assert(BufferSize == sizeof...(T),
                  "|BufferSize| must match initializer list count");

    // Check that arguments are within byte range. Restrict checking to smaller
    // inputs to limit compile time impact and because clang considers fold
    // expressions "nested" (i.e. subject to a default 256 depth limit).
    if constexpr (sizeof...(bytes) <= 256) {
      constexpr auto is_byte_storable = [](auto value) {
        if constexpr (sizeof(value) > sizeof(uint8_t)) {
          return static_cast<std::make_unsigned_t<decltype(value)>>(value) <=
                 std::numeric_limits<uint8_t>::max();
        }
        return true;
      };

      // This is a runtime assert because this class was written to work with
      // non-constant values but most uses of StaticByteBuffer are in tests so
      // this is an acceptable cost.
      PW_DASSERT((is_byte_storable(bytes) && ...));
    }
  }

  // ByteBuffer overrides
  const uint8_t* data() const override { return buffer_.data(); }
  size_t size() const override { return buffer_.size(); }
  const_iterator cbegin() const override { return &*buffer_.cbegin(); }
  const_iterator cend() const override { return &*buffer_.cend(); }

  // MutableByteBuffer overrides:
  uint8_t* mutable_data() override { return buffer_.data(); }
  void Fill(uint8_t value) override { buffer_.fill(value); }

  // `constexpr virtual` is a C++20 feature, so no constexpr `size()` yet.
  constexpr size_t static_size() const { return buffer_.size(); }

 private:
  // Value-initialize to 0.
  std::array<uint8_t, BufferSize> buffer_{};
};

// Template deduction guide for the |BufferSize| class template parameter using
// the number of parameters passed into the templated parameter pack
// constructor. This allows |BufferSize| to be omitted when it should be deduced
// from the initializer:
//
//   StaticByteBuffer buffer(0x00, 0x01, 0x02);
//
template <typename... T>
StaticByteBuffer(T... bytes) -> StaticByteBuffer<sizeof...(T)>;

// A ByteBuffer with dynamic storage duration. The underlying buffer is
// allocated using malloc. Instances of this class are move-only.
class DynamicByteBuffer : public MutableByteBuffer {
 public:
  // The default constructor creates an empty buffer with size 0.
  DynamicByteBuffer();
  ~DynamicByteBuffer() override = default;

  // Allocates a new buffer with |buffer_size| bytes. The buffer bytes will be
  // initialized to 0x00.
  explicit DynamicByteBuffer(size_t buffer_size);

  // Copies the contents of |buffer|.
  DynamicByteBuffer(const DynamicByteBuffer& buffer);
  explicit DynamicByteBuffer(const ByteBuffer& buffer);
  explicit DynamicByteBuffer(const std::string& buffer);

  // Takes ownership of |buffer| and avoids allocating a new buffer. Since this
  // constructor performs a simple assignment, the caller must make sure that
  // the buffer pointed to by |buffer| actually contains |buffer_size| bytes.
  DynamicByteBuffer(size_t buffer_size, pw::UniquePtr<std::byte[]> buffer);

  // Move constructor and assignment operator
  DynamicByteBuffer(DynamicByteBuffer&& other);
  DynamicByteBuffer& operator=(DynamicByteBuffer&& other);

  // Copy assignment is prohibited.
  DynamicByteBuffer& operator=(const DynamicByteBuffer&) = delete;

  // ByteBuffer overrides:
  const uint8_t* data() const override;
  size_t size() const override;
  const_iterator cbegin() const override;
  const_iterator cend() const override;

  // MutableByteBuffer overrides:
  uint8_t* mutable_data() override;
  void Fill(uint8_t value) override;

  // Allocates a new buffer of the given size and copies all underlying data to
  // the new buffer. This method is meant only to grow the underlying buffer to
  // fit in more data. Returns false if the new buffer size is less than the
  // current buffer size.
  bool expand(size_t new_buffer_size);

 private:
  // Pointer to the underlying buffer, which is owned and managed by us.
  pw::UniquePtr<std::byte[]> buffer_;
};

// A ByteBuffer that does not own the memory that it points to but rather
// provides an immutable view over it.
//
// WARNING: A BufferView is only valid as long as the buffer that it points to
// is valid. Care should be taken to ensure that a BufferView does not outlive
// its backing buffer.
class BufferView final : public ByteBuffer {
 public:
  BufferView(const void* bytes, size_t size);
  ~BufferView() override = default;

  explicit BufferView(const ByteBuffer& buffer,
                      size_t size = std::numeric_limits<std::size_t>::max());
  explicit BufferView(std::string_view string);
  explicit BufferView(const std::vector<uint8_t>& vec);
  explicit BufferView(pw::span<const std::byte> bytes);

  // The default constructor initializes this to an empty buffer.
  BufferView();

  // ByteBuffer overrides:
  const uint8_t* data() const override;
  size_t size() const override;
  const_iterator cbegin() const override;
  const_iterator cend() const override;

 private:
  size_t size_ = 0u;
  const uint8_t* bytes_ = nullptr;
};

// A ByteBuffer that does not own the memory that it points to but rather
// provides a mutable view over it.
//
// WARNING: A BufferView is only valid as long as the buffer that it points to
// is valid. Care should be taken to ensure that a BufferView does not outlive
// its backing buffer.
class MutableBufferView final : public MutableByteBuffer {
 public:
  explicit MutableBufferView(MutableByteBuffer* buffer);
  MutableBufferView(void* bytes, size_t size);
  ~MutableBufferView() override = default;

  // The default constructor initializes this to an empty buffer.
  MutableBufferView();

  // ByteBuffer overrides:
  const uint8_t* data() const override;
  size_t size() const override;
  const_iterator cbegin() const override;
  const_iterator cend() const override;

  // MutableByteBuffer overrides:
  uint8_t* mutable_data() override;
  void Fill(uint8_t value) override;

 private:
  size_t size_ = 0u;
  uint8_t* bytes_ = nullptr;
};

}  // namespace bt
