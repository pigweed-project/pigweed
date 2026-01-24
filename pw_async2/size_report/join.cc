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

#include "pw_async2/join.h"

#include "public/pw_async2/size_report/size_report.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_async2/value_future.h"
#include "pw_bloat/bloat_this_binary.h"

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}  // namespace

#ifdef _PW_ASYNC2_SIZE_REPORT_JOIN

int SingleTypeJoin(uint32_t mask) {
  ValueFuture<int> value_1 = ValueFuture<int>::Resolved(47);
  ValueFuture<int> value_2 = ValueFuture<int>::Resolved(52);
  ValueFuture<int> value_3 = ValueFuture<int>::Resolved(57);
  auto future =
      Join(std::move(value_1), std::move(value_2), std::move(value_3));
  decltype(future.Pend(std::declval<Context&>())) result = Pending();
  FuncTask task([&](Context& cx) {
    result = future.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  int value = -1;
  if (result.IsReady()) {
    value = std::get<0>(*result) + std::get<1>(*result) + std::get<2>(*result);
  }
  return value;
}

#ifdef _PW_ASYNC2_SIZE_REPORT_JOIN_INCREMENTAL

int MultiTypeJoin(uint32_t mask) {
  ValueFuture<int> value_1 = ValueFuture<int>::Resolved(47);
  ValueFuture<uint32_t> value_2 = ValueFuture<uint32_t>::Resolved(0x00ff00ffu);
  ValueFuture<char> value_3 = ValueFuture<char>::Resolved('c');
  auto future =
      Join(std::move(value_1), std::move(value_2), std::move(value_3));
  decltype(future.Pend(std::declval<Context&>())) result = Pending();
  FuncTask task([&](Context& cx) {
    result = future.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  int value = -1;
  if (result.IsReady()) {
    value = std::get<0>(*result) + static_cast<int>(std::get<1>(*result)) +
            static_cast<int>(std::get<2>(*result));
  }
  return value;
}

#endif  // _PW_ASYNC2_SIZE_REPORT_JOIN_INCREMENTAL
#endif  // _PW_ASYNC2_SIZE_REPORT_JOIN

// Ensure all ValueFuture types and their core operations (construct/move/Pend)
// are in the base binary.
void SetBaselineValueFutures(uint32_t mask) {
  ValueFuture<int> value_1 = ValueFuture<int>::Resolved(47);
  ValueFuture<uint32_t> value_2 = ValueFuture<uint32_t>::Resolved(0x00ff00ffu);
  ValueFuture<char> value_3 = ValueFuture<char>::Resolved('c');

  FuncTask task([v1 = std::move(value_1),
                 v2 = std::move(value_2),
                 v3 = std::move(value_3),
                 &mask](Context& cx) mutable -> Poll<> {
    auto result_1 = v1.Pend(cx);
    auto result_2 = v2.Pend(cx);
    auto result_3 = v3.Pend(cx);
    bool all_ready =
        result_1.IsReady() && result_2.IsReady() && result_3.IsReady();
    PW_BLOAT_COND(all_ready, mask);
    if (all_ready) {
      return Ready();
    }
    return Pending();
  });

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
}

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  SetBaseline(mask);

  MockTask task;
  dispatcher.Post(task);

  // Move the waker onto the stack to call its operator= before waking the task.
  Waker waker;
  PW_BLOAT_EXPR((waker = std::move(task.last_waker)), mask);
  waker.Wake();
  dispatcher.RunToCompletion();

  SetBaselineValueFutures(mask);

  int result = -1;

#ifdef _PW_ASYNC2_SIZE_REPORT_JOIN
  result = SingleTypeJoin(mask);

#ifdef _PW_ASYNC2_SIZE_REPORT_JOIN_INCREMENTAL
  result += MultiTypeJoin(mask);
#endif  //_PW_ASYNC2_SIZE_REPORT_JOIN_INCREMENTAL

#endif  // _PW_ASYNC2_SIZE_REPORT_JOIN

  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
