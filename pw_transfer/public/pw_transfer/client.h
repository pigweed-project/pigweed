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

class Client {
 public:
  /// A handle to an active transfer. Used to manage the transfer during its
  /// operation.
  class Handle {
   public:
    constexpr Handle() : client_(nullptr), id_(kUnassignedHandleId) {}

    /// Terminates the transfer.
    void Cancel() {
      if (client_ != nullptr) {
        client_->CancelTransfer(*this);
      }
    }

    /// In a `Write()` transfer, updates the size of the resource being
    /// transferred. This size will be indicated to the server.
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

  using CompletionFunc = Function<void(Status)>;

  // Initializes a transfer client on a specified RPC client and channel.
  // Transfer tasks are processed on the provided transfer thread, which may be
  // shared between a transfer client and service.
  //
  // `max_window_size_bytes` is the maximum amount of data to ask for at a
  // time during a read transfer, unless told a more restrictive amount by the
  // transfer's stream. This size should span multiple chunks, and can be set
  // quite large. The transfer protocol automatically adjusts its window size
  // as a transfer progresses to attempt to find an optimal configuration for
  // the connection over which it is running.
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

  // Begins a new read transfer for the given resource ID. The data read from
  // the server is written to the provided writer. Returns OK if the transfer is
  // successfully started. When the transfer finishes (successfully or not), the
  // completion callback is invoked with the overall status.
  Result<Handle> Read(
      uint32_t resource_id,
      stream::Writer& output,
      CompletionFunc&& on_completion,
      ProtocolVersion protocol_version,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u);

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

  // Begins a new write transfer for the given resource ID. Data from the
  // provided reader is sent to the server. When the transfer finishes
  // (successfully or not), the completion callback is invoked with the overall
  // status.
  Result<Handle> Write(
      uint32_t resource_id,
      stream::Reader& input,
      CompletionFunc&& on_completion,
      ProtocolVersion protocol_version,
      chrono::SystemClock::duration timeout = cfg::kDefaultClientTimeout,
      chrono::SystemClock::duration initial_chunk_timeout =
          cfg::kDefaultInitialChunkTimeout,
      uint32_t initial_offset = 0u);

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

  Status set_extend_window_divisor(uint32_t extend_window_divisor) {
    if (extend_window_divisor <= 1) {
      return Status::InvalidArgument();
    }

    max_parameters_.set_extend_window_divisor(extend_window_divisor);
    return OkStatus();
  }

  constexpr Status set_max_retries(uint32_t max_retries) {
    if (max_retries < 1 || max_retries > max_lifetime_retries_ ||
        max_retries > std::numeric_limits<uint8_t>::max()) {
      return Status::InvalidArgument();
    }
    max_retries_ = max_retries;
    return OkStatus();
  }

  constexpr Status set_max_lifetime_retries(uint32_t max_lifetime_retries) {
    if (max_lifetime_retries < max_retries_) {
      return Status::InvalidArgument();
    }
    max_lifetime_retries_ = max_lifetime_retries;
    return OkStatus();
  }

  constexpr void set_protocol_version(ProtocolVersion new_version) {
    default_protocol_version = new_version;
  }

  bool has_read_stream() const { return has_read_stream_; }

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

}  // namespace pw::transfer
