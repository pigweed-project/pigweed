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
#pragma once

#include "pw_assert/assert.h"
#include "pw_channel/channel.h"

namespace pw::channel {

// Channel functions that must be implemented inline after AnyChannel.

template <DataType kDataType, Property... kProperties>
constexpr bool Channel<kDataType, kProperties...>::is_read_open() const {
  return readable() && static_cast<const AnyChannel*>(this)->is_read_open();
}

template <DataType kDataType, Property... kProperties>
constexpr bool Channel<kDataType, kProperties...>::is_write_open() const {
  return writable() && static_cast<const AnyChannel*>(this)->is_write_open();
}

template <DataType kDataType, Property... kProperties>
async2::PollResult<multibuf::MultiBuf>
Channel<kDataType, kProperties...>::PendRead(async2::Context& cx) {
  static_assert(readable(), "PendRead may only be called on readable channels");
  return static_cast<AnyChannel*>(this)->PendRead(cx);
}

template <DataType kDataType, Property... kProperties>
async2::Poll<Status> Channel<kDataType, kProperties...>::PendReadyToWrite(
    pw::async2::Context& cx) {
  static_assert(writable(),
                "PendReadyToWrite may only be called on writable channels");
  return static_cast<AnyChannel*>(this)->PendReadyToWrite(cx);
}
template <DataType kDataType, Property... kProperties>
async2::PollOptional<multibuf::MultiBuf>
Channel<kDataType, kProperties...>::PendAllocateWriteBuffer(async2::Context& cx,
                                                            size_t min_bytes) {
  static_assert(
      writable(),
      "PendAllocateWriteBuffer may only be called on writable channels");
  return static_cast<AnyChannel*>(this)->PendAllocateWriteBuffer(cx, min_bytes);
}
template <DataType kDataType, Property... kProperties>
Status Channel<kDataType, kProperties...>::StageWrite(
    multibuf::MultiBuf&& data) {
  static_assert(writable(),
                "StageWrite may only be called on writable channels");
  return static_cast<AnyChannel*>(this)->StageWrite(std::move(data));
}
template <DataType kDataType, Property... kProperties>
async2::Poll<Status> Channel<kDataType, kProperties...>::PendWrite(
    async2::Context& cx) {
  static_assert(writable(),
                "PendWrite may only be called on writable channels");
  return static_cast<AnyChannel*>(this)->PendWrite(cx);
}

template <DataType kDataType, Property... kProperties>
async2::Poll<pw::Status> Channel<kDataType, kProperties...>::PendClose(
    async2::Context& cx) {
  return static_cast<AnyChannel*>(this)->PendClose(cx);
}

namespace internal {

template <DataType kDataType, Property... kProperties>
class BaseChannelImpl : public AnyChannel {
 public:
  using Channel = Channel<kDataType, kProperties...>;

  Channel& channel() { return *this; }
  const Channel& channel() const { return *this; }

  using Channel::as;

  // Use the Channel's version of the AnyChannel API, so unsupported methods are
  // disabled.
  using Channel::PendAllocateWriteBuffer;
  using Channel::PendClose;
  using Channel::PendRead;
  using Channel::PendReadyToWrite;
  using Channel::PendWrite;
  using Channel::StageWrite;

 private:
  friend class ChannelImpl<kDataType, kProperties...>;

  constexpr BaseChannelImpl()
      : AnyChannel(kDataType, (static_cast<uint8_t>(kProperties) | ...)) {}
};

}  // namespace internal

// Defines a channel specialization with the specified properties. ChannelImpl
// is specialized for each supported combination of byte/datagram and read/write
// attribute. Invalid combinations fall back to the default implementation and
// fail a static_assert.
//
// Channel is specialized to implement unsupported operations in a standard way.
// That way, extending a channel only requires implementing supported functions.
#define _PW_CHANNEL_IMPL(type, ...)                               \
  template <>                                                     \
  class ChannelImpl<DataType::__VA_ARGS__>                        \
      : public internal::BaseChannelImpl<DataType::__VA_ARGS__> { \
   protected:                                                     \
    explicit constexpr ChannelImpl() = default;                   \
                                                                  \
   private:                                                       \
    _PW_CHANNEL_##type                                            \
  }

// Macros that stub out read/write if unsupported.
#define _PW_CHANNEL_READ_WRITE

#define _PW_CHANNEL_WRITE_ONLY                                                 \
  async2::PollResult<multibuf::MultiBuf> DoPendRead(async2::Context&) final {  \
    return async2::Ready(Result<multibuf::MultiBuf>(Status::Unimplemented())); \
  }

#define _PW_CHANNEL_READ_ONLY                                             \
  async2::Poll<Status> DoPendReadyToWrite(async2::Context&) final {       \
    return Status::Unimplemented();                                       \
  }                                                                       \
  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(     \
      async2::Context&, size_t) final {                                   \
    PW_ASSERT(false); /* shouldn't be called on non-writeable channels */ \
  }                                                                       \
  Status DoStageWrite(multibuf::MultiBuf&&) final {                       \
    return Status::Unimplemented();                                       \
  }                                                                       \
  async2::Poll<Status> DoPendWrite(async2::Context&) final {              \
    return async2::Ready(Status::Unimplemented());                        \
  }

// Generate specializations for the supported channel types.
_PW_CHANNEL_IMPL(READ_WRITE, kByte, kReliable, kReadable, kWritable);
_PW_CHANNEL_IMPL(READ_ONLY, kByte, kReliable, kReadable);
_PW_CHANNEL_IMPL(WRITE_ONLY, kByte, kReliable, kWritable);

_PW_CHANNEL_IMPL(READ_WRITE, kByte, kReadable, kWritable);
_PW_CHANNEL_IMPL(READ_ONLY, kByte, kReadable);
_PW_CHANNEL_IMPL(WRITE_ONLY, kByte, kWritable);

_PW_CHANNEL_IMPL(READ_WRITE, kDatagram, kReliable, kReadable, kWritable);
_PW_CHANNEL_IMPL(READ_ONLY, kDatagram, kReliable, kReadable);
_PW_CHANNEL_IMPL(WRITE_ONLY, kDatagram, kReliable, kWritable);

_PW_CHANNEL_IMPL(READ_WRITE, kDatagram, kReadable, kWritable);
_PW_CHANNEL_IMPL(READ_ONLY, kDatagram, kReadable);
_PW_CHANNEL_IMPL(WRITE_ONLY, kDatagram, kWritable);

#undef _PW_CHANNEL_IMPL
#undef _PW_CHANNEL_READ_WRITE
#undef _PW_CHANNEL_READ_ONLY
#undef _PW_CHANNEL_WRITE_ONLY

}  // namespace pw::channel
