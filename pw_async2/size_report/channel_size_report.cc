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

#include <cstdint>
#include <optional>
#include <type_traits>

#include "pw_allocator/allocator.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/size_report/async2_baseline.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"

namespace pw::async2::size_report {

template <typename T>
int MeasureStatic(volatile uint32_t& mask) {
  BasicDispatcher& dispatcher = GetDispatcher();

  ChannelStorage<T, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<T> sender = channel.CreateSender();
  Receiver<T> receiver = channel.CreateReceiver();
  channel.Release();

  FuncTask send_task([&](Context& cx) -> Poll<> {
    if constexpr (std::is_void_v<T>) {
      auto future = sender.Send();
      if (future.Pend(cx).IsReady()) {
        return Ready();
      }
      return Pending();
    } else {
      auto future = sender.Send(T());
      if (future.Pend(cx).IsReady()) {
        return Ready();
      }
      return Pending();
    }
  });
  dispatcher.Post(send_task);

  FuncTask receive_task([&](Context& cx) -> Poll<> {
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

[[maybe_unused]] int MeasureDynamic(volatile uint32_t& mask) {
  BasicDispatcher& dispatcher = GetDispatcher();
  Allocator& alloc = GetAllocator();

  std::optional<MpmcChannelHandle<int>> channel =
      CreateMpmcChannel<int>(alloc, 2);

  if (channel.has_value()) {
    Sender<int> sender = channel->CreateSender();
    Receiver<int> receiver = channel->CreateReceiver();
    channel->Release();

    FuncTask send_task([&](Context& cx) -> Poll<> {
      auto future = sender.Send(1);
      if (future.Pend(cx).IsReady()) {
        return Ready();
      }
      return Pending();
    });
    dispatcher.Post(send_task);

    FuncTask receive_task([&](Context& cx) -> Poll<> {
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

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  int result = SetAsync2Baseline(mask);

#if defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT)
  result += MeasureStatic<int>(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT_AND_UINT)
  result += MeasureStatic<int>(mask);
  result += MeasureStatic<unsigned>(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT_AND_MOVE_ONLY)
  result += MeasureStatic<int>(mask);
  result += MeasureStatic<MoveOnly>(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_DYNAMIC_INT)
  result += MeasureDynamic(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_DYNAMIC_STATIC_INT)
  result += MeasureDynamic(mask);
  result += MeasureStatic<int>(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_NOTIFICATION)
  result += MeasureStatic<void>(mask);
#elif defined(PW_ASYNC2_CHANNEL_SIZE_REPORT_STATIC_INT_AND_NOTIFICATION)
  result += MeasureStatic<int>(mask);
  result += MeasureStatic<void>(mask);
#else
#error "No size report macro was set!"
#endif
  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
