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

#include "pw_async2/select.h"

#include "public/pw_async2/size_report/size_report.h"
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_log/log.h"

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT

int SingleTypeSelect(uint32_t mask) {
  ValueFuture<int> value_1 = ValueFuture<int>::Resolved(47);
  ValueFuture<int> value_2 = ValueFuture<int>::Resolved(52);
  ValueFuture<int> value_3 = ValueFuture<int>::Resolved(57);
  auto future =
      Select(std::move(value_1), std::move(value_2), std::move(value_3));

  int value = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(auto result, future.Pend(cx));

    if (result.has_value<0>()) {
      value = result.value<0>();
    } else if (result.has_value<1>()) {
      value = result.value<1>();
    } else if (result.has_value<2>()) {
      value = result.value<2>();
    }

    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  PW_BLOAT_COND(value != -1, mask);

  return value;
}

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL

int MultiTypeSelect(uint32_t mask) {
  ValueFuture<int> value_1 = ValueFuture<int>::Resolved(47);
  ValueFuture<uint32_t> value_2 = ValueFuture<uint32_t>::Resolved(0xffffffff);
  ValueFuture<char> value_3 = ValueFuture<char>::Resolved('c');
  auto future =
      Select(std::move(value_1), std::move(value_2), std::move(value_3));

  int value = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(auto result, future.Pend(cx));

    if (result.has_value<0>()) {
      value = result.value<0>();
    } else if (result.has_value<1>()) {
      value = static_cast<int>(result.value<1>());
    } else if (result.has_value<2>()) {
      value = static_cast<int>(result.value<2>());
    }

    return Ready();
  });
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  PW_BLOAT_COND(value != -1, mask);

  return value;
}

#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL
#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT

#ifdef _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL

class SelectComparisonTask : public Task {
 public:
  SelectComparisonTask()
      : value_1_(ValueFuture<int>::Resolved(47)),
        value_2_(ValueFuture<uint32_t>::Resolved(0xffffffff)),
        value_3_(ValueFuture<char>::Resolved('c')) {}

  // Implements the logic of Select() by manually polling each pendable.
  Poll<> DoPend(Context& cx) override {
    Poll<int> poll_1 = value_1_.Pend(cx);
    if (poll_1.IsReady()) {
      PW_LOG_INFO("Value 1 ready: %d", *poll_1);
      return Ready();
    }

    Poll<uint32_t> poll_2 = value_2_.Pend(cx);
    if (poll_2.IsReady()) {
      PW_LOG_INFO("Value 2 ready: %u", *poll_2);
      return Ready();
    }

    Poll<char> poll_3 = value_3_.Pend(cx);
    if (poll_3.IsReady()) {
      PW_LOG_INFO("Value 3 ready: %c", *poll_3);
      return Ready();
    }

    return Pending();
  }

 private:
  ValueFuture<int> value_1_;
  ValueFuture<uint32_t> value_2_;
  ValueFuture<char> value_3_;
};

#endif  // _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL

#ifdef _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER

class SelectComparisonTask : public Task {
 public:
  SelectComparisonTask()
      : value_1_(ValueFuture<int>::Resolved(47)),
        value_2_(ValueFuture<uint32_t>::Resolved(0xffffffff)),
        value_3_(ValueFuture<char>::Resolved('c')) {}

  // Calls Select() with the pendables.
  Poll<> DoPend(Context& cx) override {
    auto future =
        Select(std::move(value_1_), std::move(value_2_), std::move(value_3_));
    PW_TRY_READY_ASSIGN(auto result, future.Pend(cx));

    if (result.has_value<0>()) {
      PW_LOG_INFO("Value 1 ready: %d", result.value<0>());
    } else if (result.has_value<1>()) {
      PW_LOG_INFO("Value 2 ready: %u", result.value<1>());
    } else if (result.has_value<2>()) {
      PW_LOG_INFO("Value 3 ready: %c", result.value<2>());
    }
    return Ready();
  }

 private:
  ValueFuture<int> value_1_;
  ValueFuture<uint32_t> value_2_;
  ValueFuture<char> value_3_;
};

#endif  // _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER

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
  SetBaselineValueFutures(mask);

  MockTask task;
  dispatcher.Post(task);

  // Move the waker onto the stack to call its operator= before waking the task.
  Waker waker;
  PW_BLOAT_EXPR((waker = std::move(task.last_waker)), mask);
  waker.Wake();
  dispatcher.RunToCompletion();

  int result = -1;

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT
  result = SingleTypeSelect(mask);

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL
  result += MultiTypeSelect(mask);
#endif  //_PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL

#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT

#if defined(_PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL) || \
    defined(_PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER)
  ValueFuture<int> pendable_int = ValueFuture<int>::Resolved(47);
  FuncTask task2(
      [&](Context& cx) { return pendable_int.Pend(cx).Readiness(); });
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();

  SelectComparisonTask comparison;
  dispatcher.Post(comparison);
  dispatcher.RunUntilStalled();
#endif
  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
