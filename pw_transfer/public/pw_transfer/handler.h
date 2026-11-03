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

#include <limits>

#include "pw_assert/assert.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_transfer/internal/event.h"

namespace pw::transfer {
namespace internal {

class Context;

}  // namespace internal

/// @module{pw_transfer}

/// Base class for transfer handlers.
///
/// Transfer handlers connect a transfer resource ID to a data stream, wrapped
/// with initialization and cleanup procedures. Handlers use a
/// `pw::stream::Reader` or `pw::stream::Writer` to perform reads and writes,
/// and provide optional `Prepare` and `Finalize` methods invoked from the
/// transfer thread.
class Handler : public IntrusiveForwardList<Handler>::Item {
 public:
  virtual ~Handler() = default;

  /// Returns the unique identifier for the resource managed by this handler.
  ///
  /// @returns The resource ID.
  constexpr uint32_t id() const { return resource_id_; }

  /// Called on the transfer thread at the beginning of a read transfer. The
  /// `pw::stream::Reader` must be set and ready to read after a successful
  /// `PrepareRead()` call. Returning a non-OK status aborts the read.
  ///
  /// @returns
  /// * @OK: The reader is ready for the transfer.
  /// * @UNIMPLEMENTED: Reads are not supported by this handler.
  /// * Any other error status to abort the read transfer.
  virtual Status PrepareRead() = 0;

  /// Called on the transfer thread at the beginning of a non-zero-offset read
  /// transfer.
  ///
  /// Implementations should verify that `offset` is a valid read point for the
  /// resource and position the `pw::stream::Reader` to read from `offset`
  /// before returning `@OK`. Returning a non-OK status aborts the read.
  ///
  /// @param[in] offset Starting byte offset for the read transfer.
  ///
  /// @returns
  /// * @OK: The reader is positioned at `offset` and ready for the transfer.
  /// * @UNIMPLEMENTED: Non-zero-offset reads are not supported.
  /// * @RESOURCE_EXHAUSTED: `offset` is past the end of the stream.
  /// * Any other error status to abort the read transfer.
  virtual Status PrepareRead([[maybe_unused]] uint32_t offset) {
    return Status::Unimplemented();
  }

  /// Called on the transfer thread at the end of a read transfer.
  ///
  /// @param[in] status Overall status indicating whether the data transfer
  /// succeeded.
  virtual void FinalizeRead([[maybe_unused]] Status status) {}

  /// Called on the transfer thread at the beginning of a write transfer. The
  /// `pw::stream::Writer` must be set and ready to write after a successful
  /// `PrepareWrite()` call. Returning a non-OK status aborts the write.
  ///
  /// @returns
  /// * @OK: The writer is ready for the transfer.
  /// * @UNIMPLEMENTED: Writes are not supported by this handler.
  /// * Any other error status to abort the write transfer.
  virtual Status PrepareWrite() = 0;

  /// Called on the transfer thread at the beginning of a non-zero-offset write
  /// transfer.
  ///
  /// Implementations should verify that `offset` is a valid write point for the
  /// resource and prepare the `pw::stream::Writer` to write at `offset` before
  /// returning `@OK`. Returning a non-OK status aborts the write.
  ///
  /// @param[in] offset Starting byte offset for the write transfer.
  ///
  /// @returns
  /// * @OK: The writer is positioned at `offset` and ready for the transfer.
  /// * @UNIMPLEMENTED: Non-zero-offset writes are not supported.
  /// * @RESOURCE_EXHAUSTED: `offset` is past the end of the resource.
  /// * Any other error status to abort the write transfer.
  virtual Status PrepareWrite([[maybe_unused]] uint32_t offset) {
    return Status::Unimplemented();
  }

  /// Called on the transfer thread at the end of a write transfer.
  ///
  /// Returning an error signals that the transfer failed, even if data transfer
  /// had succeeded up to this point.
  ///
  /// @param[in] status Status indicating whether the data transfer succeeded up
  /// to finalization.
  ///
  /// @returns
  /// * @OK: Finalization succeeded.
  /// * Any error status to fail the transfer.
  virtual Status FinalizeWrite([[maybe_unused]] Status status) {
    return OkStatus();
  }

  /// Returns the total size of the transfer resource in bytes, if known.
  ///
  /// @returns The resource size in bytes, or
  /// `std::numeric_limits<size_t>::max()` if unknown.
  virtual size_t ResourceSize() const {
    return std::numeric_limits<size_t>::max();
  }

  /// Queries application-defined status metadata for this resource in response
  /// to a `Transfer.GetResourceStatus` RPC.
  ///
  /// @param[out] readable_offset Readable byte offset of the resource.
  /// @param[out] writeable_offset Writeable byte offset of the resource.
  /// @param[out] read_checksum Checksum of readable data.
  /// @param[out] write_checksum Checksum of written data.
  ///
  /// @returns
  /// * @OK: The status fields were populated by the handler.
  /// * @UNIMPLEMENTED: Resource status reporting is not supported by this
  ///   handler.
  /// * Any other error status to report to the caller.
  virtual Status GetStatus(uint64_t& readable_offset,
                           uint64_t& writeable_offset,
                           uint64_t& read_checksum,
                           uint64_t& write_checksum) {
    readable_offset = 0;
    writeable_offset = 0;
    read_checksum = 0;
    write_checksum = 0;
    return Status::Unimplemented();
  }

 protected:
  /// Constructs a `Handler` with an optional initial `pw::stream::Reader`.
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  /// @param[in] reader Optional reader stream to associate with this handler.
  constexpr Handler(uint32_t resource_id, stream::Reader* reader)
      : resource_id_(resource_id), reader_(reader) {}

  /// Constructs a `Handler` with an optional initial `pw::stream::Writer`.
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  /// @param[in] writer Optional writer stream to associate with this handler.
  constexpr Handler(uint32_t resource_id, stream::Writer* writer)
      : resource_id_(resource_id), writer_(writer) {}

  /// Sets the `pw::stream::Reader` used by this handler.
  ///
  /// @param[in] reader Stream from which transfer data will be read.
  void set_reader(stream::Reader& reader) { reader_ = &reader; }

  /// Sets the `pw::stream::Writer` used by this handler.
  ///
  /// @param[in] writer Stream to which transfer data will be written.
  void set_writer(stream::Writer& writer) { writer_ = &writer; }

 private:
  friend class internal::Context;

  // Prepares for either a read or write transfer.
  Status Prepare(internal::TransferType type, uint32_t offset = 0) {
    if (offset == 0) {
      return type == internal::TransferType::kTransmit ? PrepareRead()
                                                       : PrepareWrite();
    }

    return type == internal::TransferType::kTransmit ? PrepareRead(offset)
                                                     : PrepareWrite(offset);
  }

  // Only valid after a PrepareRead() or PrepareWrite() call that returns OK.
  stream::Stream& stream() const {
    PW_DASSERT(reader_ != nullptr);
    return *reader_;
  }

  uint32_t resource_id_;

  // Use a union to support constexpr construction.
  union {
    stream::Reader* reader_;
    stream::Writer* writer_;
  };
};

/// A `Handler` that only supports read transfers (`PrepareWrite` returns
/// `@PERMISSION_DENIED`).
class ReadOnlyHandler : public Handler {
 public:
  /// Constructs a `ReadOnlyHandler` whose reader will be set later (for example
  /// in `PrepareRead()`).
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  constexpr ReadOnlyHandler(uint32_t resource_id)
      : Handler(resource_id, static_cast<stream::Reader*>(nullptr)) {}

  /// Constructs a `ReadOnlyHandler` backed by `reader`.
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  /// @param[in] reader Stream from which transfer data will be read.
  constexpr ReadOnlyHandler(uint32_t resource_id, stream::Reader& reader)
      : Handler(resource_id, &reader) {}

  ~ReadOnlyHandler() override = default;

  /// Prepares for a read transfer. Defaults to returning `@OK`.
  ///
  /// @returns
  /// * @OK: The handler is ready for reading.
  Status PrepareRead() override { return OkStatus(); }

  /// Rejects write transfers.
  ///
  /// @returns
  /// * @PERMISSION_DENIED: Writes are not supported.
  Status PrepareWrite() final { return Status::PermissionDenied(); }

  using Handler::set_reader;

 private:
  using Handler::set_writer;
};

/// A `Handler` that only supports write transfers (`PrepareRead` returns
/// `@PERMISSION_DENIED`).
class WriteOnlyHandler : public Handler {
 public:
  /// Constructs a `WriteOnlyHandler` whose writer will be set later (for
  /// example in `PrepareWrite()`).
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  constexpr WriteOnlyHandler(uint32_t resource_id)
      : Handler(resource_id, static_cast<stream::Writer*>(nullptr)) {}

  /// Constructs a `WriteOnlyHandler` backed by `writer`.
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  /// @param[in] writer Stream to which transfer data will be written.
  constexpr WriteOnlyHandler(uint32_t resource_id, stream::Writer& writer)
      : Handler(resource_id, &writer) {}

  ~WriteOnlyHandler() override = default;

  /// Rejects read transfers.
  ///
  /// @returns
  /// * @PERMISSION_DENIED: Reads are not supported.
  Status PrepareRead() final { return Status::PermissionDenied(); }

  /// Prepares for a write transfer. Defaults to returning `@OK`.
  ///
  /// @returns
  /// * @OK: The handler is ready for writing.
  Status PrepareWrite() override { return OkStatus(); }

  using Handler::set_writer;

 private:
  using Handler::set_reader;
};

/// A `Handler` that supports both read and write transfers.
class ReadWriteHandler : public Handler {
 public:
  /// Constructs a `ReadWriteHandler` whose stream will be set later (for
  /// example in `PrepareRead()` or `PrepareWrite()`).
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  constexpr ReadWriteHandler(uint32_t resource_id)
      : Handler(resource_id, static_cast<stream::Reader*>(nullptr)) {}

  /// Constructs a `ReadWriteHandler` backed by `reader_writer`.
  ///
  /// @param[in] resource_id Unique ID of the transferable resource.
  /// @param[in] reader_writer Stream used for both reading and writing.
  ReadWriteHandler(uint32_t resource_id, stream::ReaderWriter& reader_writer)
      : Handler(resource_id, &static_cast<stream::Reader&>(reader_writer)) {}

  ~ReadWriteHandler() override = default;

  /// Prepares for a read transfer. Defaults to returning `@OK`.
  ///
  /// @returns
  /// * @OK: The handler is ready for reading.
  Status PrepareRead() override { return OkStatus(); }

  /// Prepares for a write transfer. Defaults to returning `@OK`.
  ///
  /// @returns
  /// * @OK: The handler is ready for writing.
  Status PrepareWrite() override { return OkStatus(); }

  /// Sets the `pw::stream::ReaderWriter` used by this handler.
  ///
  /// @param[in] reader_writer Stream used for both reading and writing.
  void set_reader_writer(stream::ReaderWriter& reader_writer) {
    set_reader(reader_writer);
  }
};

/// @endmodule

}  // namespace pw::transfer
