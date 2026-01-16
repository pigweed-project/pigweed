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

#include "pw_grpc/connection.h"

#include <cinttypes>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_grpc_private/hpack.h"
#include "pw_log/log.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_status/try.h"

namespace pw::grpc {
namespace internal {

// RFC 9113 §6
// Enum names left in naming style of RFC
enum class FrameType : uint8_t {
  DATA = 0x00,
  HEADERS = 0x01,
  PRIORITY = 0x02,
  RST_STREAM = 0x03,
  SETTINGS = 0x04,
  PUSH_PROMISE = 0x05,
  PING = 0x06,
  GOAWAY = 0x07,
  WINDOW_UPDATE = 0x08,
  CONTINUATION = 0x09,
};

// RFC 9113 §4.1
constexpr size_t kFrameHeaderEncodedSize = 9;
struct FrameHeader {
  uint32_t payload_length;
  FrameType type;
  uint8_t flags;
  StreamId stream_id;
};

// RFC 9113 §7
// Enum names left in naming style of RFC
enum class Http2Error : uint32_t {
  NO_ERROR = 0x00,
  PROTOCOL_ERROR = 0x01,
  INTERNAL_ERROR = 0x02,
  FLOW_CONTROL_ERROR = 0x03,
  SETTINGS_TIMEOUT = 0x04,
  STREAM_CLOSED = 0x05,
  FRAME_SIZE_ERROR = 0x06,
  REFUSED_STREAM = 0x07,
  CANCEL = 0x08,
  COMPRESSION_ERROR = 0x09,
  CONNECT_ERROR = 0x0a,
  ENHANCE_YOUR_CALM = 0x0b,
  INADEQUATE_SECURITY = 0x0c,
  HTTP_1_1_REQUIRED = 0x0d,
};

}  // namespace internal

namespace {

using internal::FrameHeader;
using internal::FrameType;
using internal::Http2Error;
using internal::kMaxConcurrentStreams;
using internal::kMaxGrpcMessageSize;

// RFC 9113 §3.4
constexpr std::string_view kExpectedConnectionPrefaceLiteral(
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");

static_assert(kMaxMethodNameSize == kHpackMaxStringSize);

constexpr size_t kLengthPrefixedMessageHdrSize = 5;

enum {
  FLAGS_ACK = 0x01,
  FLAGS_END_STREAM = 0x01,
  FLAGS_END_HEADERS = 0x04,
  FLAGS_PADDED = 0x08,
  FLAGS_PRIORITY = 0x20,
};

// RFC 9113 §6.5.2
enum SettingType : uint16_t {
  SETTINGS_HEADER_TABLE_SIZE = 0x01,
  SETTINGS_ENABLE_PUSH = 0x02,
  SETTINGS_MAX_CONCURRENT_STREAMS = 0x03,
  SETTINGS_INITIAL_WINDOW_SIZE = 0x04,
  SETTINGS_MAX_FRAME_SIZE = 0x05,
  SETTINGS_MAX_HEADER_LIST_SIZE = 0x06,
};

Status ReadExactly(stream::Reader& reader, ByteSpan buffer) {
  size_t bytes_read = 0;
  while (bytes_read < buffer.size()) {
    PW_TRY_ASSIGN(auto out, reader.Read(buffer.subspan(bytes_read)));
    bytes_read += out.size();
  }
  return OkStatus();
}

Result<FrameHeader> ReadFrameHeader(stream::Reader& reader) {
  std::array<std::byte, internal::kFrameHeaderEncodedSize> buffer;
  PW_TRY(ReadExactly(reader, buffer));

  // RFC 9113 §4.1
  FrameHeader out;
  ByteBuilder builder(as_writable_bytes(span{buffer}));
  auto it = builder.begin();
  auto type_and_length = it.ReadUint32(endian::big);
  out.payload_length = type_and_length >> 8;
  out.type = static_cast<FrameType>(type_and_length & 0xff);
  out.flags = it.ReadUint8();
  out.stream_id = it.ReadUint32(endian::big) & 0x7fffffff;
  return out;
}

template <typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
constexpr T ToNetworkOrder(T value) {
  return bytes::ConvertOrder(/*from=*/endian::native,
                             /*to=*/endian::big,
                             value);
}

template <typename T, std::enable_if_t<std::is_enum_v<T>, bool> = true>
constexpr std::underlying_type_t<T> ToNetworkOrder(T value) {
  return ToNetworkOrder(static_cast<std::underlying_type_t<T>>(value));
}

// Use this instead of FrameHeader when writing frames.
PW_PACKED(struct) WireFrameHeader {
  WireFrameHeader(FrameHeader h)
      : payload_length_and_type(ToNetworkOrder(h.payload_length << 8 |
                                               static_cast<uint32_t>(h.type))),
        flags(h.flags),
        stream_id(ToNetworkOrder(h.stream_id)) {}

  uint32_t payload_length_and_type;
  uint8_t flags;
  uint32_t stream_id;
};

template <typename T, std::size_t N, typename... Args, std::size_t... I>
constexpr std::array<T, N> MakeArrayWithValueImpl(std::index_sequence<I...>,
                                                  Args&&... args) {
  // For each index 'I' in the sequence, the comma operator expression
  // discards the index and evaluates to a new T(args...).
  // The result is an initializer list of N identical elements.
  return {(static_cast<void>(I), T(args...))...};
}

template <typename T, std::size_t N, typename... Args>
constexpr std::array<T, N> MakeArrayWithValue(Args&&... args) {
  return MakeArrayWithValueImpl<T, N>(std::make_index_sequence<N>{},
                                      std::forward<Args>(args)...);
}

}  // namespace

Connection::Connection(stream::Reader& reader,
                       SendQueue& send_queue,
                       RequestCallbacks& callbacks,
                       Allocator* message_assembly_allocator,
                       Allocator& send_allocator)
    : send_allocator_(send_allocator),
      shared_state_(std::in_place,
                    message_assembly_allocator,
                    send_allocator_,
                    send_queue),
      reader_(*this, callbacks, reader),
      writer_(*this) {}

Connection::SharedState::SharedState(
    allocator::Allocator* message_assembly_allocator,
    Allocator& send_allocator,
    SendQueue& send_queue)
    : streams_(
          MakeArrayWithValue<Stream, kMaxConcurrentStreams>(send_allocator)),
      message_assembly_allocator_(message_assembly_allocator),
      send_allocator_(send_allocator),
      send_queue_(send_queue) {}

Result<Connection::DataFrame> Connection::DataFrame::Create(
    Allocator& allocator, size_t message_payload_size) {
  DataFrame data_frame(allocator, message_payload_size);
  if (data_frame.bytes_ == nullptr) {
    return Status::ResourceExhausted();
  }
  return data_frame;
}

Connection::DataFrame::DataFrame(Allocator& allocator,
                                 size_t message_payload_size)
    : bytes_(allocator.MakeUnique<std::byte[]>(message_payload_size +
                                               kLengthPrefixedMessageHdrSize +
                                               sizeof(WireFrameHeader))) {}

size_t Connection::DataFrame::frame_payload_size() const {
  return bytes_.size() - sizeof(WireFrameHeader);
}

ByteSpan Connection::DataFrame::writable_frame_header() {
  return {bytes_.get(), sizeof(WireFrameHeader)};
}

ByteSpan Connection::DataFrame::writable_message_prefix() {
  return {bytes_.get() + sizeof(WireFrameHeader),
          kLengthPrefixedMessageHdrSize};
}

ByteSpan Connection::DataFrame::writable_message_payload() {
  return {
      bytes_.get() + sizeof(WireFrameHeader) + kLengthPrefixedMessageHdrSize,
      bytes_.size() - sizeof(WireFrameHeader) - kLengthPrefixedMessageHdrSize};
}

Status Connection::Reader::ProcessFrame() {
  if (!received_connection_preface_) {
    return Status::FailedPrecondition();
  }

  PW_TRY_ASSIGN(auto frame, ReadFrameHeader(reader_));
  switch (frame.type) {
    // Frames that we handle.
    case FrameType::DATA:
      PW_TRY(ProcessDataFrame(frame));
      break;
    case FrameType::HEADERS:
      PW_TRY(ProcessHeadersFrame(frame));
      break;
    case FrameType::PRIORITY:
      PW_TRY(ProcessIgnoredFrame(frame));
      break;
    case FrameType::RST_STREAM:
      PW_TRY(ProcessRstStreamFrame(frame));
      break;
    case FrameType::SETTINGS:
      PW_TRY(ProcessSettingsFrame(frame, /*send_ack=*/true));
      break;
    case FrameType::PING:
      PW_TRY(ProcessPingFrame(frame));
      break;
    case FrameType::WINDOW_UPDATE:
      PW_TRY(ProcessWindowUpdateFrame(frame));
      break;

    // Frames that trigger an immediate connection close.
    case FrameType::GOAWAY:
      PW_LOG_ERROR("Client sent GOAWAY");
      // don't bother sending GOAWAY in response
      return Status::Internal();
    case FrameType::PUSH_PROMISE:
      PW_LOG_ERROR("Client sent PUSH_PROMISE");
      SendGoAway(Http2Error::PROTOCOL_ERROR);
      return Status::Internal();
    case FrameType::CONTINUATION:
      PW_LOG_ERROR("Client sent CONTINUATION: unsupported");
      SendGoAway(Http2Error::INTERNAL_ERROR);
      return Status::Internal();
  }

  return OkStatus();
}

pw::Status Connection::SharedState::CreateStream(StreamId id,
                                                 int32_t initial_send_window) {
  for (size_t i = 0; i < streams_.size(); i++) {
    if (streams_[i].id != 0) {
      continue;
    }
    PW_LOG_DEBUG("Conn.CreateStream id=%" PRIu32 " at slot=%" PRIu32,
                 id,
                 static_cast<uint32_t>(i));
    streams_[i].id = id;
    streams_[i].half_closed = false;
    streams_[i].started_response = false;
    streams_[i].recv_window = kDefaultInitialWindowSize;
    streams_[i].send_window = initial_send_window;
    return OkStatus();
  }
  PW_LOG_WARN("Conn.CreateStream id=%" PRIu32 " OUT OF SPACE", id);
  return Status::ResourceExhausted();
}

void Connection::SharedState::ForAllStreams(
    Function<void(Stream*)>&& callback) {
  for (size_t i = 0; i < streams_.size(); i++) {
    if (streams_[i].id != 0) {
      callback(&streams_[i]);
    }
  }
}

Connection::Stream* Connection::SharedState::LookupStream(StreamId id) {
  for (size_t i = 0; i < streams_.size(); i++) {
    if (streams_[i].id == id) {
      return &streams_[i];
    }
  }
  return nullptr;
}

Status Connection::SharedState::DrainResponseQueue(Stream& stream) {
  while (stream.response_queue.size() > 0) {
    size_t message_size = stream.response_queue.front().frame_payload_size();

    if (static_cast<int32_t>(message_size) > stream.send_window ||
        static_cast<int32_t>(message_size) > connection_send_window_) {
      // TODO(b/471320234): remove after debugging
      if (!stream.debug_logged_no_window) {
        stream.debug_logged_no_window = true;
        PW_LOG_WARN("stream id=%d not enough window: msg=%zu ssw=%d csw=%d",
                    stream.id,
                    message_size,
                    stream.send_window,
                    connection_send_window_);
      }
      break;
    }

    DataFrame front = std::move(stream.response_queue.front());
    stream.response_queue.pop();

    PW_TRY(SendQueuedDataFrame(stream, std::move(front)));
  }
  return OkStatus();
}

Status Connection::SharedState::DrainResponseQueues() {
  for (Stream& stream : streams_) {
    PW_TRY(DrainResponseQueue(stream));
  }
  return OkStatus();
}

Status Connection::SharedState::SendBytes(ConstByteSpan message) {
  UniquePtr<std::byte[]> buffer =
      send_allocator_.MakeUnique<std::byte[]>(message.size());
  if (buffer == nullptr) {
    return Status::ResourceExhausted();
  }

  std::memcpy(buffer.get(), message.data(), message.size());
  send_queue_.QueueSend(std::move(buffer));
  return OkStatus();
}

// RFC 9113 §6.1
Status Connection::SharedState::SendData(StreamId stream_id,
                                         DataFrame&& data_frame) {
  const size_t message_size = data_frame.frame_payload_size();
  PW_LOG_DEBUG("Conn.Send DATA with id=%" PRIu32 " len=%" PRIu32,
               stream_id,
               static_cast<uint32_t>(message_size));

  send_queue_.QueueSend(data_frame.release());
  return OkStatus();
}

// RFC 9113 §6.2
Status Connection::SharedState::SendHeaders(StreamId stream_id,
                                            ConstByteSpan payload1,
                                            ConstByteSpan payload2,
                                            bool end_stream) {
  PW_LOG_DEBUG("Conn.Send HEADERS with id=%" PRIu32 " len1=%" PRIu32
               " len2=%" PRIu32 " end=%d",
               stream_id,
               static_cast<uint32_t>(payload1.size()),
               static_cast<uint32_t>(payload2.size()),
               end_stream);
  WireFrameHeader frame(FrameHeader{
      .payload_length =
          static_cast<uint32_t>(payload1.size() + payload2.size()),
      .type = FrameType::HEADERS,
      .flags = FLAGS_END_HEADERS,
      .stream_id = stream_id,
  });

  if (end_stream) {
    frame.flags |= FLAGS_END_STREAM;
  }

  ConstByteSpan frame_span = ObjectAsBytes(frame);

  UniquePtr<std::byte[]> buffer = send_allocator_.MakeUnique<std::byte[]>(
      frame_span.size() + payload1.size() + payload2.size());

  if (buffer == nullptr) {
    return Status::ResourceExhausted();
  }

  size_t offset = 0;
  std::memcpy(buffer.get(), frame_span.data(), frame_span.size());
  offset += frame_span.size();

  if (!payload1.empty()) {
    std::memcpy(buffer.get() + offset, payload1.data(), payload1.size());
    offset += payload1.size();
  }
  if (!payload2.empty()) {
    std::memcpy(buffer.get() + offset, payload2.data(), payload2.size());
    offset += payload2.size();
  }

  send_queue_.QueueSend(std::move(buffer));
  return OkStatus();
}

// RFC 9113 §6.4
Status Connection::SharedState::SendRstStream(StreamId stream_id,
                                              Http2Error code) {
  PW_PACKED(struct) RstStreamFrame {
    WireFrameHeader header;
    uint32_t error_code;
  };
  RstStreamFrame frame{
      .header = WireFrameHeader(FrameHeader{
          .payload_length = 4,
          .type = FrameType::RST_STREAM,
          .flags = 0,
          .stream_id = stream_id,
      }),
      .error_code = ToNetworkOrder(code),
  };
  return SendBytes(ObjectAsBytes(frame));
}

// RFC 9113 §6.9
Status Connection::SharedState::SendWindowUpdates(Stream* stream,
                                                  uint32_t connection_increment,
                                                  uint32_t stream_increment) {
  // It is illegal to send updates with increment=0. We won't include
  // stream_increment if it is 0 or stream is null.
  if (connection_increment == 0) {
    return OkStatus();
  }

  if (connection_increment & 0x80000000 || stream_increment & 0x80000000) {
    // Upper bit is reserved, error.
    return Status::InvalidArgument();
  }

  PW_LOG_DEBUG("Conn.Send WINDOW_UPDATE frames with id=%" PRIu32
               " connection_increment=%" PRIu32 " stream_increment=%" PRIu32,
               stream ? stream->id : 0,
               connection_increment,
               stream_increment);

  connection_recv_window_ += connection_increment;
  if (stream) {
    stream->recv_window += stream_increment;
    if (stream_increment == 0) {
      return OkStatus();
    }
  }

  PW_PACKED(struct) WindowUpdateFrame {
    WireFrameHeader header;
    uint32_t increment;
  };
  WindowUpdateFrame frames[2] = {
      {
          .header = WireFrameHeader(FrameHeader{
              .payload_length = 4,
              .type = FrameType::WINDOW_UPDATE,
              .flags = 0,
              .stream_id = 0,
          }),
          .increment = ToNetworkOrder(connection_increment),
      },
      // Will not be included if stream is null.
      {
          .header = WireFrameHeader(FrameHeader{
              .payload_length = 4,
              .type = FrameType::WINDOW_UPDATE,
              .flags = 0,
              .stream_id = stream ? stream->id : 0,
          }),
          .increment = ToNetworkOrder(stream_increment),
      },
  };
  return SendBytes(as_bytes(span{frames, stream ? 2U : 1U}));
}

// RFC 9113 §6.5
Status Connection::SharedState::SendSettingsAck() {
  PW_LOG_DEBUG("Conn.Send SETTINGS ACK");
  WireFrameHeader frame(FrameHeader{
      .payload_length = 0,
      .type = FrameType::SETTINGS,
      .flags = FLAGS_ACK,
      .stream_id = 0,
  });
  return SendBytes(ObjectAsBytes(frame));
}

Status Connection::Writer::SendResponseMessage(StreamId stream_id,
                                               ConstByteSpan message) {
  auto state = connection_.LockState();

  if (message.size() > kMaxGrpcMessageSize) {
    PW_LOG_WARN("Message %" PRIu32 " bytes on id=%" PRIu32
                " exceeds maximum message size",
                static_cast<uint32_t>(message.size()),
                stream_id);
    return Status::InvalidArgument();
  }

  PW_TRY_ASSIGN(DataFrame data_frame,
                DataFrame::Create(state->send_allocator(), message.size()));

  WireFrameHeader frame(FrameHeader{
      .payload_length = static_cast<uint32_t>(data_frame.frame_payload_size()),
      .type = FrameType::DATA,
      .flags = 0,
      .stream_id = stream_id,
  });

  ConstByteSpan frame_span = ObjectAsBytes(frame);
  std::copy_n(frame_span.begin(),
              frame_span.size(),
              data_frame.writable_frame_header().begin());

  ByteBuilder prefix(data_frame.writable_message_prefix());
  prefix.PutUint8(0);
  prefix.PutUint32(static_cast<uint32_t>(message.size()), endian::big);

  std::copy_n(message.begin(),
              message.size(),
              data_frame.writable_message_payload().begin());

  return state->QueueStreamResponse(stream_id, std::move(data_frame));
}

Status Connection::SharedState::QueueStreamResponse(StreamId id,
                                                    DataFrame&& data_frame) {
  auto stream = LookupStream(id);
  if (!stream) {
    return Status::NotFound();
  }
  if (!stream->response_queue.try_emplace(std::move(data_frame))) {
    return Status::ResourceExhausted();
  }
  // Try and send if we have window
  return DrainResponseQueues();
}

Status Connection::SharedState::SendQueuedDataFrame(Connection::Stream& stream,
                                                    DataFrame&& data_frame) {
  const size_t message_size = data_frame.frame_payload_size();

  auto status = OkStatus();
  if (!stream.started_response) {
    stream.started_response = true;
    status = SendHeaders(stream.id,
                         ResponseHeadersPayload(),
                         ConstByteSpan(),
                         /*end_stream=*/false);
  }

  if (status.ok()) {
    status = SendData(stream.id, std::move(data_frame));
  }

  if (!status.ok()) {
    PW_LOG_WARN("Failed sending response message on id=%" PRIu32 " error=%d",
                stream.id,
                status.code());
    return status;
  }

  stream.send_window -= message_size;
  connection_send_window_ -= message_size;

  return OkStatus();
}

Status Connection::Writer::SendResponseComplete(StreamId stream_id,
                                                Status response_code) {
  auto state = connection_.LockState();
  auto stream = state->LookupStream(stream_id);
  if (!stream) {
    return Status::NotFound();
  }

  Status status;
  if (!stream->started_response) {
    // If the response has not started yet, we need to include the initial
    // headers.
    PW_LOG_DEBUG("Conn.SendResponseWithTrailers id=%" PRIu32 " code=%d",
                 stream_id,
                 response_code.code());
    status = state->SendHeaders(stream_id,
                                ResponseHeadersPayload(),
                                ResponseTrailersPayload(response_code),
                                /*end_stream=*/true);
  } else {
    PW_LOG_DEBUG("Conn.SendTrailers id=%" PRIu32 " code=%d",
                 stream_id,
                 response_code.code());
    status = state->SendHeaders(stream_id,
                                ConstByteSpan(),
                                ResponseTrailersPayload(response_code),
                                /*end_stream=*/true);
  }

  if (!status.ok()) {
    PW_LOG_WARN("Failed sending response complete on id=%" PRIu32 " error=%d",
                stream_id,
                status.code());
    return Status::Unavailable();
  }

  PW_LOG_DEBUG("Conn.CloseStream id=%" PRIu32, stream_id);
  stream->Reset();

  return OkStatus();
}

void Connection::Reader::CloseStream(Connection::Stream* stream) {
  StreamId id = stream->id;
  PW_LOG_DEBUG("Conn.CloseStream id=%" PRIu32, id);
  stream->Reset();
  callbacks_.OnCancel(id);
}

// RFC 9113 §3.4
Status Connection::Reader::ProcessConnectionPreface() {
  if (received_connection_preface_) {
    return OkStatus();
  }

  callbacks_.OnNewConnection();

  // The preface starts with a literal string.
  auto literal = span{payload_scratch_}.subspan(
      0, kExpectedConnectionPrefaceLiteral.size());

  PW_TRY(ReadExactly(reader_, literal));
  if (std::memcmp(literal.data(),
                  kExpectedConnectionPrefaceLiteral.data(),
                  kExpectedConnectionPrefaceLiteral.size()) != 0) {
    PW_LOG_ERROR("Invalid connection preface literal");
    return Status::Internal();
  }

  PW_LOG_DEBUG("Conn.Preface received literal");

  // Client must send a SETTINGS frames.
  PW_TRY_ASSIGN(auto client_frame, ReadFrameHeader(reader_));
  if (client_frame.type != FrameType::SETTINGS) {
    PW_LOG_ERROR(
        "Connection preface missing SETTINGS frame, found frame.type=%d",
        static_cast<int>(client_frame.type));
    return Status::Internal();
  }

  // Don't send an ACK yet, we'll do that below.
  PW_TRY(ProcessSettingsFrame(client_frame, /*send_ack=*/false));
  PW_LOG_DEBUG("Conn.Preface received SETTINGS");

  // We must send a SETTINGS frame.
  // RFC 9113 §6.5.2
  PW_PACKED(struct) Setting {
    uint16_t id;
    uint32_t value;
  };
  PW_PACKED(struct) SettingsFrame {
    WireFrameHeader header;
    Setting settings[2];
  };
  SettingsFrame server_frame{
      .header = WireFrameHeader(FrameHeader{
          .payload_length = 12,
          .type = FrameType::SETTINGS,
          .flags = 0,
          .stream_id = 0,
      }),
      .settings =
          {
              {
                  .id = ToNetworkOrder(SETTINGS_HEADER_TABLE_SIZE),
                  .value = ToNetworkOrder(kHpackDynamicHeaderTableSize),
              },
              {
                  .id = ToNetworkOrder(SETTINGS_MAX_CONCURRENT_STREAMS),
                  .value = ToNetworkOrder(kMaxConcurrentStreams),
              },
          },
  };
  PW_LOG_DEBUG("Conn.Send SETTINGS");

  {
    auto state = connection_.LockState();
    PW_TRY(state->SendBytes(ObjectAsBytes(server_frame)));

    // We must ack the client's SETTINGS frame *after* sending our SETTINGS.
    PW_TRY(state->SendSettingsAck());
  }

  received_connection_preface_ = true;
  PW_LOG_DEBUG("Conn.Preface complete");
  return OkStatus();
}

// RFC 9113 §6.1
Status Connection::Reader::ProcessDataFrame(const FrameHeader& frame) {
  PW_LOG_DEBUG("Conn.Recv DATA id=%" PRIu32 " flags=0x%x len=%" PRIu32,
               frame.stream_id,
               frame.flags,
               frame.payload_length);

  if (frame.stream_id == 0) {
    // RFC 9113 §6.1: "If a DATA frame is received whose Stream Identifier field
    // is 0x00, the recipient MUST respond with a connection error of type
    // PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }

  {
    auto state = connection_.LockState();
    auto stream = state->LookupStream(frame.stream_id);

    // From RFC 9113 §6.9: "A receiver that receives a flow-controlled frame
    // MUST always account for its contribution against the connection
    // flow-control window, unless the receiver treats this as a connection
    // error. This is necessary even if the frame is in error. The sender
    // counts the frame toward the flow-control window, but if the receiver
    // does not, the flow-control window at the sender and receiver can become
    // different."
    PW_TRY(state->UpdateRecvWindow(stream, frame.payload_length));

    if (!stream) {
      PW_LOG_DEBUG("Ignoring DATA on closed stream id=%" PRIu32,
                   frame.stream_id);
      // Unlock since ProcessIgnoredFrame will try and read all the ignored
      // data, and also may try and take the lock in the error case.
      connection_.UnlockState(std::move(state));
      PW_TRY(ProcessIgnoredFrame(frame));
      // Stream has been fully closed: silently ignore.
      return OkStatus();
    }

    if (stream->half_closed) {
      PW_LOG_ERROR("Recv DATA on half-closed stream id=%" PRIu32,
                   frame.stream_id);
      // Unlock since ProcessIgnoredFrame will try and read all the ignored
      // data, and also may try and take the lock in the error case.
      connection_.UnlockState(std::move(state));
      PW_TRY(ProcessIgnoredFrame(frame));
      state = connection_.LockState();
      stream = state->LookupStream(frame.stream_id);
      if (stream) {
        // RFC 9113 §6.1: "If a DATA frame is received whose stream is not in
        // the "open" or "half-closed (local)" state, the recipient MUST respond
        // with a stream error of type STREAM_CLOSED."
        PW_TRY(SendRstStreamAndClose(state, stream, Http2Error::STREAM_CLOSED));
      }
      return OkStatus();
    }
  }

  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));

  // Drop padding.
  if ((frame.flags & FLAGS_PADDED) != 0) {
    if (payload.size() < 1) {
      // RFC 9113 §4.2: "An endpoint MUST send an error code of FRAME_SIZE_ERROR
      // if a frame ... is too small to contain mandatory frame data."
      SendGoAway(Http2Error::FRAME_SIZE_ERROR);
      return Status::Internal();
    }

    uint32_t pad_length = static_cast<uint32_t>(payload[0]);
    if (pad_length >= frame.payload_length) {
      // RFC 9113 §6.1: "If the length of the padding is the length of the frame
      // payload or greater, the recipient MUST treat this as a connection error
      // of type PROTOCOL_ERROR."
      SendGoAway(Http2Error::PROTOCOL_ERROR);
      return Status::Internal();
    }
    payload = payload.subspan(1, payload.size() - pad_length - 1);
  }

  auto state = connection_.LockState();
  Stream* stream = state->LookupStream(frame.stream_id);
  if (!stream) {
    return OkStatus();
  }

  // Parse repeated grpc Length-Prefix-Message.
  // https://github.com/grpc/grpc/blob/v1.60.x/doc/PROTOCOL-HTTP2.md#requests
  while (!payload.empty()) {
    uint32_t message_length;

    // If we aren't reassembling a message, read the next length prefix.
    if (stream->assembly_buffer == nullptr) {
      size_t read =
          std::min(5 - static_cast<size_t>(stream->assembly.prefix.received),
                   payload.size());
      std::copy(payload.begin(),
                payload.begin() + static_cast<ptrdiff_t>(read),
                stream->assembly.prefix.buffer.data() +
                    stream->assembly.prefix.received);
      stream->assembly.prefix.received += read;
      payload = payload.subspan(read);

      // Read the length prefix.
      if (stream->assembly.prefix.received < 5) {
        continue;
      }
      stream->assembly.prefix.received = 0;

      ByteBuilder builder(stream->assembly.prefix.buffer);
      auto it = builder.begin();
      auto message_compressed = it.ReadUint8();
      message_length = it.ReadUint32(endian::big);
      if (message_compressed != 0) {
        PW_LOG_ERROR("Unsupported: grpc message is compressed");
        PW_TRY(
            SendRstStreamAndClose(state, stream, Http2Error::INTERNAL_ERROR));
        return OkStatus();
      }

      if (message_length > payload.size()) {
        // gRPC message is split across DATA frames, must allocate buffer.
        if (!state->message_assembly_allocator()) {
          PW_LOG_ERROR(
              "Unsupported: split grpc message without allocator provided");
          PW_TRY(
              SendRstStreamAndClose(state, stream, Http2Error::INTERNAL_ERROR));
          return OkStatus();
        }

        stream->assembly_buffer =
            state->message_assembly_allocator()->MakeUnique<std::byte[]>(
                message_length);
        if (stream->assembly_buffer == nullptr) {
          PW_LOG_ERROR("Partial message reassembly buffer allocation failed");
          PW_TRY(
              SendRstStreamAndClose(state, stream, Http2Error::INTERNAL_ERROR));
          return OkStatus();
        }
        stream->assembly.message.length = message_length;
        stream->assembly.message.received = 0;
        continue;
      }
    }

    pw::ByteSpan message;

    // Reading message payload.
    if (stream->assembly_buffer != nullptr) {
      uint32_t read = std::min(
          stream->assembly.message.length - stream->assembly.message.received,
          static_cast<uint32_t>(payload.size()));
      std::copy(
          payload.begin(),
          payload.begin() + read,
          stream->assembly_buffer.get() + stream->assembly.message.received);
      payload = payload.subspan(read);
      stream->assembly.message.received += read;
      if (stream->assembly.message.received < stream->assembly.message.length) {
        continue;
      }
      // Fully received message.
      message = pw::span(stream->assembly_buffer.get(),
                         stream->assembly.message.length);
    } else {
      message = payload.subspan(0, message_length);
      payload = payload.subspan(message_length);
    }

    // Release state lock before callback, reacquire after.
    connection_.UnlockState(std::move(state));
    const auto status = callbacks_.OnMessage(frame.stream_id, message);
    state = connection_.LockState();
    stream = state->LookupStream(frame.stream_id);
    if (!stream) {
      return OkStatus();
    }

    if (!status.ok()) {
      PW_TRY(SendRstStreamAndClose(state, stream, Http2Error::INTERNAL_ERROR));
      return OkStatus();
    }

    if (stream->assembly_buffer != nullptr) {
      stream->assembly_buffer = nullptr;
      stream->assembly = {};
    }
  }

  // grpc requires every request stream to end with an empty DATA frame with
  // FLAGS_END_STREAM. If a client sends FLAGS_END_STREAM with a non-empty
  // payload, it's not specified how the server should respond. We choose to
  // accept the payload before ending the stream.
  // See: https://github.com/grpc/grpc/blob/v1.60.x/doc/PROTOCOL-HTTP2.md.
  if ((frame.flags & FLAGS_END_STREAM) != 0) {
    stream->half_closed = true;
    connection_.UnlockState(std::move(state));
    callbacks_.OnHalfClose(frame.stream_id);
  }

  return OkStatus();
}

// RFC 9113 §6.2
Status Connection::Reader::ProcessHeadersFrame(const FrameHeader& frame) {
  PW_LOG_DEBUG("Conn.Recv HEADERS id=%" PRIu32 " len=%" PRIu32,
               frame.stream_id,
               frame.payload_length);

  if (frame.stream_id == 0) {
    // RFC 9113 §6.2: "If a HEADERS frame is received whose Stream Identifier
    // field is 0x00, the recipient MUST respond with a connection error of type
    // PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }
  if (frame.stream_id % 2 != 1 || frame.stream_id <= last_stream_id_) {
    // RFC 9113 §5.1.1: "Streams initiated by a client MUST use odd-numbered
    // stream identifiers ... The identifier of a newly established stream MUST
    // be numerically greater than all streams that the initiating endpoint has
    // opened ... An endpoint that receives an unexpected stream identifier MUST
    // respond with a connection error of type PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }

  last_stream_id_ = frame.stream_id;

  {
    auto state = connection_.LockState();
    if (Stream* stream = state->LookupStream(frame.stream_id);
        stream != nullptr) {
      PW_LOG_DEBUG("Client sent HEADERS after the first stream message");
      // Unlock since ProcessIgnoredFrame will try and read all the ignored
      // data, and also may try and take the lock in the error case.
      connection_.UnlockState(std::move(state));
      PW_TRY(ProcessIgnoredFrame(frame));
      state = connection_.LockState();
      stream = state->LookupStream(frame.stream_id);
      if (stream) {
        // grpc requests cannot contain trailers.
        // See:
        // https://github.com/grpc/grpc/blob/v1.60.x/doc/PROTOCOL-HTTP2.md.
        PW_TRY(
            SendRstStreamAndClose(state, stream, Http2Error::PROTOCOL_ERROR));
      }
      return OkStatus();
    }
  }

  if ((frame.flags & FLAGS_END_STREAM) != 0) {
    PW_LOG_DEBUG("Client sent HEADERS with END_STREAM");
    PW_TRY(ProcessIgnoredFrame(frame));
    // grpc requests must send END_STREAM in an empty DATA frame.
    // See: https://github.com/grpc/grpc/blob/v1.60.x/doc/PROTOCOL-HTTP2.md.
    auto state = connection_.LockState();
    PW_TRY(state->SendRstStream(frame.stream_id, Http2Error::PROTOCOL_ERROR));
    return OkStatus();
  }
  if ((frame.flags & FLAGS_END_HEADERS) == 0) {
    PW_LOG_ERROR("Client sent HEADERS frame without END_HEADERS: unsupported");
    SendGoAway(Http2Error::INTERNAL_ERROR);
    return Status::Internal();
  }

  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));

  // Drop padding.
  if ((frame.flags & FLAGS_PADDED) != 0) {
    if (payload.size() < 1) {
      // RFC 9113 §4.2: "An endpoint MUST send an error code of FRAME_SIZE_ERROR
      // if a frame ... is too small to contain mandatory frame data. A frame
      // size error in a frame that could alter the state of the entire
      // connection MUST be treated as a connection error"
      SendGoAway(Http2Error::FRAME_SIZE_ERROR);
      return Status::Internal();
    }
    uint32_t pad_length = static_cast<uint32_t>(payload[0]);
    if (pad_length >= frame.payload_length) {
      // RFC 9113 §6.2: "If the length of the padding is the length of the frame
      // payload or greater, the recipient MUST treat this as a connection error
      // of type PROTOCOL_ERROR."
      SendGoAway(Http2Error::PROTOCOL_ERROR);
      return Status::Internal();
    }
    payload = payload.subspan(1, payload.size() - pad_length - 1);
  }

  // Drop priority fields.
  if ((frame.flags & FLAGS_PRIORITY) != 0) {
    if (payload.size() < 5) {
      // RFC 9113 §4.2: "An endpoint MUST send an error code of FRAME_SIZE_ERROR
      // if a frame ... is too small to contain mandatory frame data."
      SendGoAway(Http2Error::FRAME_SIZE_ERROR);
      return Status::Internal();
    }
    payload = payload.subspan(5);
  }

  PW_TRY_ASSIGN(auto method_name, HpackParseRequestHeaders(payload));
  {
    auto state = connection_.LockState();
    if (!state->CreateStream(frame.stream_id, initial_send_window_).ok()) {
      PW_LOG_WARN("Too many streams, rejecting id=%" PRIu32, frame.stream_id);
      return state->SendRstStream(frame.stream_id, Http2Error::REFUSED_STREAM);
    }
  }

  if (const auto status = callbacks_.OnNew(frame.stream_id, method_name);
      !status.ok()) {
    if (status.IsNotFound()) {
      return connection_.writer_.SendResponseComplete(
          frame.stream_id, pw::Status::Unimplemented());
    }
    auto state = connection_.LockState();
    if (Stream* stream = state->LookupStream(frame.stream_id);
        stream != nullptr) {
      return SendRstStreamAndClose(state, stream, Http2Error::INTERNAL_ERROR);
    }
  }

  return OkStatus();
}

Status Connection::SharedState::AddConnectionSendWindow(int32_t delta) {
  if (!CheckedIncrement(connection_send_window_, delta)) {
    return Status::Internal();
  }

  DrainResponseQueues().IgnoreError();

  return OkStatus();
}

Status Connection::SharedState::AddAllStreamsSendWindow(int32_t delta) {
  for (size_t i = 0; i < streams_.size(); i++) {
    if (streams_[i].id == 0) {
      continue;
    }
    if (!CheckedIncrement(streams_[i].send_window, delta)) {
      return Status::Internal();
    }
  }

  DrainResponseQueues().IgnoreError();

  return OkStatus();
}

Status Connection::SharedState::AddStreamSendWindow(StreamId id,
                                                    int32_t delta) {
  Stream* stream = LookupStream(id);
  if (!stream) {
    return Status::NotFound();
  }

  if (!CheckedIncrement(stream->send_window, delta)) {
    return Status::Internal();
  }

  DrainResponseQueues().IgnoreError();

  return OkStatus();
}

Status Connection::SharedState::UpdateRecvWindow(Stream* stream,
                                                 uint32_t data_length) {
  connection_recv_window_ -= data_length;

  // Make sure window value won't underflow.
  if (connection_recv_window_ > kTargetConnectionWindowSize) {
    return Status::InvalidArgument();
  }

  uint32_t connection_increment = static_cast<uint32_t>(
      kTargetConnectionWindowSize - connection_recv_window_);

  uint32_t stream_increment = 0;
  if (stream) {
    stream->recv_window -= data_length;
    if (stream->recv_window > kTargetStreamWindowSize) {
      return Status::InvalidArgument();
    }
    stream_increment =
        static_cast<uint32_t>(kTargetStreamWindowSize - stream->recv_window);
  }

  constexpr size_t kConnectionWindowUpdateThreshold =
      kTargetConnectionWindowSize / 2;
  constexpr size_t kStreamWindowUpdateThreshold = kTargetStreamWindowSize / 2;

  // Suppress window updates till we reach target.
  if (connection_increment > kConnectionWindowUpdateThreshold ||
      stream_increment > kStreamWindowUpdateThreshold) {
    PW_TRY(SendWindowUpdates(stream, connection_increment, stream_increment));
  }

  return OkStatus();
}

// RFC 9113 §6.4
Status Connection::Reader::ProcessRstStreamFrame(const FrameHeader& frame) {
  PW_LOG_DEBUG("Conn.Recv RST_STREAM id=%" PRIu32 " len=%" PRIu32,
               frame.stream_id,
               frame.payload_length);

  if (frame.stream_id == 0) {
    // RFC 9113 §6.4: "If a RST_STREAM frame is received with a stream
    // identifier of 0x00, the recipient MUST treat this as a connection error
    // of type PROTOCOL_ERROR".
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }
  if (frame.stream_id > last_stream_id_) {
    // RFC 9113 §6.4: "If a RST_STREAM frame identifying an idle stream is
    // received, the recipient MUST treat this as a connection error of type
    // PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }
  if (frame.payload_length != 4) {
    // RFC 9113 §6.4: "A RST_STREAM frame with a length other than 4 octets MUST
    // be treated as a connection error of type FRAME_SIZE_ERROR."
    SendGoAway(Http2Error::FRAME_SIZE_ERROR);
    return Status::Internal();
  }

  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));
  ByteBuilder builder(payload);
  auto error_code = builder.begin().ReadUint32(endian::big);

  PW_LOG_DEBUG("Conn.RstStream id=%" PRIu32 " error=%" PRIu32,
               frame.stream_id,
               error_code);
  auto state = connection_.LockState();
  if (Stream* stream = state->LookupStream(frame.stream_id);
      stream != nullptr) {
    CloseStream(stream);
  }
  return OkStatus();
}

// RFC 9113 §6.5
Status Connection::Reader::ProcessSettingsFrame(const FrameHeader& frame,
                                                bool send_ack) {
  PW_LOG_DEBUG("Conn.Recv SETTINGS len=%" PRIu32 " flags=0x%x",
               frame.payload_length,
               frame.flags);

  if ((frame.flags & FLAGS_ACK) != 0) {
    // RFC 9113 §6.5: "Receipt of a SETTINGS frame with the ACK flag set and a
    // length field value other than 0 MUST be treated as a connection error of
    // type FRAME_SIZE_ERROR."
    if (frame.payload_length != 0) {
      PW_LOG_ERROR("Invalid SETTINGS frame: has ACK with non-empty payload");
      SendGoAway(Http2Error::FRAME_SIZE_ERROR);
      return Status::Internal();
    }
    // Don't ACK an ACK.
    send_ack = false;
  } else {
    // RFC 9113 §6.5: "A SETTINGS frame with a length other than a multiple of 6
    // octets MUST be treated as a connection error of type FRAME_SIZE_ERROR."
    if (frame.payload_length % 6 != 0) {
      PW_LOG_ERROR("Invalid SETTINGS frame: payload size invalid");
      SendGoAway(Http2Error::FRAME_SIZE_ERROR);
      return Status::Internal();
    }
  }

  if (frame.stream_id != 0) {
    // RFC 9113 §6.5: "If an endpoint receives a SETTINGS frame whose Stream
    // Identifier field is anything other than 0x00, the endpoint MUST respond
    // with a connection error of type PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }

  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));

  // RFC 9113 §6.5.2
  ByteBuilder builder(payload);
  // ByteBuilder.end() points at end of written buffer view, not input.
  const ByteBuilder::iterator end(payload.data() + payload.size());
  for (auto it = builder.begin(); it != end;) {
    auto id = it.ReadUint16(endian::big);
    auto value = it.ReadUint32(endian::big);
    PW_LOG_DEBUG("Applying SETTING id=%" PRIu16 " value=%" PRIu32, id, value);
    switch (id) {
      case SETTINGS_INITIAL_WINDOW_SIZE: {
        // RFC 9113 §6.5.2: "Values above the maximum flow-control window size
        // of 2^31-1 MUST be treated as a connection error of type
        // FLOW_CONTROL_ERROR."
        if ((value & 0x80000000) != 0) {
          SendGoAway(Http2Error::FLOW_CONTROL_ERROR);
          return Status::Internal();
        }
        // RFC 9113 §6.9.2: "When the value of SETTINGS_INITIAL_WINDOW_SIZE
        // changes, a receiver MUST adjust the size of all stream flow-control
        // windows that it maintains by the difference between the new value and
        // the old value."
        int32_t newval = static_cast<int32_t>(value);
        int32_t delta = newval - initial_send_window_;
        auto state = connection_.LockState();
        if (const auto status = state->AddAllStreamsSendWindow(delta);
            !status.ok()) {
          SendGoAway(Http2Error::FLOW_CONTROL_ERROR);
          return Status::Internal();
        }
        initial_send_window_ = newval;
        break;
      }
      case SETTINGS_MAX_FRAME_SIZE:
        // RFC 9113 §6.5.2: "Values outside this range MUST be treated as a
        // connection error of type PROTOCOL_ERROR".
        if (value < 16384 || 16777215 < value) {
          SendGoAway(Http2Error::PROTOCOL_ERROR);
          return Status::Internal();
        }
        // We never send frame payloads larger than 16384, so we don't need to
        // track the client's preference.
        break;
      // Ignore these.
      // SETTINGS_HEADER_TABLE_SIZE: our responses don't use the dynamic table
      // SETTINGS_ENABLE_PUSH: we don't support push
      // SETTINGS_MAX_CONCURRENT_STREAMS: we don't support push
      // SETTINGS_MAX_HEADER_LIST_SIZE: we send very tiny response HEADERS
      default:
        break;
    }
  }

  if (send_ack) {
    auto state = connection_.LockState();
    PW_TRY(state->SendSettingsAck());
  }

  return OkStatus();
}

// RFC 9113 §6.7
Status Connection::Reader::ProcessPingFrame(const FrameHeader& frame) {
  PW_LOG_DEBUG("Conn.Recv PING len=%" PRIu32, frame.payload_length);

  if (frame.stream_id != 0) {
    // RFC 9113 §6.7: "If a PING frame is received with a Stream Identifier
    // field value other than 0x00, the recipient MUST respond with a connection
    // error of type PROTOCOL_ERROR."
    SendGoAway(Http2Error::PROTOCOL_ERROR);
    return Status::Internal();
  }
  if (frame.payload_length != 8) {
    // RFC 9113 §6.7: "Receipt of a PING frame with a length field value other
    // than 8 MUST be treated as a connection error of type FRAME_SIZE_ERROR."
    SendGoAway(Http2Error::FRAME_SIZE_ERROR);
    return Status::Internal();
  }

  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));

  // Don't ACK an ACK.
  if ((frame.flags & FLAGS_ACK) != 0) {
    return OkStatus();
  }

  // Send an ACK.
  PW_PACKED(struct) PingFrame {
    WireFrameHeader header;
    uint64_t opaque_data;
  };
  ByteBuilder builder(payload);
  PingFrame ack_frame = {
      .header = WireFrameHeader(FrameHeader{
          .payload_length = 8,
          .type = FrameType::PING,
          .flags = FLAGS_ACK,
          .stream_id = 0,
      }),
      // Since we're going to echo this, read as native endian so it gets echoed
      // exactly as-is.
      .opaque_data = builder.begin().ReadUint64(endian::native),
  };

  {
    auto state = connection_.LockState();
    PW_TRY(state->SendBytes(ObjectAsBytes(ack_frame)));
  }

  return OkStatus();
}

// RFC 9113 §6.9
Status Connection::Reader::ProcessWindowUpdateFrame(const FrameHeader& frame) {
  PW_LOG_DEBUG("Conn.Recv WINDOW_UPDATE id=%" PRIu32 " len=%" PRIu32,
               frame.stream_id,
               frame.payload_length);

  if (frame.payload_length != 4) {
    // RFC 9113 §6.9: "A WINDOW_UPDATE frame with a length other than 4 octets
    // MUST be treated as a connection error of type FRAME_SIZE_ERROR."
    SendGoAway(Http2Error::FRAME_SIZE_ERROR);
    return Status::Internal();
  }

  // Read window size increment.
  PW_TRY_ASSIGN(auto payload, ReadFramePayload(frame));
  ByteBuilder builder(payload);
  int32_t delta = static_cast<int32_t>(builder.begin().ReadUint32(endian::big) &
                                       0x7fffffff);

  auto state = connection_.LockState();
  auto stream = state->LookupStream(frame.stream_id);

  if (delta == 0) {
    // RFC 9113 §6.9: "A receiver MUST treat a WINDOW_UPDATE frame with an
    // increment of 0 as a stream error of type PROTOCOL_ERROR; errors on the
    // connection flow-control window MUST be treated as a connection error."
    if (frame.stream_id == 0) {
      SendGoAway(Http2Error::PROTOCOL_ERROR);
      return Status::Internal();
    } else {
      if (!stream) {
        // Already closed
        return OkStatus();
      }
      return SendRstStreamAndClose(state, stream, Http2Error::PROTOCOL_ERROR);
    }
  }

  // RFC 9113 §6.9.1: "If a sender receives a WINDOW_UPDATE that causes a
  // flow-control window to exceed 2^31-1 bytes, it MUST terminate either the
  // stream or the connection, as appropriate ... with an error code of
  // FLOW_CONTROL_ERROR"
  if (frame.stream_id == 0) {
    if (const auto status = state->AddConnectionSendWindow(delta);
        !status.ok()) {
      SendGoAway(Http2Error::FLOW_CONTROL_ERROR);
      return Status::Internal();
    }
  } else {
    if (!stream) {
      // Already closed
      return OkStatus();
    }
    if (const auto status = state->AddStreamSendWindow(stream->id, delta);
        !status.ok()) {
      return SendRstStreamAndClose(
          state, stream, Http2Error::FLOW_CONTROL_ERROR);
    }
  }

  return OkStatus();
}

// Advance past the payload.
Status Connection::Reader::ProcessIgnoredFrame(const FrameHeader& frame) {
  size_t to_read = frame.payload_length;
  while (to_read > 0) {
    auto chunk = span{payload_scratch_}.subspan(
        0, std::min(payload_scratch_.size(), to_read));
    PW_TRY(ReadExactly(reader_, chunk));
    to_read -= chunk.size();
  }
  return OkStatus();
}

Result<ByteSpan> Connection::Reader::ReadFramePayload(
    const FrameHeader& frame) {
  if (frame.payload_length == 0) {
    return ByteSpan();
  }
  if (frame.payload_length > payload_scratch_.size()) {
    PW_LOG_ERROR("Frame type=%d payload too large: %" PRIu32 " > %" PRIu32,
                 static_cast<int>(frame.type),
                 frame.payload_length,
                 static_cast<uint32_t>(payload_scratch_.size()));
    SendGoAway(Http2Error::FRAME_SIZE_ERROR);
    return Status::Internal();
  }
  auto payload = span{payload_scratch_}.subspan(0, frame.payload_length);
  PW_TRY(ReadExactly(reader_, payload));
  return payload;
}

// RFC 9113 §6.8
void Connection::Reader::SendGoAway(Http2Error code) {
  if (!received_connection_preface_) {
    // RFC 9113 §3.4: "A GOAWAY frame MAY be omitted in this case, since an
    // invalid preface indicates that the peer is not using HTTP/2."
    return;
  }

  PW_PACKED(struct) GoAwayFrame {
    WireFrameHeader header;
    uint32_t last_stream_id;
    uint32_t error_code;
  };
  GoAwayFrame frame{
      .header = WireFrameHeader(FrameHeader{
          .payload_length = 8,
          .type = FrameType::GOAWAY,
          .flags = 0,
          .stream_id = 0,
      }),
      .last_stream_id = ToNetworkOrder(last_stream_id_),
      .error_code = ToNetworkOrder(code),
  };

  {
    auto state = connection_.LockState();
    // Close all open streams.
    state->ForAllStreams([this](Stream* stream) { CloseStream(stream); });
    // Ignore errors since we're about to close the connection anyway.
    state->SendBytes(ObjectAsBytes(frame)).IgnoreError();
  }
}

// RFC 9113 §6.4
Status Connection::Reader::SendRstStreamAndClose(
    sync::BorrowedPointer<SharedState>& state,
    Stream* stream,
    Http2Error code) {
  state->SendRstStream(stream->id, code).IgnoreError();
  CloseStream(stream);
  return OkStatus();
}

}  // namespace pw::grpc
