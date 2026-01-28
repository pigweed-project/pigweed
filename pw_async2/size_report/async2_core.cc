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
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"

#ifdef _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER
#include "pw_async2/once_sender.h"
#endif  // _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER

#ifdef _PW_ASYNC2_SIZE_REPORT_COROUTINE
#include "pw_async2/coro.h"
#endif  // _PW_ASYNC2_SIZE_REPORT_COROUTINE

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}  // namespace

#ifdef _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER

OnceReceiver<uint32_t> SenderAdd(uint32_t a, uint32_t b) {
  auto [result_sender, result_receiver] = MakeOnceSenderAndReceiver<uint32_t>();
  result_sender.emplace(a + b);
  return std::move(result_receiver);
}

template <typename T>
class ReceiverTask : public Task {
 public:
  ReceiverTask(OnceReceiver<T>&& receiver) : receiver_(std::move(receiver)) {}

 private:
  Poll<> DoPend(Context& cx) override {
    PollResult<T> result = receiver_.Pend(cx);
    if (result.IsPending()) {
      return Pending();
    }

    return Ready();
  }

  OnceReceiver<T> receiver_;
};

#endif  // _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER

#ifdef _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER_INCREMENTAL

OnceReceiver<int32_t> SenderSub(int32_t a, int32_t b) {
  auto [result_sender, result_receiver] = MakeOnceSenderAndReceiver<int32_t>();
  result_sender.emplace(a - b);
  return std::move(result_receiver);
}

#endif  // _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER_INCREMENTAL

#ifdef _PW_ASYNC2_SIZE_REPORT_COROUTINE

class ExpectCoroTask final : public Task {
 public:
  ExpectCoroTask(Coro<pw::Status>&& coro) : coro_(std::move(coro)) {}

 private:
  Poll<> DoPend(Context& cx) final {
    Poll<Status> result = coro_.Pend(cx);
    if (result.IsPending()) {
      return Pending();
    }
    PW_CHECK_OK(*result);
    return Ready();
  }
  Coro<pw::Status> coro_;
};

Coro<Result<int>> ImmediatelyReturnsFive(CoroContext&) { co_return 5; }

Coro<Status> StoresFiveThenReturns(CoroContext& coro_cx, int& out) {
  PW_CO_TRY_ASSIGN(out, co_await ImmediatelyReturnsFive(coro_cx));
  co_return OkStatus();
}

class ObjectWithCoroMethod {
 public:
  ObjectWithCoroMethod(int x) : x_(x) {}
  Coro<Status> CoroMethodStoresField(CoroContext&, int& out) {
    out = x_;
    co_return OkStatus();
  }

 private:
  int x_;
};

#endif  // _PW_ASYNC2_SIZE_REPORT_COROUTINE

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  SetBaseline(mask);

  PendableInt value(47);

  MockTask task;
  dispatcher.Post(task);

#ifdef _PW_ASYNC2_SIZE_REPORT_INCREMENTAL_TASK

  MockTask task2;
  task2.should_complete = true;
  dispatcher.Post(task2);

#endif  // _PW_ASYNC2_SIZE_REPORT_INCREMENTAL_TASK

  dispatcher.RunUntilStalled();

  FuncTask task3([&](Context& cx) { return value.Get(cx).Readiness(); });
  dispatcher.Post(task3);
  dispatcher.RunUntilStalled();

  task.should_complete = true;
  // Move the waker onto the stack to call its operator= before waking the task.
  Waker waker;
  PW_BLOAT_EXPR((waker = std::move(task.last_waker)), mask);
  waker.Wake();
  dispatcher.RunToCompletion();

#ifdef _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER

  ReceiverTask add_receiver_task(SenderAdd(1, 2));
  dispatcher.Post(add_receiver_task);
  dispatcher.RunUntilStalled();

#endif  // _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER

#ifdef _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER_INCREMENTAL

  ReceiverTask sub_receiver_task(SenderSub(1, 2));
  dispatcher.Post(sub_receiver_task);
  dispatcher.RunUntilStalled();

#endif  // _PW_ASYNC2_SIZE_REPORT_ONCE_SENDER_INCREMENTAL

#ifdef _PW_ASYNC2_SIZE_REPORT_COROUTINE

  CoroContext coro_cx(GetAllocator());
  int output = 0;
  ExpectCoroTask coro_task = StoresFiveThenReturns(coro_cx, output);
  dispatcher.Post(coro_task);
  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);

#endif  // _PW_ASYNC2_SIZE_REPORT_COROUTINE

  return task.polled;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
