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
#include "pw_rpc2/size_report/size_report.h"
#include "pw_rpc2/size_report/size_report.raw.rpc2.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::size_report {
namespace {

class ClientMeasurementTask : public async2::Task {
 public:
  explicit ClientMeasurementTask(ClientFuture fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsReady() && poll->ok()) {
      pw_rpc2::raw::BaselineSizeReport::Client client1(poll->value());
      auto call1 = client1.MeasureUnary::Copy(ConstBuf());
      (void)call1.Pend(cx);
#ifdef PW_RPC2_SIZE_REPORT_ALTERNATE_CALL
      pw_rpc2::raw::UnarySizeReport::Client client2(poll->value());
      auto call2 = client2.MeasureUnary::Copy(ConstBuf());
      (void)call2.Pend(cx);
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
