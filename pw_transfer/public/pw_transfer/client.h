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

#include "pw_function/function.h"
#include "pw_result/result.h"
#include "pw_rpc/client.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/transfer.raw_rpc.pb.h"
#include "pw_transfer/transfer_thread.h"

namespace pw::transfer {

/// @module{pw_transfer}

/// RPC client for running `pw_transfer` read and write data transfers.
///
/// Currently, a `Client` is capable of running transfers over a single RPC
/// channel, processed on a shared `TransferThread`.
class Client {
 public:
  /// A handle to an active transfer. Used to manage the transfer during its
  /// operation.
  class Handle {
   public:
    /// Creates an unassigned `Handle` not associated with any active transfer.
    constexpr Handle() : client_(nullptr), id_(kUnassignedHandleId) {}

    /// Terminates the ongoing transfer.
    void Cancel() {
      if (client_ != nullptr) {
        client_->CancelTransfer(*this);
      }
    }

    /// In a `Write()` transfer, updates the total size of the resource being
    /// transferred. This size will be indicated to the server.
    ///
    /// @param[in] size_bytes Total size of the transfer resource in bytes.
    void SetTransferSize(size_t size_bytes) {
      if (client_ != nullptr) {
        client_->UpdateTransferSize(*this, size_bytes);
      }
    }

   private:
    friend class Client;

    static constexpr uint32_t kUnassignedHandleId = 0;

    explicit constexpr Handle(Client* client, uint32_t id)
        : client_(client), id_(id) {}
    constexpr uint32_t id() const { return id_; }
    constexpr bool is_unassigned() const { return id_ == kUnassignedHandleId; }

    Client* client_;
    uint32_t id_;
  };

  /// Callback invoked from the transfer thread when a transfer completes,
  /// receiving the overall status of the transfer.
  using CompletionFunc = Function<void(Status)>;

  /// Initializes a transfer client on a specified RPC client and channel.
  ///
  /// Transfer tasks are processed on the provided `transfer_thread`, which may
  /// be shared between a transfer client and service.
  ///
  /// @param[in] rpc_client RPC client over which transfer streams are opened.
  /// @param[in] channel_id RPC channel ID on which transfers are run.
  /// @param[in] transfer_thread Thread used to process transfer events.
  /// @param[in] max_window_size_bytes Maximum amount of data in bytes to
  /// request at a time during a read transfer, unless told a more restrictive
  /// amount by the transfer's stream. This size should span multiple chunks and
  /// can be set quite large; the transfer protocol automatically adjusts its
  /// window size as a transfer progresses.
  /// @param[in] extend_window_divisor Fractional position within a window at
  /// which a receive transfer extends its window size.
  Client(rpc::Client& rpc_client,
         uint32_t channel_id,
         TransferThread& transfer_thread,
         size_t max_window_size_bytes,
         uint32_t extend_window_divisor = cfg::kDefaultExtendWindowDivisor)
      : default_protocol_version(ProtocolVersion::kLatest),
        client_(rpc_client, channel_id),
        transfer_thread_(transfer_thread),
        next_handle_id_(1),
        max_parameters_(static_cast<uint32_t>(max_window_size_bytes),
                        static_cast<uint32_t>(transfer_thread.max_chunk_size()),
                        extend_window_divisor),
        max_retries_(cfg::kDefaultMaxClientRetries),
        max_lifetime_retries_(cfg::kDefaultMaxLifetimeRetries),
        has_read_stream_(false),
        has_write_stream_(false) {}

  /// @deprecated Explicitly provide `max_window_size_bytes` instead.
  ///
  /// @param[in] rpc_client RPC client over which transfer streams are opened.
  /// @param[in] channel_id RPC channel ID on which transfers are run.
  /// @param[in] transfer_thread Thread used to process transfer events.
  [[deprecated("Explicitly provide a maximum window size")]]
  Client(rpc::Client& rpc_client,
         uint32_t channel_id,
         TransferThread& transfer_thread)
      : Client(rpc_client,
               channel_id,
               transfer_thread,
               transfer_thread.max_chunk_size()) {}

  Client(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(const Client&) = delete;
  Client& operator=(Client&&) = delete;

  /// Begins a new read transfer for the given `resource_id` using an explicit
  /// `protocol_version`.
  ///
  /// Data read from the server is written to `output`. Due to the asynchronous
  /// nature of transfer operations, this function only returns an error status
  /// synchronously if called with invalid arguments; transfer errors during
  /// execution are reported through `on_completion` on the transfer thread.
  ///
  /// When `initial_offset` is non-zero, `output` is used as-is without seeking;
  /// the caller must position `output` beforehand if needed.
  ///
  /// @param[in] resource_id ID of the server resource to read.
  /// @param[in,out] output Stream to which received data is written.
  /// @param[in] on_completion Callback invoked from the transfer thread with
  /// the overall transfer status when the transfer finishes.
  /// @param[in] protocol_version Protocol version to request for this transfer.
  /// @param[in] timeout Maximum time to wait for a chunk before retrying.
  /// @param[in] initial_chunk_timeout Maximum time to wait for the server's
  /// initial response chunk before retrying.
  /// @param[in] initial_offset Starting byte offset within the server resource.
  ///
  /// @returns @Result{a `Handle` to manage the active transfer}
  /// * @INVALID_ARGUMENT: `on_completion` is null, `protocol_version` is
  ///   `ProtocolVersion::kUnknown`, or `initial_offset` is non-zero with a
  ///   protocol version older than `ProtocolVersion::kVersionTwo`.
  Result<Handle> Read(
      uint32_t resource_id,
      stream::Writer& output,
      CompletionFunc&& on_completion,
      ProtocolVersion protocol_version,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u);

  /// Begins a new read transfer for the given `resource_id` using the client's
  /// configured default protocol version.
  ///
  /// Data read from the server is written to `output`. Due to the asynchronous
  /// nature of transfer operations, this function only returns an error status
  /// synchronously if called with invalid arguments; transfer errors during
  /// execution are reported through `on_completion` on the transfer thread.
  ///
  /// @param[in] resource_id ID of the server resource to read.
  /// @param[in,out] output Stream to which received data is written.
  /// @param[in] on_completion Callback invoked from the transfer thread with
  /// the overall transfer status when the transfer finishes.
  /// @param[in] timeout Maximum time to wait for a chunk before retrying.
  /// @param[in] initial_chunk_timeout Maximum time to wait for the server's
  /// initial response chunk before retrying.
  /// @param[in] initial_offset Starting byte offset within the server resource.
  ///
  /// @returns @Result{a `Handle` to manage the active transfer}
  /// * @INVALID_ARGUMENT: `on_completion` is null, the client's default
  ///   protocol version is `ProtocolVersion::kUnknown`, or `initial_offset` is
  ///   non-zero with a protocol version older than
  ///   `ProtocolVersion::kVersionTwo`.
  Result<Handle> Read(
      uint32_t resource_id,
      stream::Writer& output,
      CompletionFunc&& on_completion,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u) {
    return Read(resource_id,
                output,
                std::move(on_completion),
                default_protocol_version,
                timeout,
                initial_chunk_timeout,
                initial_offset);
  }

  /// Begins a new write transfer for the given `resource_id` using an explicit
  /// `protocol_version`.
  ///
  /// Data from `input` is sent to the server. Due to the asynchronous nature of
  /// transfer operations, this function only returns an error status
  /// synchronously if called with invalid arguments; transfer errors during
  /// execution are reported through `on_completion` on the transfer thread.
  ///
  /// When `initial_offset` is non-zero, `input` is used as-is without seeking;
  /// the caller must position `input` beforehand if needed.
  ///
  /// @param[in] resource_id ID of the server resource to write.
  /// @param[in,out] input Stream from which data to transmit is read.
  /// @param[in] on_completion Callback invoked from the transfer thread with
  /// the overall transfer status when the transfer finishes.
  /// @param[in] protocol_version Protocol version to request for this transfer.
  /// @param[in] timeout Maximum time to wait for a chunk before retrying.
  /// @param[in] initial_chunk_timeout Maximum time to wait for the server's
  /// initial response chunk before retrying.
  /// @param[in] initial_offset Starting byte offset within the server resource.
  ///
  /// @returns @Result{a `Handle` to manage the active transfer}
  /// * @INVALID_ARGUMENT: `on_completion` is null, `protocol_version` is
  ///   `ProtocolVersion::kUnknown`, or `initial_offset` is non-zero with a
  ///   protocol version older than `ProtocolVersion::kVersionTwo`.
  Result<Handle> Write(
      uint32_t resource_id,
      stream::Reader& input,
      CompletionFunc&& on_completion,
      ProtocolVersion protocol_version,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u);

  /// Begins a new write transfer for the given `resource_id` using the client's
  /// configured default protocol version.
  ///
  /// Data from `input` is sent to the server. Due to the asynchronous nature of
  /// transfer operations, this function only returns an error status
  /// synchronously if called with invalid arguments; transfer errors during
  /// execution are reported through `on_completion` on the transfer thread.
  ///
  /// @param[in] resource_id ID of the server resource to write.
  /// @param[in,out] input Stream from which data to transmit is read.
  /// @param[in] on_completion Callback invoked from the transfer thread with
  /// the overall transfer status when the transfer finishes.
  /// @param[in] timeout Maximum time to wait for a chunk before retrying.
  /// @param[in] initial_chunk_timeout Maximum time to wait for the server's
  /// initial response chunk before retrying.
  /// @param[in] initial_offset Starting byte offset within the server resource.
  ///
  /// @returns @Result{a `Handle` to manage the active transfer}
  /// * @INVALID_ARGUMENT: `on_completion` is null, the client's default
  ///   protocol version is `ProtocolVersion::kUnknown`, or `initial_offset` is
  ///   non-zero with a protocol version older than
  ///   `ProtocolVersion::kVersionTwo`.
  Result<Handle> Write(
      uint32_t resource_id,
      stream::Reader& input,
      CompletionFunc&& on_completion,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u) {
    return Write(resource_id,
                 input,
                 std::move(on_completion),
                 default_protocol_version,
                 timeout,
                 initial_chunk_timeout,
                 initial_offset);
  }

  /// Sets the window divisor at which receive transfers extend their window.
  ///
  /// @param[in] extend_window_divisor Fractional divisor (must be greater than
  /// `1`).
  ///
  /// @returns
  /// * @OK: The divisor was updated.
  /// * @INVALID_ARGUMENT: `extend_window_divisor` is `1` or `0`.
  Status set_extend_window_divisor(uint32_t extend_window_divisor) {
    if (extend_window_divisor <= 1) {
      return Status::InvalidArgument();
    }

    max_parameters_.set_extend_window_divisor(extend_window_divisor);
    return OkStatus();
  }

  /// Sets the maximum number of retries for an individual chunk on new
  /// transfers.
  ///
  /// @param[in] max_retries Retry count (must be between `1` and the smaller of
  /// `max_lifetime_retries` and `255`).
  ///
  /// @returns
  /// * @OK: `max_retries` was updated.
  /// * @INVALID_ARGUMENT: `max_retries` is `0`, exceeds `max_lifetime_retries`,
  ///   or exceeds `std::numeric_limits<uint8_t>::max()`.
  constexpr Status set_max_retries(uint32_t max_retries) {
    if (max_retries < 1 || max_retries > max_lifetime_retries_ ||
        max_retries > std::numeric_limits<uint8_t>::max()) {
      return Status::InvalidArgument();
    }
    max_retries_ = max_retries;
    return OkStatus();
  }

  /// Sets the maximum total retries allowed over the lifetime of a transfer.
  ///
  /// @param[in] max_lifetime_retries Maximum lifetime retries (must be greater
  /// than or equal to `max_retries`).
  ///
  /// @returns
  /// * @OK: `max_lifetime_retries` was updated.
  /// * @INVALID_ARGUMENT: `max_lifetime_retries` is less than `max_retries`.
  constexpr Status set_max_lifetime_retries(uint32_t max_lifetime_retries) {
    if (max_lifetime_retries < max_retries_) {
      return Status::InvalidArgument();
    }
    max_lifetime_retries_ = max_lifetime_retries;
    return OkStatus();
  }

  /// Sets the default protocol version requested when starting new transfers.
  ///
  /// @param[in] new_version Protocol version to request by default.
  constexpr void set_protocol_version(ProtocolVersion new_version) {
    default_protocol_version = new_version;
  }

  /// Checks whether the client currently has an open RPC `Read()` stream.
  ///
  /// @returns `true` if a client read stream is open, `false` otherwise.
  bool has_read_stream() const { return has_read_stream_; }

  /// Checks whether the client currently has an open RPC `Write()` stream.
  ///
  /// @returns `true` if a client write stream is open, `false` otherwise.
  bool has_write_stream() const { return has_write_stream_; }

 private:
  friend class internal::TransferThread;

  // Terminates an ongoing transfer.
  void CancelTransfer(Handle handle) {
    if (!handle.is_unassigned()) {
      transfer_thread_.CancelClientTransfer(handle.id());
    }
  }

  void UpdateTransferSize(Handle handle, size_t transfer_size_bytes) {
    if (!handle.is_unassigned()) {
      transfer_thread_.UpdateClientTransfer(handle.id(), transfer_size_bytes);
    }
  }

  ProtocolVersion default_protocol_version;

  using Transfer = pw_rpc::raw::Transfer;

  void OnRpcError(Status status, internal::TransferType type);

  void OpenReadStream(internal::SetStreamBehavior behavior);
  void OpenWriteStream(internal::SetStreamBehavior behavior);

  Handle AssignHandle();

  Transfer::Client client_;
  TransferThread& transfer_thread_;

  uint32_t next_handle_id_;

  internal::TransferParameters max_parameters_;
  uint32_t max_retries_;
  uint32_t max_lifetime_retries_;

  bool has_read_stream_;
  bool has_write_stream_;
};

/// @endmodule

}  // namespace pw::transfer
