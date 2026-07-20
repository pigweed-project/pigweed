// Copyright 2020 The Pigweed Authors
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

#include "pw_metric/metric.h"

#include <limits>

#include "pw_log/log.h"
#include "pw_unit_test/framework.h"

namespace pw::metric {
namespace {

TEST(Metric, FloatFromObject) {
  // Note leading 4 bits are stripped from the name to store the type.
  Token token = 0xf1223344;

  TypedMetric<float> m(token, 1.5f);
  EXPECT_EQ(m.name(), 0x01223344u);
  EXPECT_TRUE(m.is_float());
  EXPECT_FALSE(m.is_uint32());
  EXPECT_EQ(m.value(), 1.5f);

  m.Set(55.1f);
  EXPECT_EQ(m.value(), 55.1f);

  // No increment operation for float.
}

TEST(Metric, IntFromObject) {
  // Note leading 4 bits are stripped from the name to store the type.
  Token token = 0xf1223344;

  TypedMetric<uint32_t> m(token, static_cast<uint32_t>(31337u));
  EXPECT_EQ(m.name(), 0x01223344u);
  EXPECT_TRUE(m.is_uint32());
  EXPECT_FALSE(m.is_float());
  EXPECT_EQ(m.value(), 31337u);

  m.Set(414u);
  EXPECT_EQ(m.value(), 414u);

  m.Increment();
  EXPECT_EQ(m.value(), 415u);

  m.Increment(11u);
  EXPECT_EQ(m.value(), 426u);
}

TEST(Metric, IntLimits) {
  // Note leading 4 bits are stripped from the name to store the type.
  Token token = 0xf1223344;

  TypedMetric<uint32_t> m(token, static_cast<uint32_t>(31337u));
  EXPECT_EQ(m.name(), 0x01223344u);
  EXPECT_TRUE(m.is_uint32());
  EXPECT_FALSE(m.is_float());
  EXPECT_EQ(m.value(), 31337u);

  m.Set(10);
  EXPECT_EQ(m.value(), 10u);

  m.Decrement(7);
  EXPECT_EQ(m.value(), 3u);

  m.Decrement(7);
  EXPECT_EQ(m.value(), 0u);

  m.Decrement(7);
  EXPECT_EQ(m.value(), 0u);

  m.Set(std::numeric_limits<uint32_t>::max() - 10);
  EXPECT_EQ(m.value(), std::numeric_limits<uint32_t>::max() - 10);

  m.Increment();
  EXPECT_EQ(m.value(), std::numeric_limits<uint32_t>::max() - 9);

  m.Increment(9);
  EXPECT_EQ(m.value(), std::numeric_limits<uint32_t>::max());

  m.Increment(2);
  EXPECT_EQ(m.value(), std::numeric_limits<uint32_t>::max());
}

TEST(Metric, IntFromMacroLocal) {
  PW_METRIC(m, "some_metric", 14u);
  EXPECT_TRUE(m.is_uint32());
  EXPECT_EQ(m.value(), 14u);
}

TEST(Metric, FloatFromMacroLocal) {
  PW_METRIC(m, "some_metric", 3.14f);
  EXPECT_TRUE(m.is_float());
  EXPECT_EQ(m.value(), 3.14f);
}

TEST(Metric, ExposedToken) {
  PW_METRIC(m, "some_metric", 0u);
  static constexpr Token kToken = PW_METRIC_TOKEN("some_metric");
  EXPECT_EQ(m.name(), kToken);
}

TEST(Metric, GroupMacroInFunctionContext) {
  PW_METRIC_GROUP(group, "fancy_subsystem");
  PW_METRIC(group, x, "x", 5555u);
  PW_METRIC(group, y, "y", 6.0f);

  // These calls are needed to satisfy GCC, otherwise GCC warns about an unused
  // variable (even though it is used and passed to the group, which adds it):
  //
  //   metric_test.cc:72:20: error: variable 'x' set but not used
  //   [-Werror=unused-but-set-variable]
  //
  x.Increment(10);
  y.Set(5.0f);

  group.Dump();
  EXPECT_EQ(group.metrics().size(), 2u);
}

// The below are compile tests to ensure the macros work at global scope.

// Case 1: No group specified.
PW_METRIC(global_x, "global_x", 5555u);
PW_METRIC(global_y, "global_y", 6.0f);

// Case 2: Group specified.
PW_METRIC_GROUP(global_group, "a_global_group");
PW_METRIC(global_group, global_z, "global_x", 5555u);
PW_METRIC(global_group, global_w, "global_y", 6.0f);

TEST(Metric, GlobalScope) {
  EXPECT_EQ(global_x.value(), 5555u);
  EXPECT_EQ(global_y.value(), 6.0f);
}

// A fake object to illustrate the API and show nesting metrics.
// This also tests creating metrics as members inside a class.
class I2cBus {
 public:
  void Transaction() {
    // An entirely unconvincing fake I2C transaction implementation.
    transactions_.Increment();
    bytes_sent_.Increment(5);
  }

  Group& stats() { return metrics_; }

 private:
  // Test a group with metrics in it, as a class member.
  // Note that in many cases, the group would be passed in externally instead.
  PW_METRIC_GROUP(metrics_, "i2c");
  PW_METRIC(metrics_, bus_errors_, "bus_errors", 0u);
  PW_METRIC(metrics_, transactions_, "transactions", 0u);
  PW_METRIC(metrics_, bytes_sent_, "bytes_sent", 0u);

  // Test metrics without a group, as a class member.
  PW_METRIC(a, "a", 0u);
  PW_METRIC(b, "b", 10.0f);
  PW_METRIC(c, "c", 525u);
};

class Gyro {
 public:
  Gyro(I2cBus& i2c_bus, Group& parent_metrics) : i2c_bus_(i2c_bus) {
    // Make the gyro a child of the I2C bus. Note that the other arrangement,
    // where the i2c bus is a child of the gyro, doesn't work if there are
    // multiple objects on the I2C bus due to the intrusive list mechanism.
    parent_metrics.Add(metrics_);
  }

  void Init() {
    i2c_bus_.Transaction();
    initialized_.Increment();
  }

  void ReadAngularVelocity() {
    // Pretend to be doing some transactions and pulling angular velocity.
    // Pretend this gyro is inefficient and requires multiple transactions.
    i2c_bus_.Transaction();
    i2c_bus_.Transaction();
    i2c_bus_.Transaction();
    num_samples_.Increment();
  }

  Group& stats() { return metrics_; }

 private:
  I2cBus& i2c_bus_;

  // In this case, "gyro" groups the relevant metrics, but it is possible to
  // have freestanding metrics directly without a group; however, those
  // free-standing metrics must be added to a group or list supplied elsewhere
  // for collection.
  PW_METRIC_GROUP(metrics_, "gyro");
  PW_METRIC(metrics_, num_samples_, "num_samples", 1u);
  PW_METRIC(metrics_, init_time_us_, "init_time_us", 1.0f);
  PW_METRIC(metrics_, initialized_, "initialized", 0u);
};

// The below test produces output like:
//
//   "$6doqFw==": {
//     "$05OCZw==": {
//       "$VpPfzg==": 1,
//       "$LGPMBQ==": 1.000000,
//       "$+iJvUg==": 5,
//     }
//     "$9hPNxw==": 65,
//     "$oK7HmA==": 13,
//     "$FCM4qQ==": 0,
//   }
//
// Note the metric names are tokenized with base64. Decoding requires using the
// Pigweed detokenizer. With a detokenizing-enabled logger, you would get:
//
//   "i2c": {
//     "gyro": {
//       "num_sampleses": 1,
//       "init_time_us": 1.000000,
//       "initialized": 5,
//     }
//     "bus_errors": 65,
//     "transactions": 13,
//     "bytes_sent": 0,
//   }
//
TEST(Metric, InlineConstructionWithGroups) {
  I2cBus i2c_bus;
  Gyro gyro(i2c_bus, i2c_bus.stats());

  gyro.Init();
  gyro.ReadAngularVelocity();
  gyro.ReadAngularVelocity();
  gyro.ReadAngularVelocity();
  gyro.ReadAngularVelocity();

  // This "test" doesn't really test anything, and more illustrates how to the
  // metrics could be instantiated in an object tree.
  //
  // Unfortunatlely, testing dump is difficult since we don't have log
  // redirection for tests.
  i2c_bus.stats().Dump();
}

// PW_METRIC_STATIC doesn't support class scopes, since a definition must be
// provided outside of the class body.
// TODO(keir): add support for class scopes and enable this test
#if 0
class MetricTest: public ::testing::Test {
  public:
    void Increment() {
      metric_.Increment();
    }

  private:
    PW_METRIC_STATIC(metric_, "metric", 0u);
};

TEST_F(MetricTest, StaticWithinAClass) {
  Increment();
}
#endif

Metric* StaticMetricIncrement() {
  PW_METRIC_STATIC(metric, "metric", 0u);
  metric.Increment();
  return &metric;
}

TEST(Metric, StaticWithinAFunction) {
  Metric* metric = StaticMetricIncrement();
  EXPECT_EQ(metric->as_int(), 1u);
  StaticMetricIncrement();
  EXPECT_EQ(metric->as_int(), 2u);
}

TEST(Metric, ListFind) {
  PW_METRIC_GROUP(group, "fancy_subsystem");
  PW_METRIC(group, x, "x", 5555u);
  PW_METRIC(group, y, "y", 6.0f);

  static constexpr Token token_x = PW_METRIC_TOKEN("x");
  static constexpr Token token_y = PW_METRIC_TOKEN("y");
  static constexpr Token token_z = PW_METRIC_TOKEN("z");

  EXPECT_EQ(group.metrics().find(token_x), &x);
  EXPECT_EQ(group.metrics().find(token_y), &y);
  EXPECT_EQ(group.metrics().find(token_z), nullptr);
}

TEST(Metric, GroupListFind) {
  PW_METRIC_GROUP(parent, "parent");
  PW_METRIC_GROUP(parent, child1, "child1");
  PW_METRIC_GROUP(parent, child2, "child2");

  static constexpr Token token_1 =
      PW_TOKENIZE_STRING_DOMAIN("metrics", "child1");
  static constexpr Token token_2 =
      PW_TOKENIZE_STRING_DOMAIN("metrics", "child2");
  static constexpr Token token_3 =
      PW_TOKENIZE_STRING_DOMAIN("metrics", "child3");

  EXPECT_EQ(parent.children().find(token_1), &child1);
  EXPECT_EQ(parent.children().find(token_2), &child2);
  EXPECT_EQ(parent.children().find(token_3), nullptr);
}

TEST(Metric, ListForEachWithFunctor) {
  PW_METRIC_GROUP(group, "fancy_subsystem");
  PW_METRIC(group, x, "x", 5555u);
  PW_METRIC(group, y, "y", 6.0f);

  struct Count {
    int count = 0;
    void operator()(const Metric&) { count++; }
  };

  Count c = group.metrics().for_each(Count());
  EXPECT_EQ(c.count, 2);
}

}  // namespace
}  // namespace pw::metric
