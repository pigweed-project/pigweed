// Copyright 2026 The Pigweed Authors
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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_rpc/client.h"
#include "pw_rpc/examples/chat_service.rpc.pb.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"

namespace pw::rpc::examples {
namespace {

// DOCSTAG: [pw_rpc-nanopb-service-impl]
class ChatService final
    : public ::chat::pw_rpc::nanopb::Chat::Service<ChatService> {
 public:
  // 1. Unary RPC
  pw::Status GetRoomInformation(const chat_RoomInfoRequest& request,
                                chat_RoomInfoResponse& response) {
    PW_LOG_INFO("Room requested: %s", request.room);
    std::strncpy(response.room, request.room, sizeof(response.room));
    response.users = 42;
    return pw::OkStatus();
  }

  // 2. Server Streaming RPC
  void ListUsersInRoom(
      const chat_ListUsersRequest& request,
      pw::rpc::NanopbServerWriter<chat_ListUsersResponse>& writer) {
    PW_LOG_INFO("Listing users in room: %s", request.room);
    chat_ListUsersResponse user1{.user = "Alice"};
    static_cast<void>(writer.Write(user1));
    chat_ListUsersResponse user2{.user = "Bob"};
    static_cast<void>(writer.Write(user2));
    static_cast<void>(writer.Finish(pw::OkStatus()));
  }

  // 3. Client Streaming RPC
  void UploadFile(
      pw::rpc::NanopbServerReader<chat_UploadFileRequest,
                                  chat_UploadFileResponse>& reader) {
    upload_reader_ = std::move(reader);

    upload_reader_.set_on_next([this](const chat_UploadFileRequest& request) {
      total_bytes_ += request.chunk.size;
      if (request.chunk.size == 0) {
        // Upload finished: complete the call with total bytes transferred
        chat_UploadFileResponse response{
            .bytes_received = static_cast<uint32_t>(total_bytes_)};
        static_cast<void>(upload_reader_.Finish(response, pw::OkStatus()));
      }
    });
  }

  // 4. Bidirectional Streaming RPC
  void SendMessage(
      pw::rpc::NanopbServerReaderWriter<chat_ChatMessage, chat_ChatMessage>&
          stream) {
    chat_stream_ = std::move(stream);

    chat_stream_.set_on_next([this](const chat_ChatMessage& message) {
      chat_ChatMessage reply{.msg = "Echo", .timestamp = message.timestamp};
      static_cast<void>(chat_stream_.Write(reply));
    });
  }

 private:
  pw::rpc::NanopbServerReader<chat_UploadFileRequest, chat_UploadFileResponse>
      upload_reader_;
  pw::rpc::NanopbServerReaderWriter<chat_ChatMessage, chat_ChatMessage>
      chat_stream_;
  size_t total_bytes_ = 0;
};
// DOCSTAG: [pw_rpc-nanopb-service-impl]

class FakeOutput : public pw::rpc::ChannelOutput {
 public:
  constexpr FakeOutput() : pw::rpc::ChannelOutput("Fake") {}
  pw::Status Send(pw::span<const std::byte>) override { return pw::OkStatus(); }
};

FakeOutput fake_output;
pw::rpc::Channel channels[] = {pw::rpc::Channel::Create<1>(&fake_output)};
pw::rpc::Client client(channels);

// DOCSTAG: [pw_rpc-nanopb-client-full-example]
using ChatClient = ::chat::pw_rpc::nanopb::Chat::Client;

void LogRoomInformation(const chat_RoomInfoResponse& response,
                        pw::Status status) {
  if (status.ok()) {
    PW_LOG_INFO("Room %s has %u users",
                response.room,
                static_cast<unsigned>(response.users));
  }
}

[[maybe_unused]] void InvokeSomeRpcs() {
  ChatClient chat_client(client, 1);

  // Unary call
  auto call =
      chat_client.GetRoomInformation({.room = "pigweed"}, LogRoomInformation);
  if (!call.active()) {
    return;
  }
}
// DOCSTAG: [pw_rpc-nanopb-client-full-example]

// DOCSTAG: [pw_rpc-nanopb-client-streaming-call]
[[maybe_unused]] void StartUpload() {
  ChatClient chat_client(client, 1);

  auto writer = chat_client.UploadFile(
      [](const chat_UploadFileResponse& response, pw::Status status) {
        if (status.ok()) {
          PW_LOG_INFO("Uploaded %u bytes",
                      static_cast<unsigned>(response.bytes_received));
        }
      });

  chat_UploadFileRequest chunk{.chunk = {.size = 0, .bytes = {0}}};
  static_cast<void>(writer.Write(chunk));
  static_cast<void>(writer.RequestCompletion());
}
// DOCSTAG: [pw_rpc-nanopb-client-streaming-call]

// DOCSTAG: [pw_rpc-nanopb-client-bidi-streaming-call]
[[maybe_unused]] void StartChat() {
  ChatClient chat_client(client, 1);

  auto stream = chat_client.SendMessage(
      [](const chat_ChatMessage& msg) {
        PW_LOG_INFO("Message from room: %s", msg.msg);
      },
      [](pw::Status status) { PW_LOG_INFO("Chat closed: %s", status.str()); });

  chat_ChatMessage msg{.msg = "Hello!", .timestamp = 0};
  static_cast<void>(stream.Write(msg));
  static_cast<void>(stream.RequestCompletion());
}
// DOCSTAG: [pw_rpc-nanopb-client-bidi-streaming-call]

}  // namespace
}  // namespace pw::rpc::examples
