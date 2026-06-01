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

#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

#include "pw_allocator/best_fit.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_bytes/byte_builder.h"
#include "pw_bytes/span.h"
#include "pw_checksum/crc32.h"
#include "pw_grpc/connection.h"
#include "pw_grpc/examples/echo/echo.rpc.pwpb.h"
#include "pw_grpc/grpc_channel_output.h"
#include "pw_grpc/pw_rpc_handler.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc/internal/hash.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc_transport/service_registry.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/socket_stream.h"
#include "pw_stream/stream.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"

using pw::grpc::StreamId;

namespace {
static constexpr size_t kBufferSize = 512;

class EchoService
    : public ::grpc::examples::echo::pw_rpc::pwpb::Echo::Service<EchoService> {
 public:
  void UnaryEcho(pw::ConstByteSpan request,
                 pw::rpc::RawUnaryResponder& responder) {
    auto message =
        ::grpc::examples::echo::pwpb::EchoRequest::FindMessage(request);
    if (!message.ok()) {
      responder.Finish({}, pw::OkStatus()).IgnoreError();
      return;
    }

    if (message->size() < 100) {
      PW_LOG_INFO("UnaryEcho %s", message->data());
    } else {
      PW_LOG_INFO("UnaryEcho (len=%zu)", message->size());
    }

    quiet_ = message->compare("quiet") == 0;
    last_unary_responder_ = std::move(responder);
    if (quiet_) {
      return;
    }

    std::array<std::byte, kBufferSize> mem_writer_buffer_;
    std::array<std::byte, kBufferSize> encoder_scratch_buffer_;
    pw::stream::MemoryWriter writer(mem_writer_buffer_);
    ::grpc::examples::echo::pwpb::EchoResponse::StreamEncoder encoder(
        writer, encoder_scratch_buffer_);

    auto checksum = message->rfind("crc32:", 0) == 0;
    if (checksum) {
      uint32_t crc32 = pw::checksum::Crc32::Calculate(
          pw::span(reinterpret_cast<const std::byte*>(message->data()),
                   message->size()));
      encoder.Write({.message = std::string_view(std::to_string(crc32))})
          .IgnoreError();
    } else {
      encoder.Write({.message = *message}).IgnoreError();
    }

    last_unary_responder_.Finish(writer.WrittenData(), pw::OkStatus())
        .IgnoreError();
  }

  void ServerStreamingEcho(
      const ::grpc::examples::echo::pwpb::EchoRequest::Message& request,
      ServerWriter<::grpc::examples::echo::pwpb::EchoResponse::Message>&
          writer) {
    PW_LOG_INFO("ServerStreamingEcho %s", request.message.c_str());
    quiet_ = request.message.compare("quiet") == 0;
    last_writer_ = std::move(writer);
    if (quiet_) {
      PW_LOG_INFO("not writing server streaming echo");
      return;
    }
    size_t num_responses = 3;
    bool block = request.message.compare("block") == 0;
    if (block) {
      num_responses = 10;
    }
    std::string big_msg(200, 'a');
    for (size_t i = 0; i < num_responses; ++i) {
      if (block) {
        auto status =
            last_writer_.Write({.message = std::string_view(big_msg)});
        if (!status.ok()) {
          PW_LOG_ERROR("Write failed with status %u", status.code());
        }
      } else {
        last_writer_.Write({.message = request.message}).IgnoreError();
      }
    }
    last_writer_.Finish(pw::OkStatus()).IgnoreError();
  }

  void ClientStreamingEcho(
      ServerReader<::grpc::examples::echo::pwpb::EchoRequest::Message,
                   ::grpc::examples::echo::pwpb::EchoResponse::Message>&
          reader) {
    PW_LOG_INFO("ClientStreamingEcho");
    last_reader_ = std::move(reader);
    last_reader_.set_on_next(
        [this](
            const ::grpc::examples::echo::pwpb::EchoRequest::Message& request) {
          quiet_ = request.message.compare("quiet") == 0;
          PW_LOG_INFO("ClientStreaming message %s", request.message.c_str());
        });

    last_reader_.set_on_completion_requested([this]() {
      if (quiet_) {
        return;
      }
      last_reader_.Finish({.message = "done"}).IgnoreError();
    });
  }

  void BidirectionalStreamingEcho(
      ServerReaderWriter<::grpc::examples::echo::pwpb::EchoRequest::Message,
                         ::grpc::examples::echo::pwpb::EchoResponse::Message>&
          reader_writer) {
    PW_LOG_INFO("BidirectionalStreamingEcho");
    last_reader_writer_ = std::move(reader_writer);
    last_reader_writer_.set_on_completion_requested([this]() {
      if (quiet_) {
        return;
      }
      last_reader_writer_.Finish(pw::OkStatus()).IgnoreError();
    });
    last_reader_writer_.set_on_next(
        [this](
            const ::grpc::examples::echo::pwpb::EchoRequest::Message& request) {
          PW_LOG_INFO("BidiStreaming message %s", request.message.c_str());
          quiet_ = request.message.compare("quiet") == 0;
          if (quiet_) {
            return;
          }
          last_reader_writer_.Write({.message = request.message}).IgnoreError();
        });
  }

 private:
  pw::rpc::RawUnaryResponder last_unary_responder_{};
  ServerWriter<::grpc::examples::echo::pwpb::EchoResponse::Message>
      last_writer_{};
  ServerReader<::grpc::examples::echo::pwpb::EchoRequest::Message,
               ::grpc::examples::echo::pwpb::EchoResponse::Message>
      last_reader_{};
  ServerReaderWriter<::grpc::examples::echo::pwpb::EchoRequest::Message,
                     ::grpc::examples::echo::pwpb::EchoResponse::Message>
      last_reader_writer_{};
  bool quiet_ = false;
};

constexpr uint32_t kTestChannelId = 1;

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> args(argv, argv + argc);
  uint16_t port = 3400;
  int num_connections = 1;

  if (args.size() > 1) {
    if (args[1] == "--help") {
      PW_LOG_INFO("Usage: [port=3400] [num_connections=1]");
      PW_LOG_INFO(
          "  num_connections positional arg sets how many socket connections "
          "should be processed before exit");
      exit(0);
    }
    port = static_cast<uint16_t>(stoi(args[1]));
  }

  if (args.size() > 2) {
    num_connections = stoi(args[2]);
  }

  std::setbuf(stdout, nullptr);  // unbuffered stdout

  pw::stream::ServerSocket server;
  pw::grpc::GrpcChannelOutput rpc_egress;
  std::array<pw::rpc::Channel, 1> tx_channels(
      {pw::rpc::Channel::Create<kTestChannelId>(&rpc_egress)});
  pw::rpc::ServiceRegistry service_registry(tx_channels);

  EchoService echo_service;
  service_registry.RegisterService(echo_service);

  pw::grpc::PwRpcHandler handler(kTestChannelId,
                                 service_registry.client_server().server());
  rpc_egress.set_callbacks(handler);

  PW_LOG_INFO("Main.Listen on port=%d", port);
  if (auto status = server.Listen(port); !status.ok()) {
    PW_LOG_ERROR("Main.Listen failed code=%d", status.code());
    return 1;
  }

  for (int i = 0; i < num_connections; ++i) {
    PW_LOG_INFO("Main.Accept");
    auto socket = server.Accept();
    if (!socket.ok()) {
      PW_LOG_ERROR("Main.Accept failed code=%d", socket.status().code());
      return 1;
    }

    PW_LOG_INFO("Main.Run");

    constexpr size_t kMaxSendQueueSize = 4096;
    pw::allocator::LibCAllocator message_assembly_allocator;

    std::array<std::byte, kMaxSendQueueSize> send_allocator_data;
    pw::allocator::BestFitAllocator<> send_allocator(send_allocator_data);
    pw::thread::test::TestThreadContext connection_thread_context;
    pw::thread::test::TestThreadContext send_thread_context;
    pw::grpc::ConnectionThread conn(
        *socket,
        send_thread_context.options(),
        handler,
        [&socket]() { socket->Close(); },
        &message_assembly_allocator,
        send_allocator);
    rpc_egress.set_connection(conn);

    pw::Thread conn_thread(connection_thread_context.options(), conn);
    conn_thread.join();
  }

  PW_LOG_INFO("Main.Run completed");
  return 0;
}
