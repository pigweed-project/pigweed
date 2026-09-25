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

#include <signal.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "pw_assert/assert.h"
#include "pw_chrono/system_clock.h"
#include "pw_rpc_transport/rpc_transport.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/socket_stream.h"
#include "pw_sync/condition_variable.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/sleep.h"
#include "pw_thread/thread_core.h"

namespace pw::rpc {

namespace internal {

void LogSocketListenError(Status);
void LogSocketAcceptError(Status);
void LogSocketConnectError(Status);
void LogSocketReadError(Status);
void LogSocketIngressHandlerError(Status);

}  // namespace internal

template <size_t kReadBufferSize>
class SocketRpcTransport : public RpcFrameSender, public thread::ThreadCore {
 public:
  struct AsServer {};
  struct AsClient {};

  static constexpr AsServer kAsServer{};
  static constexpr AsClient kAsClient{};

  SocketRpcTransport(AsServer, uint16_t port)
      : role_(ClientServerRole::kServer), port_(port) {}

  SocketRpcTransport(AsServer, uint16_t port, RpcIngressHandler& ingress)
      : role_(ClientServerRole::kServer), port_(port), ingress_(&ingress) {}

  SocketRpcTransport(AsClient, std::string_view host, uint16_t port)
      : role_(ClientServerRole::kClient), host_(host), port_(port) {}

  SocketRpcTransport(AsClient,
                     std::string_view host,
                     uint16_t port,
                     RpcIngressHandler& ingress)
      : role_(ClientServerRole::kClient),
        host_(host),
        port_(port),
        ingress_(&ingress) {}

  size_t MaximumTransmissionUnit() const override { return kReadBufferSize; }
  uint16_t port() const { return port_; }
  void set_ingress(RpcIngressHandler& ingress) { ingress_ = &ingress; }

  Status Send(RpcFrame frame) override {
    std::lock_guard lock(write_mutex_);
    PW_TRY(socket_stream_.Write(frame.header));
    PW_TRY(socket_stream_.Write(frame.payload));
    return OkStatus();
  }

  // Returns once the transport is connected to its peer.
  void WaitUntilConnected() {
    std::unique_lock lock(connected_mutex_);
    connected_cv_.wait(lock, [this]() { return connected_; });
  }

  // Returns once the transport is ready to be used (e.g. the server is
  // listening on the port or the client is ready to connect).
  void WaitUntilReady() {
    std::unique_lock lock(ready_mutex_);
    ready_cv_.wait(lock, [this]() { return ready_; });
  }

  void Start() {
    while (!stopped_) {
      const auto connect_status = EstablishConnection();
      if (!connect_status.ok()) {
        this_thread::sleep_for(kConnectionRetryPeriod);
        continue;
      }
      NotifyConnected();

      while (!stopped_) {
        const auto read_status = ReadData();
        // Break if ReadData was cancelled after the transport was stopped.
        if (stopped_) {
          break;
        }
        if (!read_status.ok()) {
          internal::LogSocketReadError(read_status);
        }
        if (read_status.IsOutOfRange()) {
          // Need to reconnect (we don't close the stream here because it's
          // already done in SocketStream::DoRead).
          {
            std::lock_guard lock(connected_mutex_);
            connected_ = false;
          }
          break;
        }
      }
    }
  }

  void Stop() {
    stopped_ = true;
    socket_stream_.Close();
    server_socket_.Close();
  }

 private:
  enum class ClientServerRole { kClient, kServer };
  static constexpr chrono::SystemClock::duration kConnectionRetryPeriod =
      std::chrono::milliseconds(100);

  void Run() override { Start(); }

  // Establishes or accepts a new socket connection. Returns when socket_stream_
  // contains a valid socket connection, or when the transport is stopped.
  Status EstablishConnection() {
    if (role_ == ClientServerRole::kServer) {
      return Serve();
    }
    return Connect();
  }

  Status Serve() {
    PW_DASSERT(role_ == ClientServerRole::kServer);

    if (!listening_) {
      const auto listen_status = server_socket_.Listen(port_);
      if (!listen_status.ok()) {
        internal::LogSocketListenError(listen_status);
        return listen_status;
      }
    }

    listening_ = true;
    port_ = server_socket_.port();
    NotifyReady();

    Result<stream::SocketStream> stream = server_socket_.Accept();
    // If Accept was cancelled due to stopping the transport, return without
    // error.
    if (stopped_) {
      return OkStatus();
    }
    if (!stream.ok()) {
      internal::LogSocketAcceptError(stream.status());
      return stream.status();
    }
    // Ensure that the writer is done writing before updating the stream.
    std::lock_guard lock(write_mutex_);
    socket_stream_ = std::move(*stream);
    return OkStatus();
  }

  Status Connect() {
    PW_DASSERT(role_ == ClientServerRole::kClient);
    NotifyReady();

    std::lock_guard lock(write_mutex_);
    auto connect_status = socket_stream_.Connect(host_.c_str(), port_);
    if (!connect_status.ok()) {
      internal::LogSocketConnectError(connect_status);
    }
    return connect_status;
  }

  Status ReadData() {
    PW_DASSERT(ingress_ != nullptr);
    PW_TRY_ASSIGN(auto buffer, socket_stream_.Read(read_buffer_));
    const auto ingress_status = ingress_->ProcessIncomingData(buffer);
    if (!ingress_status.ok()) {
      internal::LogSocketIngressHandlerError(ingress_status);
    }
    // ReadData only returns socket stream read errors; ingress errors are only
    // logged.
    return OkStatus();
  }

  void NotifyConnected() {
    {
      std::lock_guard lock(connected_mutex_);
      connected_ = true;
    }
    connected_cv_.notify_all();
  }

  void NotifyReady() {
    {
      std::lock_guard lock(ready_mutex_);
      ready_ = true;
    }
    ready_cv_.notify_all();
  }

  ClientServerRole role_;
  const std::string host_;
  std::atomic<uint16_t> port_;
  RpcIngressHandler* ingress_ = nullptr;

  // write_mutex_ must be held by the thread performing socket writes.
  sync::Mutex write_mutex_;
  stream::SocketStream socket_stream_;
  stream::ServerSocket server_socket_;

  sync::Mutex ready_mutex_;
  sync::ConditionVariable ready_cv_;
  bool ready_ = false;

  sync::Mutex connected_mutex_;
  sync::ConditionVariable connected_cv_;
  bool connected_ = false;

  std::atomic<bool> stopped_ = false;
  bool listening_ = false;
  std::array<std::byte, kReadBufferSize> read_buffer_{};
};

}  // namespace pw::rpc
