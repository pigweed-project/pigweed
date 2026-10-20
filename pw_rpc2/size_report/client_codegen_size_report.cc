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
#include <utility>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/pwpb_serialize.h"
#include "pw_rpc2/size_report/size_report.h"
#include "pw_rpc2/size_report/size_report.pwpb.rpc2.h"
#include "pw_rpc2/size_report/size_report.raw.rpc2.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::size_report {
namespace {

using SizeReportMessage = pwpb::SizeReportMessage::Message;
using RawSizeReportClient = pw_rpc2::raw::SizeReport::Client;
using PwpbSizeReportClient = pw_rpc2::pwpb::SizeReport::Client;

#if defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_BASE) ||  \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_UNARY) || \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_ALL)
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

class ClientMeasurementTask : public async2::Task {
 public:
  explicit ClientMeasurementTask(ClientFuture fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsReady() && poll->ok()) {
      pw_rpc2::raw::BaselineSizeReport::Client base_client(poll->value());
      auto call1 = base_client.MeasureUnary::Copy(ConstBuf());
      (void)call1.Pend(cx);

#if defined(PW_RPC2_SIZE_REPORT_RAW_CLIENT_UNARY)
      RawSizeReportClient client(poll->value());
      auto unary = client.MeasureUnary::Copy(ConstBuf());
      (void)unary.Pend(cx);
#elif defined(PW_RPC2_SIZE_REPORT_RAW_CLIENT_ALL)
      RawSizeReportClient client(poll->value());
      // `Method::Copy` resolves to the `CopyMethod` base class alias on
      // `ClientCopyMethods` (see `pw_rpc2/py/pw_rpc2/codegen_common.py`).
      auto unary = client.MeasureUnary::Copy(ConstBuf());
      (void)unary.Pend(cx);
      auto server_stream = client.MeasureServerStream::Copy(ConstBuf());
      (void)server_stream.Pend(cx);
      auto client_stream = client.MeasureClientStream();
      (void)client_stream.Pend(cx);
      auto bidi_stream = client.MeasureBidiStream();
      (void)bidi_stream.Pend(cx);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_UNARY)
      PwpbSizeReportClient client(poll->value());
      auto unary = client.MeasureUnary(SizeReportMessage{.value = 42});
      (void)unary.Pend(cx);
#elif defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_ALL)
      PwpbSizeReportClient client(poll->value());
      auto unary = client.MeasureUnary(SizeReportMessage{.value = 42});
      (void)unary.Pend(cx);
      auto server_stream =
          client.MeasureServerStream(SizeReportMessage{.value = 42});
      (void)server_stream.Pend(cx);
      auto client_stream = client.MeasureClientStream();
      (void)client_stream.Pend(cx);
      auto bidi_stream = client.MeasureBidiStream();
      (void)bidi_stream.Pend(cx);
#endif
    }
    return async2::Pending();
  }

 private:
  ClientFuture fut_;
};

}  // namespace

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  PW_BLOAT_COND(SetBaseline(mask) == mask, mask);

#if defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_BASE) ||  \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_UNARY) || \
    defined(PW_RPC2_SIZE_REPORT_PWPB_CLIENT_ALL)
  ExercisePwpbTableEncoderDecoder(mask);
#endif

  async2::BasicDispatcher dispatcher;
  Allocator& alloc = GetAllocator();

  MockConnection mock_conn(alloc);
  transport::ReliableDatagramSocket conn = MockListener::WrapSocket(mock_conn);
  auto fut = Client::Connect(dispatcher, alloc, std::move(conn));
  ClientMeasurementTask task(std::move(fut));
  dispatcher.Post(task);

  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

  task.Deregister();
  return static_cast<int>(mask);
}

}  // namespace pw::rpc2::size_report

int main() { return pw::rpc2::size_report::Measure(); }
