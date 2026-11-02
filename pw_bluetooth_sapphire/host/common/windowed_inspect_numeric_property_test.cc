// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/common/windowed_inspect_numeric_property.h"

#include <pw_async/fake_dispatcher_fixture.h>
#include <pw_function/function.h>

#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"

namespace bt {

namespace {

template <typename T>
class TestProperty {
 public:
  using ValueCallback = pw::Function<void(const T& value)>;
  TestProperty() = default;
  TestProperty(T value,
               ValueCallback cb,
               pw::Function<void()> subtract_cb = nullptr)
      : value_(value),
        value_cb_(std::move(cb)),
        subtract_cb_(std::move(subtract_cb)) {}

  void Add(const T& value) {
    value_ += value;
    if (value_cb_) {
      value_cb_(value_);
    }
  }

  void Subtract(const T& value) {
    value_ -= value;
    if (value_cb_) {
      value_cb_(value_);
    }
    if (subtract_cb_) {
      subtract_cb_();
    }
  }

 private:
  T value_;
  ValueCallback value_cb_;
  pw::Function<void()> subtract_cb_;
};

using WindowedProperty = WindowedInspectNumericProperty<TestProperty<int>, int>;
using WindowedInspectNumericPropertyTest =
    pw::async::test::FakeDispatcherFixture;

TEST_F(WindowedInspectNumericPropertyTest, AddTwoValues) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration);
  int value = 0;
  auto value_cb = [&](auto val) { value = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb));

  windowed_prop.Add(1);
  EXPECT_EQ(value, 1);
  RunFor(std::chrono::minutes(1));
  EXPECT_EQ(value, 1);

  windowed_prop.Add(2);
  EXPECT_EQ(value, 3);
  RunFor(std::chrono::minutes(1));
  EXPECT_EQ(value, 3);

  // Let first value expire.
  RunFor(std::chrono::minutes(1));
  EXPECT_EQ(value, 2);
  // Let second value expire.
  RunFor(std::chrono::minutes(1));
  EXPECT_EQ(value, 0);

  // Ensure timer doesn't fire again.
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);
}

TEST_F(WindowedInspectNumericPropertyTest, AddTwoValuesAtSameTime) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration);
  int value = 0;
  auto value_cb = [&](auto val) { value = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb));

  windowed_prop.Add(1);
  windowed_prop.Add(2);
  EXPECT_EQ(value, 3);
  RunFor(std::chrono::minutes(1));
  EXPECT_EQ(value, 3);
  RunFor(std::chrono::minutes(2));
  EXPECT_EQ(value, 0);

  // Ensure timer doesn't fire again.
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);
}

TEST_F(WindowedInspectNumericPropertyTest, AddValueThenExpireThenAddValue) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration);
  int value = 0;
  auto value_cb = [&](auto val) { value = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb));

  windowed_prop.Add(1);
  EXPECT_EQ(value, 1);
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);

  windowed_prop.Add(2);
  EXPECT_EQ(value, 2);
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);

  // Ensure timer doesn't fire again.
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);
}

TEST_F(WindowedInspectNumericPropertyTest,
       AddTwoValuesWithinResolutionIntervalExpiresBothSimultaneously) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  constexpr pw::chrono::SystemClock::duration kResolution =
      std::chrono::seconds(3);
  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration, kResolution);
  int value = 0;
  auto value_cb = [&](auto val) { value = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb));

  // First two values are within kResolution of each other in time.
  windowed_prop.Add(1);
  constexpr pw::chrono::SystemClock::duration kTinyDuration =
      std::chrono::milliseconds(1);
  RunFor(kTinyDuration);
  windowed_prop.Add(1);
  EXPECT_EQ(value, 2);

  // Third value is spaced kResolution apart from the first value.
  RunFor(kResolution - kTinyDuration);
  windowed_prop.Add(1);
  EXPECT_EQ(value, 3);

  // Let first value expire.
  RunFor(kExpiryDuration - kResolution);

  // First and second values should have expired because they were merged.
  EXPECT_EQ(value, 1);

  // Let third value expire.
  RunFor(kResolution);
  EXPECT_EQ(value, 0);
}

TEST_F(WindowedInspectNumericPropertyTest, SetPropertyClearsValueAndTimer) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration);
  int value_0 = 0;
  auto value_cb_0 = [&](auto val) { value_0 = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb_0));

  windowed_prop.Add(1);
  EXPECT_EQ(value_0, 1);
  int value_1 = 0;
  auto value_cb_1 = [&](auto val) { value_1 = val; };
  windowed_prop.SetProperty(TestProperty<int>(0, value_cb_1));
  // Ensure timer doesn't fire.
  RunFor(kExpiryDuration);
  EXPECT_EQ(value_0, 1);
  EXPECT_EQ(value_1, 0);

  // Ensure values can be added to new property.
  windowed_prop.Add(3);
  EXPECT_EQ(value_0, 1);
  EXPECT_EQ(value_1, 3);
  RunFor(kExpiryDuration);
  EXPECT_EQ(value_0, 1);
  EXPECT_EQ(value_1, 0);
}

TEST_F(WindowedInspectNumericPropertyTest,
       QueueSizeBoundedUnderHighFrequencyUpdates) {
  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(10);
  constexpr pw::chrono::SystemClock::duration kInterval =
      std::chrono::milliseconds(50);
  constexpr size_t kNumUpdates = 12000;

  WindowedProperty windowed_prop(dispatcher(), kExpiryDuration);
  int value = 0;
  size_t subtract_count = 0;
  windowed_prop.SetProperty(TestProperty<int>(
      0, [&](auto val) { value = val; }, [&]() { subtract_count++; }));

  for (size_t i = 0; i < kNumUpdates; ++i) {
    windowed_prop.Add(1);
    if (i + 1 < kNumUpdates) {
      RunFor(kInterval);
    }
  }

  // Elapsed time is 11,999 * 50ms = 599,950ms (< 10 minutes), so no entries
  // have expired yet and all queued nodes are concurrently retained in memory.
  EXPECT_EQ(value, static_cast<int>(kNumUpdates));
  EXPECT_EQ(subtract_count, 0u);

  // Drain the full expiry window. Each Subtract() call corresponds to one
  // distinct node popped from the internal queue.
  RunFor(kExpiryDuration);
  EXPECT_EQ(value, 0);
  EXPECT_LE(subtract_count, WindowedProperty::kMaxPendingValues);
}

#ifndef NINSPECT
TEST_F(WindowedInspectNumericPropertyTest, AttachInspectRealIntProperty) {
  using namespace ::inspect::testing;
  ::inspect::Inspector inspector;
  auto& root = inspector.GetRoot();

  constexpr pw::chrono::SystemClock::duration kExpiryDuration =
      std::chrono::minutes(3);
  WindowedInspectIntProperty windowed_property(dispatcher(), kExpiryDuration);
  windowed_property.AttachInspect(root, "windowed");

  auto hierarchy = ::inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy.is_ok());
  EXPECT_THAT(
      hierarchy.take_value(),
      AllOf(NodeMatches(PropertyList(ElementsAre(IntIs("windowed", 0))))));

  windowed_property.Add(7);

  hierarchy = ::inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy.is_ok());
  EXPECT_THAT(
      hierarchy.take_value(),
      AllOf(NodeMatches(PropertyList(ElementsAre(IntIs("windowed", 7))))));
}
#endif  // NINSPECT

}  // namespace

}  // namespace bt
