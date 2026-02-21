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
#include <pw_assert/assert.h>

#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/v2/multibuf.h"

namespace bt {

// This file defines classes which provide the interface for constructing HCI
// packets and reading/writing them using Emboss
// (https://github.com/google/emboss).
//
// Emboss does not own memory; it provides structured views into user allocated
// memory. These views are specified in Emboss source files such as hci.emb in
// pw_bluetooth, which implements the HCI protocol packet definitions.
//
// This file defines two classes: StaticPacket, which provides an Emboss view
// over a statically allocated buffer, and DynamicPacket, which is part of a
// class hierarchy that provides Emboss views over dynamic memory.
//
// EXAMPLE:
//
// Consider the following Emboss definition of the HCI Command packet header and
// Inquiry Command.
//
//  [(cpp) namespace: "bt::hci_spec"]
//  struct CommandHeader:
//    0     [+2] OpCodeBits opcode
//    $next [+1] UInt parameter_total_size
//
//  struct InquiryCommand:
//    let hdr_size = CommandHeader.$size_in_bytes
//    0     [+hdr_size] CommandHeader header
//    $next [+3] InquiryAccessCode lap
//    $next [+1] UInt inquiry_length
//    $next [+1] UInt num_responses
//
// The Emboss compiler generates two types of view for each struct. In the case
// of InquiryCommand, it generates InquiryCommandView (read only) and
// InquiryCommandWriter (read write). We can parameterize StaticPacket over
// one of these views to read and/or write an Inquiry packet:
//
//  bt::StaticPacket<pw::bluetooth::emboss::InquiryCommandWriter> packet;
//  auto view = packet.view();
//  view.inquiry_length().Write(100);
//  view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
//  cout << "inquiry_length = " << view.inquiry_length().Read();
//
// StaticPacket does not currently support packets with variable length.
template <typename T>
class StaticPacket {
 public:
  StaticPacket() = default;

  // Copy this packet from another view.
  template <typename U>
  explicit StaticPacket(const U& other) {
    view().CopyFrom(other);
  }

  // Returns an Emboss view over the buffer. Emboss views consist of two
  // pointers and a length, so they are cheap to construct on-demand.
  template <typename... Args>
  T view(Args... args) {
    T view(args..., buffer_.mutable_data(), buffer_.size());
    PW_ASSERT(view.IsComplete());
    return view;
  }

  template <typename... Args>
  T view(Args... args) const {
    T view(args..., buffer_.data(), buffer_.size());
    PW_ASSERT(view.IsComplete());
    return view;
  }

  BufferView data() const { return {buffer_.data(), buffer_.size()}; }
  MutableBufferView mutable_data() {
    return {buffer_.mutable_data(), buffer_.size()};
  }
  void SetToZeros() { buffer_.SetToZeros(); }

 private:
  // The intrinsic size of an Emboss struct is the size required to hold all of
  // its fields. An Emboss view has a static IntrinsicSizeInBytes() accessor if
  // the struct does not have dynamic length (i.e. not a variable length
  // packet).
  StaticByteBuffer<T::IntrinsicSizeInBytes().Read()> buffer_;
};

// DynamicPacket is the parent class of a two-level class hierarchy that
// implements dynamically-allocated HCI packets to which reading/writing is
// mediated by Emboss.
//
// DynamicPacket contains data and methods that are universal across packet
// type. Its children are packet type specializations, i.e. Command, Event, ACL,
// and Sco packets. These classes provide header-type-specific functionality.
//
// Instances of DynamicPacket should not be constructed directly. Instead,
// packet type specialization classes should provide static factory functions.
//
// See CommandPacket in control_packets.h for an example of a packet type
// specialization.
class DynamicPacket {
 public:
  // Returns an Emboss view over the buffer. Unlike StaticPacket, which ensures
  // type security as a struct parameterized over a particular Emboss view type,
  // DynamicPacket is a generic type for all packets, so view() is to be
  // parameterized over an Emboss view type on each call.
  template <typename T, typename... Args>
  T view(Args... args) {
    T view(args..., buffer_.mutable_data(), size());
    PW_ASSERT(view.IsComplete());
    return view;
  }

  template <typename T, typename... Args>
  T view(Args... args) const {
    T view(args..., buffer_.data(), size());
    PW_ASSERT(view.IsComplete());
    return view;
  }

  template <typename T, typename... Args>
  T unchecked_view(Args... args) {
    return T(args..., buffer_.mutable_data(), size());
  }

  template <typename T, typename... Args>
  T unchecked_view(Args... args) const {
    return T(args..., buffer_.data(), size());
  }

  // Returns the size of the packet, i.e. payload size + header size.
  size_t size() const { return buffer_.size(); }
  BufferView data() const { return {buffer_.data(), size()}; }
  MutableBufferView mutable_data() { return {buffer_.mutable_data(), size()}; }
  DynamicByteBuffer release() { return std::move(buffer_); }

 protected:
  // Construct the buffer to hold |packet_size| bytes (payload + header).
  explicit DynamicPacket(size_t packet_size) : buffer_(packet_size) {}

  explicit DynamicPacket(pw::multibuf::v2::MultiBuf::Instance&& buf) {
    pw::multibuf::v2::MultiBuf::Instance moved_buf = std::move(buf);
    const size_t size = moved_buf->size();
    if (size == 0) {
      buffer_ = DynamicByteBuffer(0);
      return;
    }
    if (moved_buf->NumFragments() == 1 &&
        moved_buf->IsReleasable(moved_buf->begin())) {
      buffer_ = DynamicByteBuffer(size, moved_buf->Release(moved_buf->begin()));
    } else {
      buffer_ = DynamicByteBuffer(size);
      moved_buf->CopyTo(
          pw::ByteSpan(reinterpret_cast<std::byte*>(buffer_.mutable_data()),
                       buffer_.size()));
    }
  }

 private:
  DynamicByteBuffer buffer_;
};

}  // namespace bt
