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

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_rpc2/internal/pwpb_serialize.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/size_report/size_report.h"
#include "pw_rpc2/size_report/size_report.pwpb.rpc2.h"
#include "pw_rpc2/size_report/size_report.raw.rpc2.h"
#include "pw_rpc2/status.h"
#include "pw_rpc2/writer.h"

#if (defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_CORO_SERVICE) || \
     defined(PW_RPC2_SIZE_REPORT_PWPB_CORO_SERVICE)) &&      \
    PW_CXX_STANDARD_IS_SUPPORTED(20)
#include "pw_async2/coro.h"
#endif

namespace pw::rpc2::size_report {
namespace {

using SizeReportMessage = pwpb::SizeReportMessage::Message;

#if defined(PW_RPC2_SIZE_REPORT_PWPB_BASE) ||               \
    defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_SERVICE) ||      \
    defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_CORO_SERVICE) || \
    defined(PW_RPC2_SIZE_REPORT_PWPB_FUTURE_SERVICE) ||     \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CORO_SERVICE)
PW_NO_INLINE void ExercisePwpbTableEncoderDecoder(uint32_t mask) {
  SizeReportMessage msg{.value = mask};
  std::array<std::byte, 16> buffer{};
  auto size_result = internal::PwpbEncodedSizeBytes(
      as_bytes(span(&msg, 1)), &pwpb::SizeReportMessage::kMessageFields);
  PW_BLOAT_COND(size_result.ok(), mask);
  auto encode_result =
      internal::PwpbEncode(as_bytes(span(&msg, 1)),
                           &pwpb::SizeReportMessage::kMessageFields,
                           buffer);
  PW_BLOAT_COND(encode_result.ok(), mask);
  auto decode_status =
      internal::PwpbDecode(buffer,
                           &pwpb::SizeReportMessage::kMessageFields,
                           as_writable_bytes(span(&msg, 1)));
  PW_BLOAT_COND(decode_status.ok(), mask);
}
#endif

class BaselineRawService
    : public pw_rpc2::raw::BaselineSizeReport::Service<BaselineRawService> {
 public:
  class MeasureUnaryFuture {
   public:
    using value_type = void;
    MeasureUnaryFuture() = default;
    MeasureUnaryFuture(ConstBuf request, RawUnaryWriter responder)
        : finish_(responder.FinishCopy(std::move(request))) {}

    bool is_pendable() const { return finish_.is_pendable(); }
    bool is_complete() const { return finish_.is_complete(); }

    async2::Poll<> Pend(async2::Context& cx) {
      return finish_.Pend(cx).Readiness();
    }

   private:
    WriteFuture<ConstBuf> finish_;
  };
};

#if defined(PW_RPC2_SIZE_REPORT_RAW_UNARY_SERVICE)

class RawUnaryService
    : public pw_rpc2::raw::UnarySizeReport::Service<RawUnaryService> {
 public:
  using MeasureUnaryFuture = BaselineRawService::MeasureUnaryFuture;
};

#elif defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_SERVICE)

class PwpbUnaryService
    : public pw_rpc2::pwpb::UnarySizeReport::Service<PwpbUnaryService> {
 public:
  class MeasureUnaryFuture {
   public:
    using value_type = void;
    MeasureUnaryFuture() = default;
    MeasureUnaryFuture(SizeReportMessage request,
                       UnaryWriter<SizeReportMessage> responder)
        : finish_(responder.Finish(request)) {}

    bool is_pendable() const { return finish_.is_pendable(); }
    bool is_complete() const { return finish_.is_complete(); }

    async2::Poll<> Pend(async2::Context& cx) {
      return finish_.Pend(cx).Readiness();
    }

   private:
    WriteFuture<SizeReportMessage> finish_;
  };
};

#elif defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_CORO_SERVICE) && \
    PW_CXX_STANDARD_IS_SUPPORTED(20)

class PwpbUnaryCoroService
    : public pw_rpc2::pwpb::UnarySizeReport::Service<PwpbUnaryCoroService> {
 public:
  async2::Coro<void> MeasureUnary(async2::CoroContext,
                                  SizeReportMessage request,
                                  UnaryWriter<SizeReportMessage> responder) {
    co_await responder.Finish(request);
  }
};

#elif defined(PW_RPC2_SIZE_REPORT_RAW_FUTURE_SERVICE)

class RawFutureService
    : public pw_rpc2::raw::SizeReport::Service<RawFutureService> {
 public:
  using MeasureUnaryFuture = BaselineRawService::MeasureUnaryFuture;

  class MeasureServerStreamFuture {
   public:
    using value_type = void;
    MeasureServerStreamFuture() = default;
    MeasureServerStreamFuture(ConstBuf request, RawWriter writer)
        : state_(State::kWriting),
          request_(std::move(request)),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      if (state_ == State::kWriting) {
        if (!write_.is_pendable()) {
          write_ = writer_.WriteCopy(std::move(request_));
        }
        PW_AWAIT(RpcStatus status, write_, cx);
        if (!status.ok()) {
          state_ = State::kDone;
          return async2::Ready();
        }
        finish_ = writer_.Finish();
        state_ = State::kFinishing;
      }
      PW_AWAIT(finish_, cx);
      state_ = State::kDone;
      return async2::Ready();
    }

   private:
    enum class State { kUninitialized, kWriting, kFinishing, kDone };
    State state_ = State::kUninitialized;
    ConstBuf request_;
    RawWriter writer_;
    WriteFuture<ConstBuf> write_;
    WriteFuture<> finish_;
  };

  class MeasureClientStreamFuture {
   public:
    using value_type = void;
    MeasureClientStreamFuture() = default;
    MeasureClientStreamFuture(RawReader reader, RawUnaryWriter responder)
        : state_(State::kReading),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      while (state_ == State::kReading) {
        if (!read_.is_pendable()) {
          read_ = reader_.Read();
        }
        PW_AWAIT(RpcResult<ConstBuf> msg, read_, cx);
        if (msg.status().IsEndOfStream()) {
          finish_ = responder_.FinishCopy(std::move(last_));
          state_ = State::kFinishing;
          break;
        }
        if (!msg.ok()) {
          state_ = State::kDone;
          return async2::Ready();
        }
        last_ = std::move(*msg);
      }
      PW_AWAIT(finish_, cx);
      state_ = State::kDone;
      return async2::Ready();
    }

   private:
    enum class State { kUninitialized, kReading, kFinishing, kDone };
    State state_ = State::kUninitialized;
    ConstBuf last_;
    RawReader reader_;
    RawUnaryWriter responder_;
    RawReadFuture read_;
    WriteFuture<ConstBuf> finish_;
  };

  class MeasureBidiStreamFuture {
   public:
    using value_type = void;
    MeasureBidiStreamFuture() = default;
    MeasureBidiStreamFuture(RawReader reader, RawWriter writer)
        : state_(State::kReading),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      while (true) {
        switch (state_) {
          case State::kReading: {
            if (!read_.is_pendable()) {
              read_ = reader_.Read();
            }
            PW_AWAIT(RpcResult<ConstBuf> msg, read_, cx);
            if (msg.status().IsEndOfStream()) {
              finish_ = writer_.Finish();
              state_ = State::kFinishing;
              break;
            }
            if (!msg.ok()) {
              state_ = State::kDone;
              return async2::Ready();
            }
            write_ = writer_.WriteCopy(std::move(*msg));
            state_ = State::kWriting;
            break;
          }
          case State::kWriting: {
            PW_AWAIT(RpcStatus status, write_, cx);
            if (!status.ok()) {
              state_ = State::kDone;
              return async2::Ready();
            }
            state_ = State::kReading;
            break;
          }
          case State::kFinishing: {
            PW_AWAIT(finish_, cx);
            state_ = State::kDone;
            return async2::Ready();
          }
          case State::kUninitialized:
          case State::kDone:
            return async2::Ready();
        }
      }
    }

   private:
    enum class State { kUninitialized, kReading, kWriting, kFinishing, kDone };
    State state_ = State::kUninitialized;
    RawReader reader_;
    RawWriter writer_;
    RawReadFuture read_;
    WriteFuture<ConstBuf> write_;
    WriteFuture<> finish_;
  };
};

#elif defined(PW_RPC2_SIZE_REPORT_PWPB_FUTURE_SERVICE)

class PwpbFutureService
    : public pw_rpc2::pwpb::SizeReport::Service<PwpbFutureService> {
 public:
  class MeasureUnaryFuture {
   public:
    using value_type = void;
    MeasureUnaryFuture() = default;
    MeasureUnaryFuture(SizeReportMessage request,
                       UnaryWriter<SizeReportMessage> responder)
        : finish_(responder.Finish(request)) {}

    bool is_pendable() const { return finish_.is_pendable(); }
    bool is_complete() const { return finish_.is_complete(); }

    async2::Poll<> Pend(async2::Context& cx) {
      return finish_.Pend(cx).Readiness();
    }

   private:
    WriteFuture<SizeReportMessage> finish_;
  };

  class MeasureServerStreamFuture {
   public:
    using value_type = void;
    MeasureServerStreamFuture() = default;
    MeasureServerStreamFuture(SizeReportMessage request,
                              Writer<SizeReportMessage> writer)
        : state_(State::kWriting),
          request_(request),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      if (state_ == State::kWriting) {
        if (!write_.is_pendable()) {
          write_ = writer_.Write(SizeReportMessage{.value = request_.value});
        }
        PW_AWAIT(RpcStatus status, write_, cx);
        if (!status.ok()) {
          state_ = State::kDone;
          return async2::Ready();
        }
        finish_ = writer_.Finish();
        state_ = State::kFinishing;
      }
      PW_AWAIT(finish_, cx);
      state_ = State::kDone;
      return async2::Ready();
    }

   private:
    enum class State { kUninitialized, kWriting, kFinishing, kDone };
    State state_ = State::kUninitialized;
    SizeReportMessage request_{};
    Writer<SizeReportMessage> writer_;
    WriteFuture<SizeReportMessage> write_;
    WriteFuture<> finish_;
  };

  class MeasureClientStreamFuture {
   public:
    using value_type = void;
    MeasureClientStreamFuture() = default;
    MeasureClientStreamFuture(Reader<SizeReportMessage> reader,
                              UnaryWriter<SizeReportMessage> responder)
        : state_(State::kReading),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      while (state_ == State::kReading) {
        if (!read_.is_pendable()) {
          read_ = reader_.Read();
        }
        PW_AWAIT(RpcResult<SizeReportMessage> msg, read_, cx);
        if (msg.status().IsEndOfStream()) {
          finish_ = responder_.Finish(SizeReportMessage{.value = sum_});
          state_ = State::kFinishing;
          break;
        }
        if (!msg.ok()) {
          state_ = State::kDone;
          return async2::Ready();
        }
        sum_ += msg->value;
      }
      PW_AWAIT(finish_, cx);
      state_ = State::kDone;
      return async2::Ready();
    }

   private:
    enum class State { kUninitialized, kReading, kFinishing, kDone };
    State state_ = State::kUninitialized;
    uint32_t sum_ = 0;
    Reader<SizeReportMessage> reader_;
    UnaryWriter<SizeReportMessage> responder_;
    ReadFuture<SizeReportMessage> read_;
    WriteFuture<SizeReportMessage> finish_;
  };

  class MeasureBidiStreamFuture {
   public:
    using value_type = void;
    MeasureBidiStreamFuture() = default;
    MeasureBidiStreamFuture(Reader<SizeReportMessage> reader,
                            Writer<SizeReportMessage> writer)
        : state_(State::kReading),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    async2::Poll<> Pend(async2::Context& cx) {
      while (true) {
        switch (state_) {
          case State::kReading: {
            if (!read_.is_pendable()) {
              read_ = reader_.Read();
            }
            PW_AWAIT(RpcResult<SizeReportMessage> msg, read_, cx);
            if (msg.status().IsEndOfStream()) {
              finish_ = writer_.Finish();
              state_ = State::kFinishing;
              break;
            }
            if (!msg.ok()) {
              state_ = State::kDone;
              return async2::Ready();
            }
            write_ = writer_.Write(*msg);
            state_ = State::kWriting;
            break;
          }
          case State::kWriting: {
            PW_AWAIT(RpcStatus status, write_, cx);
            if (!status.ok()) {
              state_ = State::kDone;
              return async2::Ready();
            }
            state_ = State::kReading;
            break;
          }
          case State::kFinishing: {
            PW_AWAIT(finish_, cx);
            state_ = State::kDone;
            return async2::Ready();
          }
          case State::kUninitialized:
          case State::kDone:
            return async2::Ready();
        }
      }
    }

   private:
    enum class State { kUninitialized, kReading, kWriting, kFinishing, kDone };
    State state_ = State::kUninitialized;
    Reader<SizeReportMessage> reader_;
    Writer<SizeReportMessage> writer_;
    ReadFuture<SizeReportMessage> read_;
    WriteFuture<SizeReportMessage> write_;
    WriteFuture<> finish_;
  };
};

#elif defined(PW_RPC2_SIZE_REPORT_PWPB_CORO_SERVICE) && \
    PW_CXX_STANDARD_IS_SUPPORTED(20)

class PwpbCoroService
    : public pw_rpc2::pwpb::SizeReport::Service<PwpbCoroService> {
 public:
  async2::Coro<void> MeasureUnary(async2::CoroContext,
                                  SizeReportMessage request,
                                  UnaryWriter<SizeReportMessage> responder) {
    co_await responder.Finish(request);
  }

  async2::Coro<void> MeasureServerStream(async2::CoroContext,
                                         SizeReportMessage request,
                                         Writer<SizeReportMessage> writer) {
    RpcStatus status = co_await writer.Write(request);
    if (!status.ok()) {
      co_return;
    }
    co_await writer.Finish();
  }

  async2::Coro<void> MeasureClientStream(
      async2::CoroContext,
      Reader<SizeReportMessage> reader,
      UnaryWriter<SizeReportMessage> responder) {
    uint32_t sum = 0;
    while (true) {
      RpcResult<SizeReportMessage> msg = co_await reader.Read();
      if (msg.status().IsEndOfStream()) {
        break;
      }
      if (!msg.ok()) {
        co_return;
      }
      sum += msg->value;
    }
    co_await responder.Finish(SizeReportMessage{.value = sum});
  }

  async2::Coro<void> MeasureBidiStream(async2::CoroContext,
                                       Reader<SizeReportMessage> reader,
                                       Writer<SizeReportMessage> writer) {
    while (true) {
      RpcResult<SizeReportMessage> msg = co_await reader.Read();
      if (msg.status().IsEndOfStream()) {
        break;
      }
      if (!msg.ok()) {
        co_return;
      }
      RpcStatus status = co_await writer.Write(*msg);
      if (!status.ok()) {
        co_return;
      }
    }
    co_await writer.Finish();
  }
};

#endif

}  // namespace

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  PW_BLOAT_COND(SetBaseline(mask) == mask, mask);

  async2::BasicDispatcher dispatcher;
  Allocator& alloc = GetAllocator();

  Server server(alloc, dispatcher);

  MockListener mock_transport;
  PW_BLOAT_COND(server.RegisterListenerBlocking(mock_transport).ok(), mask);

#if defined(PW_RPC2_SIZE_REPORT_PWPB_BASE) ||               \
    defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_SERVICE) ||      \
    defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_CORO_SERVICE) || \
    defined(PW_RPC2_SIZE_REPORT_PWPB_FUTURE_SERVICE) ||     \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CORO_SERVICE)
  ExercisePwpbTableEncoderDecoder(mask);
#endif

  BaselineRawService base_service;
  PW_BLOAT_COND(server.RegisterService(base_service).ok(), mask);

#if defined(PW_RPC2_SIZE_REPORT_RAW_UNARY_SERVICE)
  RawUnaryService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_SERVICE)
  PwpbUnaryService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_UNARY_CORO_SERVICE) && \
    PW_CXX_STANDARD_IS_SUPPORTED(20)
  PwpbUnaryCoroService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#elif defined(PW_RPC2_SIZE_REPORT_RAW_FUTURE_SERVICE)
  RawFutureService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_FUTURE_SERVICE)
  PwpbFutureService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_CORO_SERVICE) && \
    PW_CXX_STANDARD_IS_SUPPORTED(20)
  PwpbCoroService service;
  PW_BLOAT_COND(server.RegisterService(service).ok(), mask);
#endif

  server.Start();

  static_cast<void>(server.Close());
  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

  return static_cast<int>(mask);
}

}  // namespace pw::rpc2::size_report

int main() { return pw::rpc2::size_report::Measure(); }
