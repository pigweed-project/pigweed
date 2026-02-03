// Copyright 2024 The Pigweed Authors
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

#include "pw_clock_tree/clock_tree.h"

#include "pw_preprocessor/util.h"
#include "pw_unit_test/framework.h"

namespace pw::clock_tree {
namespace {

#define INIT_TEST_DATA(test_data, call_data)               \
  test_data.num_expected_calls = PW_ARRAY_SIZE(call_data); \
  test_data.num_calls = 0;                                 \
  test_data.data = call_data

enum class ClockOperation {
  kAcquire,
  kRelease,
};

struct clock_divider_test_call_data {
  uint32_t divider_name;
  uint32_t divider;
  ClockOperation op;
  pw::Status status;
};

struct clock_divider_test_data {
  uint32_t num_expected_calls;
  uint32_t num_calls;
  struct clock_divider_test_call_data* data;
};

template <typename ElementType>
class ClockDividerTest : public ClockDividerElement<ElementType> {
 public:
  constexpr ClockDividerTest(ElementType& source,
                             uint32_t divider_name,
                             uint32_t divider,
                             struct clock_divider_test_data& test_data)
      : ClockDividerElement<ElementType>(source, divider),
        divider_name_(divider_name),
        test_data_(test_data) {}

 private:
  pw::Status ValidateClockAction(ClockOperation op) {
    pw::Status status = pw::Status::OutOfRange();
    if (test_data_.num_calls < test_data_.num_expected_calls) {
      uint32_t i = test_data_.num_calls;
      EXPECT_EQ(test_data_.data[i].divider_name, divider_name_);
      EXPECT_EQ(test_data_.data[i].divider, this->divider());
      EXPECT_EQ(test_data_.data[i].op, op);
      status = test_data_.data[i].status;
    }
    test_data_.num_calls++;
    return status;
  }

  pw::Status DoEnable() final {
    return ValidateClockAction(ClockOperation::kAcquire);
  }
  pw::Status DoDisable() final {
    return ValidateClockAction(ClockOperation::kRelease);
  }

  uint32_t divider_name_;
  struct clock_divider_test_data& test_data_;
};

using ClockDividerTestBlocking = ClockDividerTest<ElementBlocking>;
using ClockDividerTestNonBlocking =
    ClockDividerTest<ElementNonBlockingMightFail>;

template <typename ElementType>
class ClockDividerNoDoDisableTest : public ClockDividerElement<ElementType> {
 public:
  constexpr ClockDividerNoDoDisableTest(
      ElementType& source,
      uint32_t divider_name,
      uint32_t divider,
      struct clock_divider_test_data& test_data)
      : ClockDividerElement<ElementType>(source, divider),
        divider_name_(divider_name),
        test_data_(test_data) {}

 private:
  pw::Status ValidateClockAction(ClockOperation op) {
    pw::Status status = pw::Status::OutOfRange();
    if (test_data_.num_calls < test_data_.num_expected_calls) {
      uint32_t i = test_data_.num_calls;
      EXPECT_EQ(test_data_.data[i].divider_name, divider_name_);
      EXPECT_EQ(test_data_.data[i].divider, this->divider());
      EXPECT_EQ(test_data_.data[i].op, op);
      status = test_data_.data[i].status;
    }
    test_data_.num_calls++;
    return status;
  }

  pw::Status DoEnable() final {
    return ValidateClockAction(ClockOperation::kAcquire);
  }

  uint32_t divider_name_;
  struct clock_divider_test_data& test_data_;
};
using ClockDividerNoDoDisableTestBlocking =
    ClockDividerNoDoDisableTest<ElementBlocking>;
using ClockDividerNoDoDisableTestNonBlocking =
    ClockDividerNoDoDisableTest<ElementNonBlockingMightFail>;

struct clock_selector_test_call_data {
  uint32_t selector;
  uint32_t value;
  ClockOperation op;
  pw::Status status;
};

struct clock_selector_test_data {
  uint32_t num_expected_calls;
  uint32_t num_calls;
  struct clock_selector_test_call_data* data;
};

template <typename ElementType>
class ClockSelectorTest : public DependentElement<ElementType> {
 public:
  constexpr ClockSelectorTest(ElementType& source,
                              uint32_t selector,
                              uint32_t selector_enable,
                              uint32_t selector_disable,
                              struct clock_selector_test_data& test_data)
      : DependentElement<ElementType>(source),
        selector_(selector),
        selector_enable_(selector_enable),
        selector_disable_(selector_disable),
        test_data_(test_data) {}

 private:
  pw::Status ValidateClockAction(ClockOperation op) {
    pw::Status status = pw::Status::OutOfRange();
    if (test_data_.num_calls < test_data_.num_expected_calls) {
      uint32_t i = test_data_.num_calls;
      uint32_t value = (op == ClockOperation::kAcquire) ? selector_enable_
                                                        : selector_disable_;
      EXPECT_EQ(test_data_.data[i].selector, selector_);
      EXPECT_EQ(test_data_.data[i].value, value);
      EXPECT_EQ(test_data_.data[i].op, op);
      status = test_data_.data[i].status;
    }
    test_data_.num_calls++;
    return status;
  }
  pw::Status DoEnable() final {
    return ValidateClockAction(ClockOperation::kAcquire);
  }
  pw::Status DoDisable() final {
    return ValidateClockAction(ClockOperation::kRelease);
  }

  uint32_t selector_;
  uint32_t selector_enable_;
  uint32_t selector_disable_;
  struct clock_selector_test_data& test_data_;
};

using ClockSelectorTestBlocking = ClockSelectorTest<ElementBlocking>;
using ClockSelectorTestNonBlockingMightFail =
    ClockSelectorTest<ElementNonBlockingMightFail>;

struct clock_source_state_test_call_data {
  uint32_t value;
  ClockOperation op;
  pw::Status status;
};

struct clock_source_state_test_data {
  uint32_t num_expected_calls;
  uint32_t num_calls;
  struct clock_source_state_test_call_data* data;
};

template <typename ElementType>
class ClockSourceStateTest : public ClockSource<ElementType> {
 public:
  constexpr ClockSourceStateTest(uint32_t value,
                                 uint32_t* clock_state,
                                 struct clock_source_state_test_data& test_data)
      : value_(value), clock_state_(clock_state), test_data_(test_data) {}

 private:
  pw::Status ValidateClockAction(ClockOperation op) {
    pw::Status status = pw::Status::OutOfRange();
    if (test_data_.num_calls < test_data_.num_expected_calls) {
      uint32_t i = test_data_.num_calls;
      EXPECT_EQ(test_data_.data[i].value, value_);
      EXPECT_EQ(test_data_.data[i].op, op);
      status = test_data_.data[i].status;
    }
    test_data_.num_calls++;
    return status;
  }

  pw::Status DoEnable() final {
    PW_TRY(ValidateClockAction(ClockOperation::kAcquire));
    *clock_state_ |= value_;
    return pw::OkStatus();
  }

  pw::Status DoDisable() final {
    PW_TRY(ValidateClockAction(ClockOperation::kRelease));
    *clock_state_ &= ~value_;
    return pw::OkStatus();
  }

  uint32_t value_;
  uint32_t* clock_state_;
  struct clock_source_state_test_data& test_data_;
};
using ClockSourceStateTestBlocking = ClockSourceStateTest<ElementBlocking>;
using ClockSourceStateTestNonBlocking =
    ClockSourceStateTest<ElementNonBlockingMightFail>;

template <typename ElementType>
class ClockSourceTest : public ClockSource<ElementType> {
 private:
  pw::Status DoEnable() final { return pw::OkStatus(); }

  pw::Status DoDisable() final { return pw::OkStatus(); }
};
using ClockSourceTestBlocking = ClockSourceTest<ElementBlocking>;
using ClockSourceTestNonBlocking = ClockSourceTest<ElementNonBlockingMightFail>;

struct clock_source_failure_test_call_data {
  ClockOperation op;
  pw::Status status;
};

struct clock_source_failure_test_data {
  uint32_t num_expected_calls;
  uint32_t num_calls;
  struct clock_source_failure_test_call_data* data;
};

template <typename ElementType>
class ClockSourceFailureTest : public ClockSource<ElementType> {
 public:
  constexpr ClockSourceFailureTest(
      struct clock_source_failure_test_data& test_data)
      : test_data_(test_data) {}

 private:
  pw::Status ValidateClockAction(ClockOperation op) {
    pw::Status status = pw::Status::OutOfRange();
    if (test_data_.num_calls < test_data_.num_expected_calls) {
      uint32_t i = test_data_.num_calls;
      EXPECT_EQ(test_data_.data[i].op, op);
      status = test_data_.data[i].status;
    }
    test_data_.num_calls++;
    return status;
  }

  pw::Status DoEnable() final {
    return ValidateClockAction(ClockOperation::kAcquire);
  }
  pw::Status DoDisable() final {
    return ValidateClockAction(ClockOperation::kRelease);
  }
  struct clock_source_failure_test_data& test_data_;
};

using ClockSourceFailureTestBlocking = ClockSourceFailureTest<ElementBlocking>;
using ClockSourceFailureTestNonBlocking =
    ClockSourceFailureTest<ElementNonBlockingMightFail>;

template <typename ElementType>
static void TestClock() {
  pw::Status status;
  ClockSourceTest<ElementType> clock_a;

  EXPECT_EQ(clock_a.ref_count(), 0u);

  status = clock_a.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);

  status = clock_a.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 2u);

  status = clock_a.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);

  status = clock_a.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
}

TEST(ClockTree, ClockBlocking) { TestClock<ElementBlocking>(); }

TEST(ClockTree, ClockNonBlocking) { TestClock<ElementNonBlockingMightFail>(); }

// Validate that the correct divider values are getting set.
// The `clock_divider_b` doesn't override the `DoDisable` function,
// so only the ClockDividerNoDoDisableTest's `DoEnable` method will be called.
template <typename ElementType>
static void TestClockDivider() {
  const uint32_t kClockDividerB = 23;
  const uint32_t kClockDividerC = 42;

  struct clock_divider_test_call_data call_data[] = {
      {kClockDividerB, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerC, 4, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerC, 4, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_divider_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceTest<ElementType> clock_a;
  ClockDividerNoDoDisableTest<ElementType> clock_divider_b(
      clock_a, kClockDividerB, 2, test_data);
  ClockDividerTest<ElementType> clock_divider_c(
      clock_a, kClockDividerC, 4, test_data);
  ClockDivider& clock_divider_b_abstract = clock_divider_b;
  Element& clock_divider_b_element = clock_divider_b_abstract.element();
  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  status = clock_divider_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  status = clock_divider_b_element.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 2u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  status = clock_divider_c.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 2u);
  EXPECT_EQ(clock_divider_c.ref_count(), 1u);

  status = clock_divider_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);
  EXPECT_EQ(clock_divider_c.ref_count(), 1u);

  // Releasing `clock_divider_b` won't be tracked, since
  // only the base class `DoDisable` method will be called.
  status = clock_divider_b_element.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 1u);

  status = clock_divider_c.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, DividerBlocking) { TestClockDivider<ElementBlocking>(); }

TEST(ClockTree, DividerNonBlocking) {
  TestClockDivider<ElementNonBlockingMightFail>();
}

// Validate that different divider values can be set.
template <typename ElementType>
static void TestClockDividerSet() {
  const uint32_t kClockDivider = 23;

  struct clock_divider_test_call_data call_data[] = {
      {kClockDivider, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDivider, 4, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDivider, 4, ClockOperation::kRelease, pw::OkStatus()},
      {kClockDivider, 6, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDivider, 6, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_divider_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);
  pw::Status status;

  ClockSourceTest<ElementType> clock_a;
  ClockDividerTest<ElementType> clock_divider_b(
      clock_a, kClockDivider, 2, test_data);
  ClockDivider& clock_divider_b_abstract = clock_divider_b;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b_abstract.SetDivider(4);
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_b.SetDivider(6);
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, ClockDividerSetBlocking) {
  TestClockDividerSet<ElementBlocking>();
}

TEST(ClockTree, ClockDividerSetNonBlocking) {
  TestClockDividerSet<ElementNonBlockingMightFail>();
}

// Validate that if the `DoEnable` function fails that gets called as part
// of a divider update, that the state of the divider doesn't change.
template <typename ElementType>
static void TestClockDividerSetFailure() {
  const uint32_t kClockDivider = 23;

  struct clock_divider_test_call_data call_data[] = {
      {kClockDivider, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDivider, 4, ClockOperation::kAcquire, pw::Status::Internal()},
      {kClockDivider, 2, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_divider_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);
  pw::Status status;

  ClockSourceTest<ElementType> clock_a;
  ClockDividerTest<ElementType> clock_divider_b(
      clock_a, kClockDivider, 2, test_data);

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b.SetDivider(4);
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, ClockDividerSetFailureBlocking) {
  TestClockDividerSetFailure<ElementBlocking>();
}

TEST(ClockTree, ClockDividerSetFailureNonBlocking) {
  TestClockDividerSetFailure<ElementNonBlockingMightFail>();
}

// Validate that a selector enables and disables correctly.
template <typename ElementType>
static void TestClockSelector() {
  const uint32_t kSelector = 41;
  struct clock_selector_test_call_data call_data[] = {
      {kSelector, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kSelector, 7, ClockOperation::kRelease, pw::OkStatus()},
      {kSelector, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kSelector, 7, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_selector_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);
  pw::Status status;

  ClockSourceTest<ElementType> clock_a;
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 2, 7, test_data);
  Element& clock_selector_b_element = clock_selector_b;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  status = clock_selector_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  status = clock_selector_b_element.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 2u);

  status = clock_selector_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  status = clock_selector_b_element.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  status = clock_selector_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  status = clock_selector_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, ClockSelectorBlocking) { TestClockSelector<ElementBlocking>(); }

TEST(ClockTree, ClockSelectorNonBlocking) {
  TestClockSelector<ElementNonBlockingMightFail>();
}

template <typename ElementType>
static void TestClockSource() {
  uint32_t shared_clock_value = 0;
  uint32_t exclusive_clock_value = 0;

  struct clock_source_state_test_call_data call_data[] = {
      {1, ClockOperation::kAcquire, pw::OkStatus()},
      {4, ClockOperation::kAcquire, pw::OkStatus()},
      {2, ClockOperation::kAcquire, pw::OkStatus()},
      {1, ClockOperation::kRelease, pw::OkStatus()},
      {2, ClockOperation::kRelease, pw::OkStatus()},
      {4, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_source_state_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);
  pw::Status status;

  ClockSourceStateTest<ElementType> clock_a(1, &shared_clock_value, test_data);
  ClockSourceStateTest<ElementType> clock_b(2, &shared_clock_value, test_data);
  ClockSourceStateTest<ElementType> clock_c(
      4, &exclusive_clock_value, test_data);
  Element& clock_c_element = clock_c;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_b.ref_count(), 0u);
  EXPECT_EQ(clock_c.ref_count(), 0u);
  EXPECT_EQ(shared_clock_value, 0u);
  EXPECT_EQ(exclusive_clock_value, 0u);

  status = clock_a.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_b.ref_count(), 0u);
  EXPECT_EQ(clock_c.ref_count(), 0u);
  EXPECT_EQ(shared_clock_value, 1u);
  EXPECT_EQ(exclusive_clock_value, 0u);

  status = clock_c_element.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_b.ref_count(), 0u);
  EXPECT_EQ(clock_c.ref_count(), 1u);
  EXPECT_EQ(shared_clock_value, 1u);
  EXPECT_EQ(exclusive_clock_value, 4u);

  status = clock_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_b.ref_count(), 1u);
  EXPECT_EQ(clock_c.ref_count(), 1u);
  EXPECT_EQ(shared_clock_value, 3u);
  EXPECT_EQ(exclusive_clock_value, 4u);

  status = clock_a.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_b.ref_count(), 1u);
  EXPECT_EQ(clock_c.ref_count(), 1u);
  EXPECT_EQ(shared_clock_value, 2u);
  EXPECT_EQ(exclusive_clock_value, 4u);

  status = clock_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_b.ref_count(), 0u);
  EXPECT_EQ(clock_c.ref_count(), 1u);
  EXPECT_EQ(shared_clock_value, 0u);
  EXPECT_EQ(exclusive_clock_value, 4u);

  status = clock_c_element.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_b.ref_count(), 0u);
  EXPECT_EQ(clock_c.ref_count(), 0u);
  EXPECT_EQ(shared_clock_value, 0u);
  EXPECT_EQ(exclusive_clock_value, 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, ClockSourceBlocking) { TestClockSource<ElementBlocking>(); }

TEST(ClockTree, ClockSourceNonBlocking) {
  TestClockSource<ElementNonBlockingMightFail>();
}

// Validate that no references have been acquired when ClockSource
// fails in `DoEnable`.
template <typename ElementType>
static void TestFailureAcquire1() {
  struct clock_source_failure_test_call_data clock_call_data[] = {
      {ClockOperation::kAcquire, pw::Status::Internal()}};

  struct clock_source_failure_test_data clock_test_data;
  INIT_TEST_DATA(clock_test_data, clock_call_data);
  ClockSourceFailureTest<ElementType> clock_a(clock_test_data);

  const uint32_t kSelector = 41;
  struct clock_selector_test_data selector_test_data = {};
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 1, 8, selector_test_data);

  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  status = clock_selector_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  EXPECT_EQ(clock_test_data.num_calls, clock_test_data.num_expected_calls);
  EXPECT_EQ(selector_test_data.num_calls,
            selector_test_data.num_expected_calls);
}

TEST(ClockTree, ClockFailureAcquire1Blocking) {
  TestFailureAcquire1<ElementBlocking>();
}

TEST(ClockTree, ClockFailureAcquire1NonBlocking) {
  TestFailureAcquire1<ElementNonBlockingMightFail>();
}

// Validate that `ClockSource` reference gets released correctly, when
// dependent clock element fails to enable in `DoEnable`, and that
// `DependentElement` doesn't get enabled if dependent
// clock tree element doesn't get enabled successfully.
template <typename ElementType>
static void TestFailureAcquire2() {
  struct clock_source_failure_test_call_data clock_call_data[] = {
      {ClockOperation::kAcquire, pw::OkStatus()},
      {ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_source_failure_test_data clock_test_data;
  INIT_TEST_DATA(clock_test_data, clock_call_data);
  ClockSourceFailureTest<ElementType> clock_a(clock_test_data);

  const uint32_t kSelector = 41;
  struct clock_selector_test_call_data selector_call_data[] = {
      {kSelector, 1, ClockOperation::kAcquire, pw::Status::Internal()}};

  struct clock_selector_test_data selector_test_data;
  INIT_TEST_DATA(selector_test_data, selector_call_data);
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 1, 8, selector_test_data);

  const uint32_t kClockDividerC = 42;
  struct clock_divider_test_data divider_test_data = {};
  ClockDividerTest<ElementType> clock_divider_c(
      clock_selector_b, kClockDividerC, 4, divider_test_data);

  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  status = clock_divider_c.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  EXPECT_EQ(clock_test_data.num_calls, clock_test_data.num_expected_calls);
  EXPECT_EQ(selector_test_data.num_calls,
            selector_test_data.num_expected_calls);
  EXPECT_EQ(divider_test_data.num_calls, divider_test_data.num_expected_calls);
}

TEST(ClockTree, ClockFailureAcquire2Blocking) {
  TestFailureAcquire2<ElementBlocking>();
}

TEST(ClockTree, ClockFailureAcquire2NonBlocking) {
  TestFailureAcquire2<ElementNonBlockingMightFail>();
}

// Validate that `ClockSource` and `DependentElement` references
// gets released correctly, when dependent clock element fails to enable
// in `DoEnable`.
template <typename ElementType>
static void TestFailureAcquire3() {
  struct clock_source_failure_test_call_data clock_call_data[] = {
      {ClockOperation::kAcquire, pw::OkStatus()},
      {ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_source_failure_test_data clock_test_data;
  INIT_TEST_DATA(clock_test_data, clock_call_data);
  ClockSourceFailureTest<ElementType> clock_a(clock_test_data);

  const uint32_t kSelector = 41;
  struct clock_selector_test_call_data selector_call_data[] = {
      {kSelector, 1, ClockOperation::kAcquire, pw::OkStatus()},
      {kSelector, 8, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_selector_test_data selector_test_data;
  INIT_TEST_DATA(selector_test_data, selector_call_data);
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 1, 8, selector_test_data);

  const uint32_t kClockDividerC = 42;
  struct clock_divider_test_call_data divider_call_data[] = {
      {kClockDividerC, 4, ClockOperation::kAcquire, pw::Status::Internal()}};

  struct clock_divider_test_data divider_test_data;
  INIT_TEST_DATA(divider_test_data, divider_call_data);
  ClockDividerTest<ElementType> clock_divider_c(
      clock_selector_b, kClockDividerC, 4, divider_test_data);

  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  status = clock_divider_c.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);
  EXPECT_EQ(clock_divider_c.ref_count(), 0u);

  EXPECT_EQ(clock_test_data.num_calls, clock_test_data.num_expected_calls);
  EXPECT_EQ(selector_test_data.num_calls,
            selector_test_data.num_expected_calls);
  EXPECT_EQ(divider_test_data.num_calls, divider_test_data.num_expected_calls);
}

TEST(ClockTree, ClockFailureAcquire3Blocking) {
  TestFailureAcquire3<ElementBlocking>();
}

TEST(ClockTree, ClockFailureAcquire3NonBlocking) {
  TestFailureAcquire3<ElementNonBlockingMightFail>();
}

// Validate that reference counts are correct when a ClockSource derived class
// fails in `DoDisable` during `Release.
template <typename ElementType>
static void TestFailureRelease1() {
  struct clock_source_failure_test_call_data clock_call_data[] = {
      {ClockOperation::kAcquire, pw::OkStatus()},
      {ClockOperation::kRelease, pw::Status::Internal()}};

  struct clock_source_failure_test_data clock_test_data;
  INIT_TEST_DATA(clock_test_data, clock_call_data);
  ClockSourceFailureTest<ElementType> clock_a(clock_test_data);

  const uint32_t kSelector = 41;
  struct clock_selector_test_call_data selector_call_data[] = {
      {kSelector, 1, ClockOperation::kAcquire, pw::OkStatus()},
      {kSelector, 8, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_selector_test_data selector_test_data;
  INIT_TEST_DATA(selector_test_data, selector_call_data);
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 1, 8, selector_test_data);

  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  // Acquire initial references
  status = clock_selector_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  status = clock_selector_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  EXPECT_EQ(clock_test_data.num_calls, clock_test_data.num_expected_calls);
  EXPECT_EQ(selector_test_data.num_calls,
            selector_test_data.num_expected_calls);
}

TEST(ClockTree, ClockFailureRelease1Blocking) {
  TestFailureRelease1<ElementBlocking>();
}

TEST(ClockTree, ClockFailureRelease1NonBlocking) {
  TestFailureRelease1<ElementNonBlockingMightFail>();
}

// Validate that the reference is kept alive if a `DoDisable` call
// fails when releasing a reference for a DependentElement derived
// class.
template <typename ElementType>
static void TestFailureRelease2() {
  struct clock_source_failure_test_call_data clock_call_data[] = {
      {ClockOperation::kAcquire, pw::OkStatus()}};

  struct clock_source_failure_test_data clock_test_data;
  INIT_TEST_DATA(clock_test_data, clock_call_data);
  ClockSourceFailureTest<ElementType> clock_a(clock_test_data);

  const uint32_t kSelector = 41;
  struct clock_selector_test_call_data selector_call_data[] = {
      {kSelector, 1, ClockOperation::kAcquire, pw::OkStatus()},
      {kSelector, 8, ClockOperation::kRelease, pw::Status::Internal()}};

  struct clock_selector_test_data selector_test_data;
  INIT_TEST_DATA(selector_test_data, selector_call_data);
  ClockSelectorTest<ElementType> clock_selector_b(
      clock_a, kSelector, 1, 8, selector_test_data);

  pw::Status status;

  EXPECT_EQ(clock_a.ref_count(), 0u);
  EXPECT_EQ(clock_selector_b.ref_count(), 0u);

  status = clock_selector_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  status = clock_selector_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_a.ref_count(), 1u);
  EXPECT_EQ(clock_selector_b.ref_count(), 1u);

  EXPECT_EQ(clock_test_data.num_calls, clock_test_data.num_expected_calls);
  EXPECT_EQ(selector_test_data.num_calls,
            selector_test_data.num_expected_calls);
}

TEST(ClockTree, ClockFailureRelease2Blocking) {
  TestFailureRelease2<ElementBlocking>();
}

TEST(ClockTree, ClockFailureRelease2NonBlocking) {
  TestFailureRelease2<ElementNonBlockingMightFail>();
}

TEST(ClockTree, ElementMayBlock) {
  ClockSourceTest<ElementNonBlockingCannotFail> clock_non_blocking_cannot_fail;
  EXPECT_FALSE(clock_non_blocking_cannot_fail.may_block());

  ClockSourceTest<ElementNonBlockingMightFail> clock_non_blocking_might_fail;
  EXPECT_FALSE(clock_non_blocking_might_fail.may_block());

  ClockSourceTest<ElementBlocking> clock_blocking;
  EXPECT_TRUE(clock_blocking.may_block());
}

TEST(ClockTree, ClockDividerMayBlock) {
  struct clock_divider_test_data test_data;

  ClockSourceTest<ElementNonBlockingCannotFail> clock_non_blocking_cannot_fail;
  ClockSourceTest<ElementNonBlockingMightFail> clock_non_blocking_might_fail;
  ClockSourceTest<ElementBlocking> clock_blocking;

  ClockDividerTest<ElementNonBlockingCannotFail>
      clock_divider_non_blocking_cannot_fail(
          clock_non_blocking_cannot_fail, 1, 1, test_data);
  EXPECT_FALSE(clock_divider_non_blocking_cannot_fail.may_block());

  ClockDividerTest<ElementNonBlockingMightFail>
      clock_divider_non_blocking_might_fail(
          clock_non_blocking_might_fail, 1, 1, test_data);
  EXPECT_FALSE(clock_divider_non_blocking_might_fail.may_block());

  ClockDividerTest<ElementBlocking> clock_divider_blocking(
      clock_blocking, 1, 1, test_data);
  EXPECT_TRUE(clock_divider_blocking.may_block());
}

// Validate the behavior of the ClockSourceNoOp class
TEST(ClockTree, ClockSourceNoOp) {
  const uint32_t kClockDividerA = 23;
  const uint32_t kClockDividerB = 42;

  struct clock_divider_test_call_data call_data[] = {
      {kClockDividerA, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerB, 4, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerB, 4, ClockOperation::kRelease, pw::OkStatus()},
      {kClockDividerA, 2, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_divider_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceNoOp clock_source_no_op;
  ClockDividerTest<ElementNonBlockingCannotFail> clock_divider_a(
      clock_source_no_op, kClockDividerA, 2, test_data);
  ClockDividerTest<ElementNonBlockingCannotFail> clock_divider_b(
      clock_source_no_op, kClockDividerB, 4, test_data);

  EXPECT_EQ(clock_source_no_op.ref_count(), 0u);
  EXPECT_EQ(clock_divider_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  clock_divider_a.Acquire();
  EXPECT_EQ(clock_source_no_op.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  clock_divider_a.Acquire();
  EXPECT_EQ(clock_source_no_op.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  clock_divider_b.Acquire();
  EXPECT_EQ(clock_source_no_op.ref_count(), 2u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  clock_divider_b.Release();
  EXPECT_EQ(clock_source_no_op.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  clock_divider_a.Release();
  EXPECT_EQ(clock_source_no_op.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  clock_divider_a.Release();
  EXPECT_EQ(clock_source_no_op.ref_count(), 0u);
  EXPECT_EQ(clock_divider_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

// Validate the behavior of the ClockSourceNoOpBlocking class
TEST(ClockTree, ClockSourceNoOpBlocking) {
  const uint32_t kClockDividerA = 23;
  const uint32_t kClockDividerB = 42;

  struct clock_divider_test_call_data call_data[] = {
      {kClockDividerA, 2, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerB, 4, ClockOperation::kAcquire, pw::OkStatus()},
      {kClockDividerB, 4, ClockOperation::kRelease, pw::OkStatus()},
      {kClockDividerA, 2, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_divider_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceNoOpBlocking clock_source_no_op_blocking;
  ClockDividerTest<ElementBlocking> clock_divider_a(
      clock_source_no_op_blocking, kClockDividerA, 2, test_data);
  ClockDividerTest<ElementBlocking> clock_divider_b(
      clock_source_no_op_blocking, kClockDividerB, 4, test_data);

  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 0u);
  EXPECT_EQ(clock_divider_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  pw::Status status;

  status = clock_divider_a.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_a.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_b.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 2u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 1u);

  status = clock_divider_b.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 2u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_a.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 1u);
  EXPECT_EQ(clock_divider_a.ref_count(), 1u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  status = clock_divider_a.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_source_no_op_blocking.ref_count(), 0u);
  EXPECT_EQ(clock_divider_a.ref_count(), 0u);
  EXPECT_EQ(clock_divider_b.ref_count(), 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

// Validate that AcquireWith acquires the element_with during
// acquisition of element.
TEST(ClockTree, AcquireWith) {
  uint32_t element_with_value = 0;
  uint32_t element_value = 0;

  // The order of acquisitions validates that we are
  // acquiring `element_with` before acquring `element`,
  // and releasing `element_with` after acquiring `element`.
  struct clock_source_state_test_call_data call_data[] = {
      // AcquireWith(element, element_with)
      {1, ClockOperation::kAcquire, pw::OkStatus()},
      {2, ClockOperation::kAcquire, pw::OkStatus()},
      {1, ClockOperation::kRelease, pw::OkStatus()},
      // Release(element)
      {2, ClockOperation::kRelease, pw::OkStatus()},
      // Acquire(element_with)
      {1, ClockOperation::kAcquire, pw::OkStatus()},
      // AcquireWith(element, element_with)
      {2, ClockOperation::kAcquire, pw::OkStatus()}};

  struct clock_source_state_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceStateTestBlocking clock_element_with(
      1, &element_with_value, test_data);
  ClockSourceStateTestBlocking clock_element(2, &element_value, test_data);

  pw::Status status;

  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);

  status = clock_element.AcquireWith(clock_element_with);
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_element.ref_count(), 1u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);
  EXPECT_EQ(element_with_value, 0u);
  EXPECT_EQ(element_value, 2u);

  status = clock_element.Release();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);
  EXPECT_EQ(element_with_value, 0u);
  EXPECT_EQ(element_value, 0u);

  status = clock_element_with.Acquire();
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 1u);
  EXPECT_EQ(element_with_value, 1u);
  EXPECT_EQ(element_value, 0u);

  status = clock_element.AcquireWith(clock_element_with);
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_element.ref_count(), 1u);
  EXPECT_EQ(clock_element_with.ref_count(), 1u);
  EXPECT_EQ(element_with_value, 1u);
  EXPECT_EQ(element_value, 2u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, AcquireWithFailure1) {
  uint32_t element_with_value = 0;
  uint32_t element_value = 0;

  struct clock_source_state_test_call_data call_data[] = {
      // AcquireWith(element, element_with)
      {1, ClockOperation::kAcquire, pw::Status::Internal()}};

  struct clock_source_state_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceStateTestBlocking clock_element_with(
      1, &element_with_value, test_data);
  ClockSourceStateTestBlocking clock_element(2, &element_value, test_data);

  pw::Status status;

  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);

  status = clock_element.AcquireWith(clock_element_with);
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);
  EXPECT_EQ(element_with_value, 0u);
  EXPECT_EQ(element_value, 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, AcquireWithFailure2) {
  uint32_t element_with_value = 0;
  uint32_t element_value = 0;

  struct clock_source_state_test_call_data call_data[] = {
      // AcquireWith(element, element_with)
      {1, ClockOperation::kAcquire, pw::OkStatus()},
      {2, ClockOperation::kAcquire, pw::Status::Internal()},
      {1, ClockOperation::kRelease, pw::OkStatus()}};

  struct clock_source_state_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceStateTestBlocking clock_element_with(
      1, &element_with_value, test_data);
  ClockSourceStateTestBlocking clock_element(2, &element_value, test_data);

  pw::Status status;

  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);

  status = clock_element.AcquireWith(clock_element_with);
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);
  EXPECT_EQ(element_with_value, 0u);
  EXPECT_EQ(element_value, 0u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

TEST(ClockTree, AcquireWithFailure3) {
  uint32_t element_with_value = 0;
  uint32_t element_value = 0;

  struct clock_source_state_test_call_data call_data[] = {
      // AcquireWith(element, element_with)
      {1, ClockOperation::kAcquire, pw::OkStatus()},
      {2, ClockOperation::kAcquire, pw::OkStatus()},
      {1, ClockOperation::kRelease, pw::Status::Internal()}};

  struct clock_source_state_test_data test_data;
  INIT_TEST_DATA(test_data, call_data);

  ClockSourceStateTestBlocking clock_element_with(
      1, &element_with_value, test_data);
  ClockSourceStateTestBlocking clock_element(2, &element_value, test_data);

  pw::Status status;

  EXPECT_EQ(clock_element.ref_count(), 0u);
  EXPECT_EQ(clock_element_with.ref_count(), 0u);

  status = clock_element.AcquireWith(clock_element_with);
  EXPECT_EQ(status.code(), PW_STATUS_OK);
  EXPECT_EQ(clock_element.ref_count(), 1u);
  EXPECT_EQ(clock_element_with.ref_count(), 1u);
  EXPECT_EQ(element_with_value, 1u);
  EXPECT_EQ(element_value, 2u);

  EXPECT_EQ(test_data.num_calls, test_data.num_expected_calls);
}

// OptionalElement

class TestElement : public ElementBlocking {
 public:
  uint32_t acquire_count() const { return acquire_count_; }
  uint32_t release_count() const { return release_count_; }

  void set_acquire_status(Status status) { acquire_status_ = status; }
  void set_release_status(Status status) { release_status_ = status; }

 private:
  Status DoAcquireLocked() final {
    ++acquire_count_;
    return acquire_status_;
  }

  Status DoReleaseLocked() final {
    ++release_count_;
    return release_status_;
  }

  Status DoEnable() final { return OkStatus(); }

  uint32_t acquire_count_ = 0;
  uint32_t release_count_ = 0;

  Status acquire_status_ = OkStatus();
  Status release_status_ = OkStatus();
};

TEST(OptionalElement, SuccessWhenEmpty) {
  OptionalElement op;

  PW_TEST_EXPECT_OK(op.Acquire());
  PW_TEST_EXPECT_OK(op.Release());
}

TEST(OptionalElement, CallsAcquireRelease) {
  TestElement element;
  OptionalElement op(element);

  PW_TEST_EXPECT_OK(op.Acquire());
  EXPECT_EQ(element.acquire_count(), 1u);
  EXPECT_EQ(element.release_count(), 0u);

  PW_TEST_EXPECT_OK(op.Release());
  EXPECT_EQ(element.acquire_count(), 1u);
  EXPECT_EQ(element.release_count(), 1u);
}

TEST(OptionalElement, PassesThroughStatus) {
  TestElement element;
  OptionalElement op(element);

  element.set_acquire_status(Status::Internal());
  EXPECT_EQ(op.Acquire(), Status::Internal());

  element.set_release_status(Status::Unavailable());
  EXPECT_EQ(op.Release(), Status::Unavailable());
}

TEST(ClockTreeDeathTest, ReleaseWithoutAcquireCrashes) {
  ClockSourceTest<ElementBlocking> clock_a;
  EXPECT_DEATH_IF_SUPPORTED(clock_a.Release().IgnoreError(), ".*");

  ClockSourceTest<ElementNonBlockingMightFail> clock_b;
  EXPECT_DEATH_IF_SUPPORTED(clock_b.Release().IgnoreError(), ".*");
}

}  // namespace
}  // namespace pw::clock_tree
