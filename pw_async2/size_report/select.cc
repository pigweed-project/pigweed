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
#include "pw_async2/pendable.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_log/log.h"

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT

int SingleTypeSelect(uint32_t mask) {
  PendableInt value_1(47);
  PendableInt value_2(52);
  PendableInt value_3(57);
  Selector selector(PendableFor<&PendableInt::Get>(value_1),
                    PendableFor<&PendableInt::Get>(value_2),
                    PendableFor<&PendableInt::Get>(value_3));
  decltype(selector.Pend(std::declval<Context&>())) result = Pending();
  FuncTask task([&](Context& cx) {
    result = selector.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  value_1.allow_completion = true;

  bool completed = false;
  int value = -1;

  VisitSelectResult(
      *result,
      [&](AllPendablesCompleted) { completed = true; },
      [&](int v) {
        value = v;  // value_1
      },
      [&](int v) {
        value = v;  // value_2
      },
      [&](int v) {
        value = v;  // value_3
      });

  return value;
}

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL

int MultiTypeSelect(uint32_t mask) {
  PendableInt value_1(47);
  PendableUint value_2(0xffffffff);
  PendableChar value_3('c');
  Selector selector(PendableFor<&PendableInt::Get>(value_1),
                    PendableFor<&PendableUint::Get>(value_2),
                    PendableFor<&PendableChar::Get>(value_3));
  decltype(selector.Pend(std::declval<Context&>())) result = Pending();
  FuncTask task([&](Context& cx) {
    result = selector.Pend(cx);
    return result.Readiness();
  });
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  PW_BLOAT_COND(result.IsReady(), mask);

  value_3.allow_completion = true;

  bool completed = false;
  int value = -1;

  VisitSelectResult(
      *result,
      [&](AllPendablesCompleted) { completed = true; },
      [&](int i) { value = i; },
      [&](unsigned int u) { value = static_cast<int>(u); },
      [&](char c) { value = static_cast<int>(c); });

  return value;
}

#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL
#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT

#ifdef _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL

class SelectComparison {
 public:
  SelectComparison() : value_1_(47), value_2_(0xffffffff), value_3_('c') {}

  // Implements the logic of Select() by manually polling each pendable.
  Poll<> Pend(Context& cx) {
    Poll<int> poll_1 = value_1_.Get(cx);
    if (poll_1.IsReady()) {
      PW_LOG_INFO("Value 1 ready: %d", *poll_1);
      return Ready();
    }

    Poll<unsigned int> poll_2 = value_2_.Get(cx);
    if (poll_2.IsReady()) {
      PW_LOG_INFO("Value 2 ready: %u", *poll_2);
      return Ready();
    }

    Poll<char> poll_3 = value_3_.Get(cx);
    if (poll_3.IsReady()) {
      PW_LOG_INFO("Value 3 ready: %c", *poll_3);
      return Ready();
    }

    return Pending();
  }

 private:
  PendableInt value_1_;
  PendableUint value_2_;
  PendableChar value_3_;
};

#endif  // _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL

#ifdef _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER

class SelectComparison {
 public:
  SelectComparison() : value_1_(47), value_2_(0xffffffff), value_3_('c') {}

  // Calls Select() with the pendables.
  Poll<> Pend(Context& cx) {
    Selector selector(PendableFor<&PendableInt::Get>(value_1_),
                      PendableFor<&PendableUint::Get>(value_2_),
                      PendableFor<&PendableChar::Get>(value_3_));
    auto result = selector.Pend(cx);
    if (result.IsPending()) {
      return Pending();
    }

    VisitSelectResult(
        *result,
        [](AllPendablesCompleted) {},
        [](int i) { PW_LOG_INFO("Value 1 ready: %d", i); },
        [](unsigned int u) { PW_LOG_INFO("Value 2 ready: %u", u); },
        [](char c) { PW_LOG_INFO("Value 3 ready: %c", c); });
    return Ready();
  }

 private:
  PendableInt value_1_;
  PendableUint value_2_;
  PendableChar value_3_;
};

#endif  // _PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER

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

  int result = -1;

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT
  result = SingleTypeSelect(mask);

#ifdef _PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL
  result += MultiTypeSelect(mask);
#endif  //_PW_ASYNC2_SIZE_REPORT_SELECT_INCREMENTAL

#endif  // _PW_ASYNC2_SIZE_REPORT_SELECT

#if defined(_PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_MANUAL) || \
    defined(_PW_ASYNC_2_SIZE_REPORT_COMPARE_SELECT_HELPER)
  PendableInt pendable_int(47);
  FuncTask task2([&](Context& cx) { return pendable_int.Get(cx).Readiness(); });
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();

  SelectComparison comparison;
  FuncTask task3([&](Context& cx) { return comparison.Pend(cx).Readiness(); });
  dispatcher.Post(task3);
  dispatcher.RunUntilStalled();
#endif

  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
