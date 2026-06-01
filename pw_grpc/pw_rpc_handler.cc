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

#include "pw_grpc/pw_rpc_handler.h"

#include <cinttypes>

#include "pw_rpc/internal/config.h"

static_assert(PW_RPC_LOCKLESS_CHANNEL_SEND,
              "pw_grpc requires PW_RPC_LOCKLESS_CHANNEL_SEND to be enabled.");

namespace pw::grpc {

using pw::rpc::internal::pwpb::PacketType;

void PwRpcHandler::OnClose(StreamId id) { ResetStream(id); }

void PwRpcHandler::OnNewConnection() { ResetAllStreams(); }

Status PwRpcHandler::OnNew(StreamId id,
                           InlineString<kMaxMethodNameSize> full_method_name) {
  // Parse out service and method from `/grpc.examples.echo.Echo/UnaryEcho`
  // formatted name.
  std::string_view view = std::string_view(full_method_name);
  auto split_pos = view.find_last_of('/');
  if (view.empty() || view[0] != '/' || split_pos == std::string_view::npos) {
    PW_LOG_WARN("Can't determine service/method name id=%" PRIu32 " name=%s",
                id,
                full_method_name.c_str());
    return Status::NotFound();
  }

  // Look up method in the server.
  std::string_view service_name = view.substr(1, split_pos - 1);
  std::string_view method_name = view.substr(split_pos + 1);
  uint32_t service_id = rpc::internal::Hash(service_name);
  uint32_t method_id = rpc::internal::Hash(method_name);
  const auto [service, method] = server_.FindMethod(service_id, method_id);
  if (service == nullptr || method == nullptr) {
    PW_LOG_WARN("Unknown method '%s'", full_method_name.c_str());
    return Status::NotFound();
  }

  return CreateStream(id, service_id, method_id, method->type());
}

Status PwRpcHandler::OnMessage(StreamId id, ByteSpan message) {
  auto stream = LookupStream(id);
  if (!stream.ok()) {
    PW_LOG_INFO("Handler.OnMessage id=%" PRIu32 " size=%zu: unknown stream",
                id,
                message.size());
    return Status::NotFound();
  }

  switch (stream->method_type) {
    case pw::rpc::MethodType::kUnary:
    case pw::rpc::MethodType::kServerStreaming: {
      auto packet = pw::rpc::internal::Packet(PacketType::kRequest,
                                              channel_id_,
                                              stream->service_id,
                                              stream->method_id,
                                              id,
                                              message,
                                              pw::OkStatus());
      PW_TRY(server_.ProcessPacket(packet));
      break;
    }
    case pw::rpc::MethodType::kClientStreaming:
    case pw::rpc::MethodType::kBidirectionalStreaming: {
      if (stream->half_closed) {
        PW_LOG_INFO(
            "Handler.OnMessage half closed, ignoring message id=%" PRIu32, id);
        break;
      }
      if (!stream->sent_request) {
        auto packet = pw::rpc::internal::Packet(PacketType::kRequest,
                                                channel_id_,
                                                stream->service_id,
                                                stream->method_id,
                                                id,
                                                {},
                                                pw::OkStatus());
        PW_TRY(server_.ProcessPacket(packet));
        MarkSentRequest(id);
      }

      auto packet = pw::rpc::internal::Packet(PacketType::kClientStream,
                                              channel_id_,
                                              stream->service_id,
                                              stream->method_id,
                                              id,
                                              message,
                                              pw::OkStatus());
      PW_TRY(server_.ProcessPacket(packet));
      break;
    }
    default:
      PW_LOG_WARN("Unexpected method type");
      return Status::Internal();
  }

  return OkStatus();
}

void PwRpcHandler::OnHalfClose(StreamId id) {
  auto stream = LookupStream(id);
  if (!stream.ok()) {
    PW_LOG_INFO("OnHalfClose unknown stream");
    return;
  }

  if (stream->method_type == pw::rpc::MethodType::kClientStreaming ||
      stream->method_type == pw::rpc::MethodType::kBidirectionalStreaming) {
    auto packet =
        pw::rpc::internal::Packet(PacketType::kClientRequestCompletion,
                                  channel_id_,
                                  stream->service_id,
                                  stream->method_id,
                                  id,
                                  {},
                                  pw::OkStatus());
    HalfCloseStream(id);

    server_.ProcessPacket(packet).IgnoreError();
  }
}

void PwRpcHandler::OnCancel(StreamId id) {
  auto stream = LookupStream(id);
  if (!stream.ok()) {
    PW_LOG_INFO("OnCancel unknown stream");
    return;
  }

  auto packet = pw::rpc::internal::Packet(PacketType::kClientError,
                                          channel_id_,
                                          stream->service_id,
                                          stream->method_id,
                                          id,
                                          {},
                                          pw::Status::Cancelled());
  ResetStream(id);

  server_.ProcessPacket(packet).IgnoreError();
}

Result<rpc::MethodType> PwRpcHandler::LookupMethodType(uint32_t call_id) {
  PW_TRY_ASSIGN(Stream stream, LookupStream(call_id));
  return stream.method_type;
}

Result<PwRpcHandler::Stream> PwRpcHandler::LookupStream(StreamId id) {
  auto streams_locked = streams_.acquire();
  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    if (stream.id == id) {
      return stream;
    }
  }
  return Status::NotFound();
}

void PwRpcHandler::ResetAllStreams() {
  auto streams_locked = streams_.acquire();
  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    stream.id = 0;
  }
}

void PwRpcHandler::ResetStream(StreamId id) {
  auto streams_locked = streams_.acquire();
  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    if (stream.id == id) {
      stream.id = 0;
      break;
    }
  }
}

void PwRpcHandler::HalfCloseStream(StreamId id) {
  auto streams_locked = streams_.acquire();
  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    if (stream.id == id) {
      stream.half_closed = true;
      break;
    }
  }
}

void PwRpcHandler::MarkSentRequest(StreamId id) {
  auto streams_locked = streams_.acquire();
  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    if (stream.id == id) {
      stream.sent_request = true;
      break;
    }
  }
}

Status PwRpcHandler::CreateStream(StreamId id,
                                  uint32_t service_id,
                                  uint32_t method_id,
                                  pw::rpc::MethodType method_type) {
  auto streams_locked = streams_.acquire();

  for (size_t i = 0; i < streams_locked->size(); ++i) {
    auto& stream = (*streams_locked)[i];
    if (!stream.id) {
      stream.id = id;
      stream.service_id = service_id;
      stream.method_id = method_id;
      stream.method_type = method_type;
      stream.sent_request = false;
      stream.half_closed = false;
      return OkStatus();
    }
  }
  return Status::ResourceExhausted();
}

}  // namespace pw::grpc
