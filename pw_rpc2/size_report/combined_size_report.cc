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

#include <utility>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_buf/buf.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/size_report/size_report.h"
#include "pw_rpc2/size_report/size_report.raw.rpc2.h"
#include "pw_rpc2/writer.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::size_report {
namespace {

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

class ClientMeasurementTask : public async2::Task {
 public:
  explicit ClientMeasurementTask(ClientFuture fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsReady() && poll->ok()) {
      pw_rpc2::raw::BaselineSizeReport::Client client(poll->value());
      auto call1 = client.MeasureUnary::Copy(ConstBuf());
      (void)call1.Pend(cx);
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

  async2::BasicDispatcher dispatcher;
  Allocator& alloc = GetAllocator();

  // 1. Initialize Server with 1 transport and 1 raw unary service.
  Server server(alloc, dispatcher);
  MockListener server_transport;
  PW_BLOAT_COND(server.RegisterListenerBlocking(server_transport).ok(), mask);
  BaselineRawService service1;
  PW_BLOAT_COND(server.RegisterService(service1).ok(), mask);
  server.Start();

  // 2. Initialize Client with 1 connection and 1 raw unary call.
  MockConnection mock_conn(alloc);
  transport::ReliableDatagramSocket conn = MockListener::WrapSocket(mock_conn);
  auto fut = Client::Connect(dispatcher, alloc, std::move(conn));
  ClientMeasurementTask task(std::move(fut));
  dispatcher.Post(task);

  static_cast<void>(server.Close());
  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

  task.Deregister();
  return static_cast<int>(mask);
}

}  // namespace pw::rpc2::size_report

int main() { return pw::rpc2::size_report::Measure(); }
