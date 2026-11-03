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

#include <cstddef>
#include <cstdint>
#include <limits>

#include "pw_bytes/span.h"
#include "pw_sync/mutex.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/internal/server_context.h"
#include "pw_transfer/transfer.raw_rpc.pb.h"
#include "pw_transfer/transfer_thread.h"

/// Reliable data transfer protocol over pw_rpc
namespace pw::transfer {
namespace internal {

class Chunk;

}  // namespace internal

/// @module{pw_transfer}

/// RPC service for running server-side `pw_transfer` transfers.
///
/// Register `Handler` instances with `RegisterHandler()` for each transferable
/// resource, and register the `TransferService` with a `pw::rpc::Server`.
class TransferService : public pw_rpc::raw::Transfer::Service<TransferService> {
 public:
  /// Callback invoked from the transfer thread when a server transfer starts.
  using StartCallback =
      pw::Function<void(uint32_t session_id, uint32_t resource_id)>;

  /// Callback invoked from the transfer thread when a server transfer ends.
  using EndCallback = pw::Function<void(uint32_t session_id, Status status)>;

  /// Initializes a `TransferService` that can be registered with an RPC server.
  ///
  /// The transfer service runs all transfer tasks on `transfer_thread`, which
  /// may be shared between a `TransferService` and a `Client`.
  ///
  /// @param[in] transfer_thread Thread on which transfer events are processed.
  /// @param[in] max_window_size_bytes Maximum amount of data in bytes to
  /// request at a time during a write transfer, unless further restricted by a
  /// handler's stream. Should span multiple chunks; the protocol automatically
  /// adjusts its window size as a transfer progresses.
  /// @param[in] chunk_timeout Maximum time to wait for a chunk before retrying
  /// or timing out.
  /// @param[in] max_retries Maximum retries for a single chunk before aborting.
  /// @param[in] extend_window_divisor Fractional window position at which a
  /// receive transfer extends its window size.
  /// @param[in] max_lifetime_retries Maximum total retries allowed across the
  /// lifetime of a transfer.
  TransferService(
      TransferThread& transfer_thread,
      uint32_t max_window_size_bytes,
      chrono::SystemClock::duration chunk_timeout = cfg::kDefaultServerTimeout,
      uint8_t max_retries = cfg::kDefaultMaxServerRetries,
      uint32_t extend_window_divisor = cfg::kDefaultExtendWindowDivisor,
      uint32_t max_lifetime_retries = cfg::kDefaultMaxLifetimeRetries)
      : TransferService(transfer_thread,
                        max_window_size_bytes,
                        chunk_timeout,
                        max_retries,
                        extend_window_divisor,
                        max_lifetime_retries,
                        nullptr, /* on_start */
                        nullptr  /* on_end */
        ) {}

  /// Initializes a `TransferService` with server transfer lifecycle callbacks.
  ///
  /// @param[in] transfer_thread Thread on which transfer events are processed.
  /// @param[in] max_window_size_bytes Maximum amount of data in bytes to
  /// request at a time during a write transfer.
  /// @param[in] on_start Callback invoked when a server transfer begins.
  /// @param[in] on_end Callback invoked when a server transfer completes.
  ///
  /// @warning The `on_start` and `on_end` callbacks are invoked from the
  /// transfer thread, so the implementation of the callbacks:
  /// 1. Must not attempt to interact with the transfer thread, as doing so can
  ///    cause deadlock or internal state corruption.
  /// 2. Must return quickly to avoid stalling the transfer thread or the RPC
  ///    thread.
  TransferService(TransferThread& transfer_thread,
                  uint32_t max_window_size_bytes,
                  StartCallback&& on_start,
                  EndCallback&& on_end)
      : TransferService(transfer_thread,
                        max_window_size_bytes,
                        cfg::kDefaultServerTimeout,
                        cfg::kDefaultMaxServerRetries,
                        cfg::kDefaultExtendWindowDivisor,
                        cfg::kDefaultMaxLifetimeRetries,
                        std::move(on_start),
                        std::move(on_end)) {}

  TransferService(const TransferService&) = delete;
  TransferService(TransferService&&) = delete;

  TransferService& operator=(const TransferService&) = delete;
  TransferService& operator=(TransferService&&) = delete;

  /// RPC handler for bidirectional server read transfers.
  ///
  /// @param[in] reader_writer Active raw RPC bidirectional stream.
  void Read(RawServerReaderWriter& reader_writer) {
    thread_.SetServerReadStream(reader_writer, [this](ConstByteSpan message) {
      HandleChunk(message, internal::TransferType::kTransmit);
    });
  }

  /// RPC handler for bidirectional server write transfers.
  ///
  /// @param[in] reader_writer Active raw RPC bidirectional stream.
  void Write(RawServerReaderWriter& reader_writer) {
    thread_.SetServerWriteStream(reader_writer, [this](ConstByteSpan message) {
      HandleChunk(message, internal::TransferType::kReceive);
    });
  }

  /// RPC handler for `GetResourceStatus` unary requests.
  ///
  /// @param[in] request Serialized `ResourceStatusRequest` protobuf payload.
  /// @param[in] responder Raw unary responder used to send the status response.
  void GetResourceStatus(ConstByteSpan request,
                         rpc::RawUnaryResponder& responder);

  /// Registers a `Handler` for a transferable resource with the service.
  ///
  /// Blocks until the registration event is processed by the transfer thread.
  ///
  /// @param[in] handler Resource handler to register. Must remain valid until
  /// unregistered.
  ///
  /// @returns `true` if the handler was registered, `false` if queuing the
  /// event on the transfer thread timed out.
  bool RegisterHandler(Handler& handler) {
    return thread_.AddTransferHandler(handler);
  }

  /// Unregisters a previously registered `Handler` from the service.
  ///
  /// Blocks until the handler is fully cleaned up on the transfer thread.
  ///
  /// @param[in] handler Resource handler to unregister.
  ///
  /// @returns `true` if the handler was unregistered, `false` if queuing the
  /// event on the transfer thread timed out.
  bool UnregisterHandler(Handler& handler) {
    return thread_.RemoveTransferHandler(handler);
  }

  /// @deprecated Use `set_max_window_size_bytes()` instead.
  ///
  /// @param[in] pending_bytes Maximum receive window size in bytes.
  [[deprecated("Use set_max_window_size_bytes instead")]]
  constexpr void set_max_pending_bytes(uint32_t pending_bytes) {
    set_max_window_size_bytes(pending_bytes);
  }

  /// Sets the maximum receive window size in bytes for write transfers.
  ///
  /// @param[in] max_window_size_bytes Maximum receive window size in bytes.
  constexpr void set_max_window_size_bytes(uint32_t max_window_size_bytes) {
    max_parameters_.set_max_window_size_bytes(max_window_size_bytes);
  }

  /// Sets the maximum size for the data payload in a `pw_transfer` chunk.
  ///
  /// Note that the max chunk size must always fit within the transfer thread's
  /// chunk buffer.
  ///
  /// @param[in] max_chunk_size_bytes Maximum chunk payload size in bytes.
  constexpr void set_max_chunk_size_bytes(uint32_t max_chunk_size_bytes) {
    max_parameters_.set_max_chunk_size_bytes(max_chunk_size_bytes);
  }

  /// Sets the chunk timeout for server transfers.
  ///
  /// @param[in] chunk_timeout Duration to wait for a chunk before retrying or
  /// aborting.
  constexpr void set_chunk_timeout(
      chrono::SystemClock::duration chunk_timeout) {
    chunk_timeout_ = chunk_timeout;
  }

  /// Sets the maximum number of retries per chunk on the server.
  ///
  /// @param[in] max_retries Maximum retries per chunk.
  constexpr void set_max_retries(uint8_t max_retries) {
    max_retries_ = max_retries;
  }

  /// Sets the window divisor at which receive transfers extend their window.
  ///
  /// @param[in] extend_window_divisor Fractional divisor (must be greater than
  /// `1`).
  ///
  /// @returns
  /// * @OK: The divisor was updated.
  /// * @INVALID_ARGUMENT: `extend_window_divisor` is `1` or `0`.
  constexpr Status set_extend_window_divisor(uint32_t extend_window_divisor) {
    if (extend_window_divisor <= 1) {
      return Status::InvalidArgument();
    }

    max_parameters_.set_extend_window_divisor(extend_window_divisor);
    return OkStatus();
  }

 private:
  // Initializes a TransferService that can be registered with an RPC server.
  //
  // The transfer service runs all of its transfer tasks on the provided
  // transfer thread. This thread may be shared between a transfer service and
  // a transfer client.
  //
  // `max_window_size_bytes` is the maximum amount of data to ask for at a
  // time during a write transfer, unless told a more restrictive amount by a
  // transfer handler. This size should span multiple chunks, and can be set
  // quite large. The transfer protocol automatically adjusts its window size
  // as a transfer progresses to attempt to find an optimal configuration for
  // the connection over which it is running.
  TransferService(TransferThread& transfer_thread,
                  uint32_t max_window_size_bytes,
                  chrono::SystemClock::duration chunk_timeout,
                  uint8_t max_retries,
                  uint32_t extend_window_divisor,
                  uint32_t max_lifetime_retries,
                  StartCallback&& on_start,
                  EndCallback&& on_end)
      : max_parameters_(max_window_size_bytes,
                        static_cast<uint32_t>(transfer_thread.max_chunk_size()),
                        extend_window_divisor),
        thread_(transfer_thread),
        chunk_timeout_(chunk_timeout),
        max_retries_(max_retries),
        max_lifetime_retries_(max_lifetime_retries),
        on_server_transfer_started_(std::move(on_start)),
        on_server_transfer_ended_(std::move(on_end)) {
    thread_.SetServerCompletionCallback(
        [this](uint32_t session_id, Status status) {
          if (on_server_transfer_ended_) {
            on_server_transfer_ended_(session_id, status);
          }
        });
  }

  void HandleChunk(ConstByteSpan message, internal::TransferType type);
  void ResourceStatusCallback(Status status,
                              const internal::ResourceStatus& stats);

  internal::TransferParameters max_parameters_;
  TransferThread& thread_;

  chrono::SystemClock::duration chunk_timeout_;
  uint8_t max_retries_;
  uint32_t max_lifetime_retries_;

  sync::Mutex resource_responder_mutex_;
  rpc::RawUnaryResponder resource_responder_
      PW_GUARDED_BY(resource_responder_mutex_);

  StartCallback on_server_transfer_started_;
  EndCallback on_server_transfer_ended_;
};

/// @endmodule

}  // namespace pw::transfer
