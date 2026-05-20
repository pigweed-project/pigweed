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

//         __      ___   ___ _  _ ___ _  _  ___
//         \ \    / /_\ | _ \ \| |_ _| \| |/ __|
//          \ \/\/ / _ \|   / .` || || .` | (_ |
//           \_/\_/_/ \_\_|_\_|\_|___|_|\_|\___|
//  _____  _____ ___ ___ ___ __  __ ___ _  _ _____ _   _
// | __\ \/ / _ \ __| _ \_ _|  \/  | __| \| |_   _/_\ | |
// | _| >  <|  _/ _||   /| || |\/| | _|| .` | | |/ _ \| |__
// |___/_/\_\_| |___|_|_\___|_|  |_|___|_|\_| |_/_/ \_\____|
//
// This module is in an early, experimental state. The APIs are in flux and may
// change without notice. Please do not rely on it in production code, but feel
// free to explore and share feedback with the Pigweed team!

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_bytes/span.h"
#include "pw_channel/properties.h"
#include "pw_multibuf/multibuf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::channel {

/// @module{pw_channel}

/// The basic `Channel` type. Unlike `AnyChannel`, the `Channel`'s properties
/// are expressed in template parameters and thus reflected in the type.
///
/// Properties must be specified in order (`kDatagram`, `kReliable`,
/// `kReadable`, `kWritable`, `kSeekable`) and without duplicates.
///
/// To implement a `Channel`, inherit from `ChannelImpl` with the specified
/// properties.
///
/// @deprecated `Channel` has not been updated for pw_async2 futures or the new
/// `pw::MultiBuf`. It will be updated and renamed to avoid conflicts with
/// `pw::async2::Channel`s.
template <DataType kDataType, Property... kProperties>
class Channel {
 public:
  /// Returns the data type of this channel.
  [[nodiscard]] static constexpr DataType data_type() { return kDataType; }

  /// Returns whether the channel type is reliable.
  [[nodiscard]] static constexpr bool reliable() {
    return ((kProperties == Property::kReliable) || ...);
  }
  /// Returns whether the channel type is seekable.
  [[nodiscard]] static constexpr bool seekable() {
    return ((kProperties == Property::kSeekable) || ...);
  }
  /// Returns whether the channel type is readable.
  [[nodiscard]] static constexpr bool readable() {
    return ((kProperties == Property::kReadable) || ...);
  }
  /// Returns whether the channel type is writable.
  [[nodiscard]] static constexpr bool writable() {
    return ((kProperties == Property::kWritable) || ...);
  }

  /// True if the channel type supports and is open for reading. Always false
  /// for write-only channels.
  [[nodiscard]] constexpr bool is_read_open() const;

  /// True if the channel type supports and is open for writing. Always false
  /// for read-only channels.
  [[nodiscard]] constexpr bool is_write_open() const;

  /// True if the channel is open for reading or writing.
  [[nodiscard]] constexpr bool is_read_or_write_open() const {
    return is_read_open() || is_write_open();
  }

  /// @copydoc AnyChannel::PendRead
  async2::PollResult<multibuf::MultiBuf> PendRead(async2::Context& cx);
  /// @copydoc AnyChannel::PendReadyToWrite
  async2::Poll<Status> PendReadyToWrite(pw::async2::Context& cx);
  /// @copydoc AnyChannel::PendAllocateWriteBuffer
  async2::PollOptional<multibuf::MultiBuf> PendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes);
  /// @copydoc AnyChannel::StageWrite
  Status StageWrite(multibuf::MultiBuf&& data);
  /// @copydoc AnyChannel::PendWrite
  async2::Poll<Status> PendWrite(async2::Context& cx);
  /// @copydoc AnyChannel::PendClose
  async2::Poll<pw::Status> PendClose(async2::Context& cx);

  // Conversions

  /// Channels may be implicitly converted to other compatible channel types.
  template <typename Sibling,
            typename = internal::EnableIfConversionIsValid<Channel, Sibling>>
  constexpr operator Sibling&() {
    return as<Sibling>();
  }
  template <typename Sibling,
            typename = internal::EnableIfConversionIsValid<Channel, Sibling>>
  constexpr operator const Sibling&() const {
    return as<Sibling>();
  }

  /// Returns a reference to this channel as another compatible channel type.
  template <typename Sibling>
  [[nodiscard]] Sibling& as() {
    internal::CheckThatConversionIsValid<Channel, Sibling>();
    return static_cast<Sibling&>(static_cast<AnyChannel&>(*this));
  }
  template <typename Sibling>
  [[nodiscard]] const Sibling& as() const {
    internal::CheckThatConversionIsValid<Channel, Sibling>();
    return static_cast<const Sibling&>(static_cast<const AnyChannel&>(*this));
  }

  /// Returns a reference to this channel as another channel with the specified
  /// properties, which must be compatible.
  template <Property... kOtherChannelProperties>
  [[nodiscard]] auto& as() {
    return as<Channel<data_type(), kOtherChannelProperties...>>();
  }
  template <Property... kOtherChannelProperties>
  [[nodiscard]] const auto& as() const {
    return as<Channel<data_type(), kOtherChannelProperties...>>();
  }

  /// @returns A byte channel reference to a datagram channel
  [[nodiscard]] Channel<DataType::kByte, kProperties...>&
  IgnoreDatagramBoundaries() {
    static_assert(kDataType == DataType::kDatagram,
                  "IgnoreDatagramBoundaries() may only be called to use a "
                  "datagram channel to a byte channel");
    return static_cast<Channel<DataType::kByte, kProperties...>&>(
        static_cast<AnyChannel&>(*this));
  }

  [[nodiscard]] const Channel<DataType::kByte, kProperties...>&
  IgnoreDatagramBoundaries() const {
    static_assert(kDataType == DataType::kDatagram,
                  "IgnoreDatagramBoundaries() may only be called to use a "
                  "datagram channel to a byte channel");
    return static_cast<const Channel<DataType::kByte, kProperties...>&>(
        static_cast<const AnyChannel&>(*this));
  }

 private:
  static_assert(internal::PropertiesAreValid<kProperties...>());

  friend class AnyChannel;

  explicit constexpr Channel() = default;
};

/// A generic data channel that may support reading or writing bytes or
/// datagrams.
///
/// Note that this channel should be used from only one ``pw::async::Task``
/// at a time, as the ``Pend`` methods are only required to remember the
/// latest ``pw::async2::Context`` that was provided. Notably, this means
/// that it is not possible to read from the channel in one task while
/// writing to it from another task: a single task must own and operate
/// the channel. In the future, a wrapper will be offered which will
/// allow the channel to be split into a read half and a write half which
/// can be used from independent tasks.
///
/// To implement a `Channel`, inherit from `ChannelImpl` with the specified
/// properties.
class AnyChannel
    : private Channel<DataType::kByte, kReadable>,
      private Channel<DataType::kByte, kWritable>,
      private Channel<DataType::kByte, kReadable, kWritable>,
      private Channel<DataType::kByte, kReliable, kReadable>,
      private Channel<DataType::kByte, kReliable, kWritable>,
      private Channel<DataType::kByte, kReliable, kReadable, kWritable>,
      private Channel<DataType::kDatagram, kReadable>,
      private Channel<DataType::kDatagram, kWritable>,
      private Channel<DataType::kDatagram, kReadable, kWritable>,
      private Channel<DataType::kDatagram, kReliable, kReadable>,
      private Channel<DataType::kDatagram, kReliable, kWritable>,
      private Channel<DataType::kDatagram, kReliable, kReadable, kWritable> {
 public:
  virtual ~AnyChannel() = default;

  // Returned by Position() if getting the position is not supported.
  // TODO: b/323622630 - `Seek` and `Position` are not yet implemented.
  // static constexpr size_t kUnknownPosition =
  //     std::numeric_limits<size_t>::max();

  // Channel properties

  /// Returns the data type of the channel implementation.
  [[nodiscard]] constexpr DataType data_type() const { return data_type_; }

  /// Returns whether the channel implementation is reliable.
  [[nodiscard]] constexpr bool reliable() const {
    return (properties_ & Property::kReliable) != 0;
  }

  /// Returns whether the channel implementation is seekable.
  [[nodiscard]] constexpr bool seekable() const {
    return (properties_ & Property::kSeekable) != 0;
  }

  /// Returns whether the channel implementation is readable.
  [[nodiscard]] constexpr bool readable() const {
    return (properties_ & Property::kReadable) != 0;
  }

  /// Returns whether the channel implementation is writable.
  [[nodiscard]] constexpr bool writable() const {
    return (properties_ & Property::kWritable) != 0;
  }

  /// True if the channel is open for reading. Always false for write-only
  /// channels.
  [[nodiscard]] constexpr bool is_read_open() const { return read_open_; }

  /// True if the channel is open for writing. Always false for read-only
  /// channels.
  [[nodiscard]] constexpr bool is_write_open() const { return write_open_; }

  /// True if the channel is open for either reading or writing.
  [[nodiscard]] constexpr bool is_read_or_write_open() const {
    return read_open_ || write_open_;
  }

  // Read API

  /// Returns a `pw::multibuf::MultiBuf` with read data, if available. If data
  /// is not available, invokes `cx.waker()` when it becomes available.
  ///
  /// For datagram channels, each successful read yields one complete
  /// datagram, which may contain zero or more bytes. For byte stream channels,
  /// each successful read yields one or more bytes.
  ///
  /// Channels only support one read operation / waker at a time.
  ///
  /// @returns
  /// * @OK: Data was read into a `MultiBuf`.
  /// * @UNIMPLEMENTED: The channel does not support reading.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @OUT_OF_RANGE: The end of the stream was reached. This may be thought of
  ///   as reaching the end of a file. Future reads may succeed after seeking
  ///   backwards, but no more new data will be produced. The channel is still
  ///   open; writes and seeks may succeed.
  async2::PollResult<multibuf::MultiBuf> PendRead(async2::Context& cx) {
    if (!is_read_open()) {
      return Status::FailedPrecondition();
    }
    async2::PollResult<multibuf::MultiBuf> result = DoPendRead(cx);
    if (result.IsReady() && result->status().IsFailedPrecondition()) {
      set_read_closed();
    }
    return result;
  }

  // Write API

  /// Checks whether a writeable channel is *currently* writeable.
  ///
  /// This should be called before attempting to ``Write``, and may be called
  /// before allocating a write buffer if trying to reduce memory pressure.
  ///
  /// This method will return:
  ///
  /// * Ready(OK) - The channel is currently writeable, and a single caller
  ///   may proceed to ``Write``.
  /// * Ready(UNIMPLEMENTED) - The channel does not support writing.
  /// * Ready(FAILED_PRECONDITION) - The channel is closed for writing.
  /// * Pending - ``cx`` will be awoken when the channel becomes writeable
  ///   again.
  ///
  /// Note: this method will always return ``Ready`` for non-writeable
  /// channels.
  async2::Poll<Status> PendReadyToWrite(pw::async2::Context& cx) {
    if (!is_write_open()) {
      return Status::FailedPrecondition();
    }
    async2::Poll<Status> result = DoPendReadyToWrite(cx);
    if (result.IsReady() && result->IsFailedPrecondition()) {
      set_write_closed();
    }
    return result;
  }

  /// Attempts to allocate a write buffer of at least `min_bytes` bytes.
  ///
  /// On success, returns a `MultiBuf` of at least `min_bytes`.
  /// This buffer should be filled with data and then passed back into
  /// `StageWrite`. The user may shrink or fragment the `MultiBuf` during
  /// its own usage of the buffer, but the `MultiBuf` should be restored
  /// to its original shape before it is passed to `StageWrite`.
  ///
  /// Users should not wait on the result of `PendAllocateWriteBuffer` while
  /// holding an existing `MultiBuf` from `PendAllocateWriteBuffer`, as this
  /// can result in deadlocks.
  ///
  /// This method will return:
  ///
  /// * Ready(buffer) - A buffer of the requested size is provided.
  /// * Ready(std::nullopt) - `min_bytes` is larger than the maximum buffer
  ///   size this channel can allocate.
  /// * Pending - No buffer of at least `min_bytes` is available. The task
  ///   associated with the provided `pw::async2::Context` will be awoken
  ///   when a sufficiently-sized buffer becomes available.
  async2::PollOptional<multibuf::MultiBuf> PendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) {
    return DoPendAllocateWriteBuffer(cx, min_bytes);
  }

  /// Writes using a previously allocated MultiBuf. Returns a token that
  /// refers to this write. These tokens are monotonically increasing, and
  /// PendWrite() returns the value of the latest token it has flushed.
  ///
  /// The ``MultiBuf`` argument to ``Write`` may consist of either:
  ///   (1) A single ``MultiBuf`` allocated by ``PendAllocateWriteBuffer``
  ///       that has not been combined with any other ``MultiBuf`` s
  ///       or ``Chunk``s OR
  ///   (2) A ``MultiBuf`` containing any combination of buffers from sources
  ///       other than ``PendAllocateWriteBuffer``.
  ///
  /// This requirement allows for more efficient use of memory in case (1).
  /// For example, a ring-buffer implementation of a ``Channel`` may
  /// specialize ``PendAllocateWriteBuffer`` to return the next section of the
  /// buffer available for writing.
  ///
  /// @returns May fail with the following error codes:
  /// * @OK: Data was accepted by the channel.
  /// * @UNIMPLEMENTED: The channel does not support writing.
  /// * @UNAVAILABLE: The write failed due to a transient error (only applies
  ///    to unreliable channels).
  /// * @FAILED_PRECONDITION: The channel is closed.
  Status StageWrite(multibuf::MultiBuf&& data) {
    if (!is_write_open()) {
      return Status::FailedPrecondition();
    }
    Status status = DoStageWrite(std::move(data));
    if (status.IsFailedPrecondition()) {
      set_write_closed();
    }
    return status;
  }

  /// Completes pending writes.
  ///
  /// Returns a ``async2::Poll`` indicating whether or the write has completed.
  ///
  /// * Ready(OK) - All data has been successfully written.
  /// * Ready(UNIMPLEMENTED) - The channel does not support writing.
  /// * Ready(FAILED_PRECONDITION) - The channel is closed.
  /// * Pending - Writing is not complete.
  async2::Poll<Status> PendWrite(async2::Context& cx) {
    if (!is_write_open()) {
      return Status::FailedPrecondition();
    }
    async2::Poll<Status> status = DoPendWrite(cx);
    if (status.IsReady() && status->IsFailedPrecondition()) {
      set_write_closed();
    }
    return status;
  }

  /// Seek changes the position in the stream.
  ///
  /// TODO: b/323622630 - `Seek` and `Position` are not yet implemented.
  ///
  /// Any ``PendRead`` or ``Write`` calls following a call to ``Seek`` will be
  /// relative to the new position. Already-written data still being flushed
  /// will be output relative to the old position.
  ///
  /// @returns
  /// * @OK: The current position was successfully changed.
  /// * @UNIMPLEMENTED: The channel does not support seeking.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @NOT_FOUND: The seek was to a valid position, but the channel is no
  ///   longer capable of seeking to this position (partially seekable
  ///   channels only).
  /// * @OUT_OF_RANGE: The seek went beyond the end of the stream.
  Status Seek(async2::Context& cx, ptrdiff_t position, Whence whence);

  /// Returns the current position in the stream, or `kUnknownPosition` if
  /// unsupported.
  ///
  /// TODO: b/323622630 - `Seek` and `Position` are not yet implemented.
  size_t Position() const;

  /// Closes the channel, flushing any data.
  ///
  /// @returns
  /// * @OK: The channel was closed and all data was sent successfully.
  /// * @DATA_LOSS: The channel was closed, but not all previously written data
  ///   was delivered.
  /// * @FAILED_PRECONDITION: Channel was already closed, which can happen
  ///   out-of-band due to errors.
  async2::Poll<pw::Status> PendClose(async2::Context& cx) {
    if (!is_read_or_write_open()) {
      return Status::FailedPrecondition();
    }
    auto result = DoPendClose(cx);
    if (result.IsReady()) {
      set_read_closed();
      set_write_closed();
    }
    return result;
  }

 protected:
  // Marks the channel as closed for reading, but does nothing else.
  //
  // PendClose() always marks the channel closed when DoPendClose() returns
  // Ready(), regardless of the status.
  void set_read_closed() { read_open_ = false; }

  // Marks the channel as closed for writing, but does nothing else.
  //
  // PendClose() always marks the channel closed when DoPendClose() returns
  // Ready(), regardless of the status.
  void set_write_closed() { write_open_ = false; }

 private:
  template <DataType, Property...>
  friend class Channel;  // Allow static_casts to AnyChannel

  template <DataType, Property...>
  friend class internal::BaseChannelImpl;  // Allow inheritance

  // `AnyChannel` may only be constructed by deriving from `ChannelImpl`.
  explicit constexpr AnyChannel(DataType type, uint8_t properties)
      : data_type_(type),
        properties_(properties),
        read_open_(readable()),
        write_open_(writable()) {}

  // Virtual interface

  // Read functions

  virtual async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) = 0;

  // Write functions

  virtual async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) = 0;

  virtual pw::async2::Poll<Status> DoPendReadyToWrite(async2::Context& cx) = 0;

  virtual Status DoStageWrite(multibuf::MultiBuf&& buffer) = 0;

  virtual pw::async2::Poll<Status> DoPendWrite(async2::Context& cx) = 0;

  // Seek functions
  /// TODO: b/323622630 - `Seek` and `Position` are not yet implemented.

  // virtual Status DoSeek(ptrdiff_t position, Whence whence) = 0;

  // virtual size_t DoPosition() const = 0;

  // Common functions
  virtual async2::Poll<Status> DoPendClose(async2::Context& cx) = 0;

  DataType data_type_;
  uint8_t properties_;
  bool read_open_;
  bool write_open_;
};

/// A `ByteChannel` exchanges data as a stream of bytes.
template <Property... kProperties>
using ByteChannel = Channel<DataType::kByte, kProperties...>;

/// A `DatagramChannel` exchanges data as a series of datagrams.
template <Property... kProperties>
using DatagramChannel = Channel<DataType::kDatagram, kProperties...>;

/// Unreliable byte-oriented `Channel` that supports reading.
using ByteReader = ByteChannel<kReadable>;
/// Unreliable byte-oriented `Channel` that supports writing.
using ByteWriter = ByteChannel<kWritable>;
/// Unreliable byte-oriented `Channel` that supports reading and writing.
using ByteReaderWriter = ByteChannel<kReadable, kWritable>;

/// Reliable byte-oriented `Channel` that supports reading.
using ReliableByteReader = ByteChannel<kReliable, kReadable>;
/// Reliable byte-oriented `Channel` that supports writing.
using ReliableByteWriter = ByteChannel<kReliable, kWritable>;
/// Reliable byte-oriented `Channel` that supports reading and writing.
using ReliableByteReaderWriter = ByteChannel<kReliable, kReadable, kWritable>;

/// Unreliable datagram-oriented `Channel` that supports reading.
using DatagramReader = DatagramChannel<kReadable>;
/// Unreliable datagram-oriented `Channel` that supports writing.
using DatagramWriter = DatagramChannel<kWritable>;
/// Unreliable datagram-oriented `Channel` that supports reading and writing.
using DatagramReaderWriter = DatagramChannel<kReadable, kWritable>;

/// Reliable datagram-oriented `Channel` that supports reading.
using ReliableDatagramReader = DatagramChannel<kReliable, kReadable>;
/// Reliable datagram-oriented `Channel` that supports writing.
using ReliableDatagramWriter = DatagramChannel<kReliable, kWritable>;
/// Reliable datagram-oriented `Channel` that supports reading and writing.
using ReliableDatagramReaderWriter =
    DatagramChannel<kReliable, kReadable, kWritable>;

// Implementation extension classes.

/// Extend `ChannelImpl` to implement a channel with the specified properties.
/// Unavailable methods on the channel will be stubbed out.
///
/// Alternately, inherit from `pw::channel::Implement` with a channel
/// reader/writer alias as the template parameter.
///
/// A `ChannelImpl` has a corresponding `Channel` type
/// (`ChannelImpl<>::Channel`). Call the `channel()` method to convert the
/// `ChannelImpl` to its corresponding `Channel`.
template <DataType kDataType, Property... kProperties>
class ChannelImpl {
  static_assert(internal::PropertiesAreValid<kProperties...>());
};

/// Implement a byte-oriented `Channel` with the specified properties.
template <Property... kProperties>
using ByteChannelImpl = ChannelImpl<DataType::kByte, kProperties...>;

/// Implement a datagram-oriented `Channel` with the specified properties.
template <Property... kProperties>
using DatagramChannelImpl = ChannelImpl<DataType::kDatagram, kProperties...>;

/// Implement the specified `Channel` type. This is intended for use with the
/// reader/writer aliases:
///
/// @code{.cpp}
/// class MyChannel : public pw::channel::Implement<pw::channel::ByteReader> {};
/// @endcode
template <typename ChannelType>
class Implement;

/// @}

template <DataType kDataType, Property... kProperties>
class Implement<Channel<kDataType, kProperties...>>
    : public ChannelImpl<kDataType, kProperties...> {
 protected:
  explicit constexpr Implement() = default;
};

}  // namespace pw::channel

// Include specializations for supported Channel types.
#include "pw_channel/internal/channel_specializations.h"
