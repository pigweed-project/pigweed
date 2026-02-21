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

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>
#include <pw_string/utf_codecs.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/unique_ptr.h"

namespace bt {

void ByteBuffer::Copy(MutableByteBuffer* out_buffer) const {
  PW_CHECK(out_buffer);
  CopyRaw(out_buffer->mutable_data(), out_buffer->size(), 0, size());
}

void ByteBuffer::Copy(MutableByteBuffer* out_buffer,
                      size_t pos,
                      size_t size) const {
  PW_CHECK(out_buffer);
  CopyRaw(out_buffer->mutable_data(), out_buffer->size(), pos, size);
}

std::string ByteBuffer::Printable(size_t pos, size_t size) const {
  PW_CHECK(pos + size <= this->size());
  const char* region_start = reinterpret_cast<const char*>(data() + pos);
  std::string_view view(region_start, size);

  // If the region already contains only valid UTF-8 characters, it's already
  // printable
  if (pw::utf8::IsStringValid(view)) {
    return std::string(view);
  }

  std::string ret(size, '\0');
  for (size_t i = 0; i < size; i++) {
    if (std::isprint(view[i])) {
      ret[i] = view[i];
    } else {
      ret[i] = '.';
    }
  }

  return ret;
}

BufferView ByteBuffer::view(size_t pos, size_t size) const {
  PW_CHECK(pos <= this->size(),
           "offset past buffer (pos: %zu, size: %zu)",
           pos,
           this->size());
  return BufferView(data() + pos, std::min(size, this->size() - pos));
}

pw::span<const std::byte> ByteBuffer::subspan(size_t pos, size_t size) const {
  PW_CHECK(pos <= this->size(),
           "offset past buffer (pos: %zu, size: %zu)",
           pos,
           this->size());
  return pw::span(reinterpret_cast<const std::byte*>(data()) + pos,
                  std::min(size, this->size() - pos));
}

std::string_view ByteBuffer::AsString() const {
  return std::string_view(reinterpret_cast<const char*>(data()), size());
}

std::string ByteBuffer::AsHexadecimal() const {
  std::string formatted_string;
  for (size_t i = 0; i < size(); ++i) {
    bt_lib_cpp_string::StringAppendf(
        &formatted_string, "%02x", static_cast<int>(data()[i]));
    if (i < size() - 1) {
      formatted_string += " ";
    }
  }
  return formatted_string;
}

std::string ByteBuffer::ToString(bool as_hex) const {
  if (as_hex) {
    return AsHexadecimal();
  }
  return std::string(AsString());
}

std::vector<uint8_t> ByteBuffer::ToVector() const {
  std::vector<uint8_t> vec(size());
  MutableBufferView vec_view(vec.data(), vec.size());
  vec_view.Write(*this);
  return vec;
}

void ByteBuffer::CopyRaw(void* dst_data,
                         size_t dst_capacity,
                         size_t src_offset,
                         size_t copy_size) const {
  PW_CHECK(copy_size == 0 || dst_data != nullptr,
           "%zu byte write to pointer %p",
           copy_size,
           dst_data);
  PW_CHECK(copy_size <= dst_capacity,
           "destination not large enough (required: %zu, available: %zu)",
           copy_size,
           dst_capacity);
  PW_CHECK(src_offset <= this->size(),
           "offset exceeds source range (begin: %zu, copy_size: %zu)",
           src_offset,
           this->size());
  PW_CHECK(
      std::numeric_limits<size_t>::max() - copy_size >= src_offset,
      "end of source range overflows size_t (src_offset: %zu, copy_size: %zu)",
      src_offset,
      copy_size);
  PW_CHECK(src_offset + copy_size <= this->size(),
           "end exceeds source range (end: %zu, copy_size: %zu)",
           src_offset + copy_size,
           this->size());

  // Data pointers for zero-length buffers are nullptr, over which memcpy has
  // undefined behavior, even for count = 0. Skip the memcpy invocation in that
  // case.
  if (copy_size == 0) {
    return;
  }
  std::memcpy(dst_data, data() + src_offset, copy_size);
}

void MutableByteBuffer::Write(const uint8_t* data, size_t size, size_t pos) {
  BufferView from(data, size);
  MutableBufferView to = mutable_view(pos);
  from.Copy(&to);
}

MutableBufferView MutableByteBuffer::mutable_view(size_t pos, size_t size) {
  PW_CHECK(pos <= this->size(),
           "offset past buffer (pos: %zu, size: %zu)",
           pos,
           this->size());
  return MutableBufferView(mutable_data() + pos,
                           std::min(size, this->size() - pos));
}

pw::span<std::byte> MutableByteBuffer::mutable_subspan(size_t pos,
                                                       size_t size) {
  PW_CHECK(pos <= this->size(),
           "offset past buffer (pos: %zu, size: %zu)",
           pos,
           this->size());
  return pw::span(reinterpret_cast<std::byte*>(mutable_data()) + pos,
                  std::min(size, this->size() - pos));
}

DynamicByteBuffer::DynamicByteBuffer() = default;

DynamicByteBuffer::DynamicByteBuffer(size_t buffer_size) {
  if (buffer_size == 0) {
    return;
  }

  buffer_ =
      pw::allocator::GetLibCAllocator().MakeUnique<std::byte[]>(buffer_size);

  // TODO(armansito): For now this is dumb but we should properly handle the
  // case when we're out of memory.
  PW_CHECK(buffer_ != nullptr, "failed to allocate buffer");

  std::memset(buffer_.get(), 0, buffer_size);
}

DynamicByteBuffer::DynamicByteBuffer(const ByteBuffer& buffer)
    : buffer_(buffer.size()
                  ? pw::allocator::GetLibCAllocator().MakeUnique<std::byte[]>(
                        buffer.size())
                  : nullptr) {
  PW_CHECK(!buffer.size() || buffer_ != nullptr,
           "|buffer| cannot be nullptr when |buffer_size| is non-zero");
  buffer.Copy(this);
}

DynamicByteBuffer::DynamicByteBuffer(const DynamicByteBuffer& buffer)
    : DynamicByteBuffer(static_cast<const ByteBuffer&>(buffer)) {}

DynamicByteBuffer::DynamicByteBuffer(const std::string& buffer) {
  buffer_ = pw::allocator::GetLibCAllocator().MakeUnique<std::byte[]>(
      buffer.length());
  PW_CHECK(buffer_ != nullptr, "failed to allocate buffer");
  memcpy(buffer_.get(), buffer.data(), buffer.length());
}

DynamicByteBuffer::DynamicByteBuffer(size_t buffer_size,
                                     pw::UniquePtr<std::byte[]> buffer)
    : buffer_(std::move(buffer)) {
  PW_CHECK(!buffer_size || buffer_ != nullptr,
           "|buffer| cannot be nullptr when |buffer_size| is non-zero");
}

DynamicByteBuffer::DynamicByteBuffer(DynamicByteBuffer&& other) {
  buffer_ = std::move(other.buffer_);
}

DynamicByteBuffer& DynamicByteBuffer::operator=(DynamicByteBuffer&& other) {
  buffer_ = std::move(other.buffer_);
  return *this;
}

const uint8_t* DynamicByteBuffer::data() const {
  return reinterpret_cast<const uint8_t*>(buffer_.get());
}

uint8_t* DynamicByteBuffer::mutable_data() {
  return reinterpret_cast<uint8_t*>(buffer_.get());
}

size_t DynamicByteBuffer::size() const {
  return buffer_ == nullptr ? 0 : buffer_.size();
}

void DynamicByteBuffer::Fill(uint8_t value) {
  std::memset(buffer_.get(), value, size());
}

bool DynamicByteBuffer::expand(size_t new_buffer_size) {
  // we only allow growing the buffer, not shrinking it
  if (new_buffer_size < size()) {
    return false;
  }

  // no reason to do extra work
  if (new_buffer_size == size()) {
    return false;
  }

  pw::UniquePtr<std::byte[]> new_buffer =
      pw::allocator::GetLibCAllocator().MakeUnique<std::byte[]>(
          new_buffer_size);
  if (new_buffer == nullptr) {
    return false;
  }

  std::memset(new_buffer.get() + size(), 0, new_buffer.size() - size());

  // Handle the case where the default constructor was used and no actual buffer
  // data was initialized.
  if (buffer_ != nullptr) {
    std::memcpy(new_buffer.get(), buffer_.get(), size());
  }

  buffer_.Swap(new_buffer);
  return true;
}

ByteBuffer::const_iterator DynamicByteBuffer::cbegin() const {
  return reinterpret_cast<ByteBuffer::const_iterator>(buffer_.get());
}

ByteBuffer::const_iterator DynamicByteBuffer::cend() const {
  return reinterpret_cast<ByteBuffer::const_iterator>(buffer_.get()) + size();
}

BufferView::BufferView(const ByteBuffer& buffer, size_t size) {
  *this = buffer.view(0u, size);
}

BufferView::BufferView(std::string_view string) {
  size_ = string.size();
  bytes_ = reinterpret_cast<const uint8_t*>(string.data());
}

BufferView::BufferView(const std::vector<uint8_t>& vec)
    : BufferView(vec.data(), vec.size()) {}

BufferView::BufferView(pw::span<const std::byte> bytes)
    : BufferView(bytes.data(), bytes.size()) {}

BufferView::BufferView(const void* bytes, size_t size)
    : size_(size), bytes_(static_cast<const uint8_t*>(bytes)) {
  // If |size| non-zero then |bytes| cannot be nullptr.
  PW_CHECK(!size_ || bytes_, "|bytes_| cannot be nullptr if |size_| > 0");
}

BufferView::BufferView() = default;

const uint8_t* BufferView::data() const { return bytes_; }

size_t BufferView::size() const { return size_; }

ByteBuffer::const_iterator BufferView::cbegin() const { return bytes_; }

ByteBuffer::const_iterator BufferView::cend() const { return bytes_ + size_; }

MutableBufferView::MutableBufferView(MutableByteBuffer* buffer) {
  PW_CHECK(buffer);
  size_ = buffer->size();
  bytes_ = buffer->mutable_data();
}

MutableBufferView::MutableBufferView(void* bytes, size_t size)
    : size_(size), bytes_(static_cast<uint8_t*>(bytes)) {
  // If |size| non-zero then |bytes| cannot be nullptr.
  PW_CHECK(!size_ || bytes_, "|bytes_| cannot be nullptr if |size_| > 0");
}

MutableBufferView::MutableBufferView() = default;

const uint8_t* MutableBufferView::data() const { return bytes_; }

size_t MutableBufferView::size() const { return size_; }

ByteBuffer::const_iterator MutableBufferView::cbegin() const { return bytes_; }

ByteBuffer::const_iterator MutableBufferView::cend() const {
  return bytes_ + size_;
}

uint8_t* MutableBufferView::mutable_data() { return bytes_; }

void MutableBufferView::Fill(uint8_t value) { memset(bytes_, value, size_); }

}  // namespace bt
