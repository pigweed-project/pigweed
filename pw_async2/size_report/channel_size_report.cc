// Copyright 2025 The Pigweed Authors
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

#include "public/pw_async2/size_report/size_report.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_bloat/bloat_this_binary.h"

namespace pw::async2::size_report {

template <typename T>
int MeasureStatic() {
  volatile uint32_t mask = bloat::kDefaultMask;
  SetBaseline(mask);

  BasicDispatcher dispatcher;

  ChannelStorage<T, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<T> sender = channel.CreateSender();
  Receiver<T> receiver = channel.CreateReceiver();
  channel.Release();

  PendFuncTask send_task([&](Context& cx) -> Poll<> {
    auto future = sender.Send(T());
    if (future.Pend(cx).IsReady()) {
      return Ready();
    }
    return Pending();
  });
  dispatcher.Post(send_task);

  PendFuncTask receive_task([&](Context& cx) -> Poll<> {
    auto future = receiver.Receive();
    if (future.Pend(cx).IsReady()) {
      return Ready();
    }
    return Pending();
  });
  dispatcher.Post(receive_task);

  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

  sender.Disconnect();
  receiver.Disconnect();

  return static_cast<int>(mask);
}

struct MoveOnly {
  MoveOnly() : val(0) {}
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;

  int val;
};

[[maybe_unused]] int MeasureDynamic() {
  volatile uint32_t mask = bloat::kDefaultMask;
  SetBaseline(mask);

  BasicDispatcher dispatcher;
  allocator::Allocator& alloc = GetAllocator();

  std::optional<MpmcChannelHandle<int>> channel =
      CreateMpmcChannel<int>(alloc, 2);

  if (channel.has_value()) {
    Sender<int> sender = channel->CreateSender();
    Receiver<int> receiver = channel->CreateReceiver();
    channel->Release();

    PendFuncTask send_task([&](Context& cx) -> Poll<> {
      auto future = sender.Send(1);
      if (future.Pend(cx).IsReady()) {
        return Ready();
      }
      return Pending();
    });
    dispatcher.Post(send_task);

    PendFuncTask receive_task([&](Context& cx) -> Poll<> {
      auto future = receiver.Receive();
      if (future.Pend(cx).IsReady()) {
        return Ready();
      }
      return Pending();
    });
    dispatcher.Post(receive_task);

    PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

    sender.Disconnect();
    receiver.Disconnect();
  }

  return static_cast<int>(mask);
}

}  // namespace pw::async2::size_report

int main() {
#if defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT)
  pw::async2::size_report::MeasureStatic<int>();
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT_AND_UINT)
  pw::async2::size_report::MeasureStatic<int>();
  pw::async2::size_report::MeasureStatic<unsigned>();
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT_AND_MOVE_ONLY)
  pw::async2::size_report::MeasureStatic<int>();
  pw::async2::size_report::MeasureStatic<pw::async2::size_report::MoveOnly>();
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_DYNAMIC_INT)
  pw::async2::size_report::MeasureDynamic();
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_DYNAMIC_STATIC_INT)
  pw::async2::size_report::MeasureDynamic();
  pw::async2::size_report::MeasureStatic<int>();
#else
#error "No size report macro was set!"
#endif
  return 0;
}
