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

#include "pw_async2/value_future.h"

#include "public/pw_async2/size_report/size_report.h"
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}  // namespace

#ifdef _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE

int SingleTypeValueFuture(uint32_t mask) {
  ValueProvider<int> provider;
  ValueFuture<int> future = provider.Get();

  Poll<int> result = Pending();
  PendFuncTask task([&](Context& cx) {
    result = future.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsPending(), mask);

  provider.Resolve(42);

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  int value = -1;
  if (result.IsReady()) {
    value = *result;
  }

  return value;
}

#ifdef _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE_INCREMENTAL

int MultiTypeValueFuture(uint32_t mask) {
  ValueProvider<float> provider;
  ValueFuture<float> future = provider.Get();

  Poll<float> result = Pending();
  PendFuncTask task([&](Context& cx) {
    result = future.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsPending(), mask);

  provider.Resolve(3.14f);

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  int value = -1;
  if (result.IsReady()) {
    value = static_cast<int>(*result);
  }

  return value;
}

#endif  // _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE_INCREMENTAL
#endif  // _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE

#ifdef _PW_ASYNC2_SIZE_REPORT_VOID_FUTURE

int VoidFutureReport(uint32_t mask) {
  ValueProvider<void> provider;
  ValueFuture<void> future = provider.Get();

  Poll<> result = Pending();
  PendFuncTask task([&](Context& cx) {
    result = future.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsPending(), mask);

  provider.Resolve();

  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  return result.IsReady() ? 1 : 0;
}

#endif  // _PW_ASYNC2_SIZE_REPORT_VOID_FUTURE

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  SetBaseline(mask);

  MockTask task;
  dispatcher.Post(task);

  Waker waker;
  PW_BLOAT_EXPR((waker = std::move(task.last_waker)), mask);
  waker.Wake();
  dispatcher.RunToCompletion();

  int result = -1;

#ifdef _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE
  result = SingleTypeValueFuture(mask);

#ifdef _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE_INCREMENTAL
  result += MultiTypeValueFuture(mask);
#endif  // _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE_INCREMENTAL

#endif  // _PW_ASYNC2_SIZE_REPORT_VALUE_FUTURE

#ifdef _PW_ASYNC2_SIZE_REPORT_VOID_FUTURE
  result = VoidFutureReport(mask);
#endif  // _PW_ASYNC2_SIZE_REPORT_VOID_FUTURE

  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
