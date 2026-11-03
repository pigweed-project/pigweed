// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/hci-spec/le_connection_parameters.h"

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_unit_test/framework.h"

namespace bt::hci_spec {
namespace {

// Valid parameters: interval 7.5 ms - 80 ms, latency 2, supervision timeout
// 640 ms (> (1 + 2) * 80 ms * 2 = 480 ms).
constexpr uint16_t kValidMinInterval = 0x0006;
constexpr uint16_t kValidMaxInterval = 0x0040;
constexpr uint16_t kValidLatency = 2;
constexpr uint16_t kValidTimeout = 0x0040;

// The "no specific value requested" value of the Peripheral Preferred
// Connection Parameters characteristic (Core Spec v6.2, Vol 3, Part C,
// Sec 12.3).
constexpr uint16_t kNoPreference = 0xFFFF;

constexpr LEPreferredConnectionParameters kValidParams(kValidMinInterval,
                                                       kValidMaxInterval,
                                                       kValidLatency,
                                                       kValidTimeout);
static_assert(kValidParams.IsValid());
static_assert(!LEPreferredConnectionParameters().IsValid());

TEST(LEPreferredConnectionParametersTest, ValidParametersAreValid) {
  EXPECT_TRUE(kValidParams.IsValid());
  EXPECT_TRUE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMin,
                                      /*max_latency=*/0,
                                      kLEConnectionSupervisionTimeoutMin)
          .IsValid());
  EXPECT_TRUE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMax,
                                      /*max_latency=*/0,
                                      kLEConnectionSupervisionTimeoutMax)
          .IsValid());
}

TEST(LEPreferredConnectionParametersTest, DefaultConstructedIsInvalid) {
  EXPECT_FALSE(LEPreferredConnectionParameters().IsValid());
}

TEST(LEPreferredConnectionParametersTest, MinIntervalLargerThanMaxIsInvalid) {
  EXPECT_FALSE(LEPreferredConnectionParameters(
                   kValidMaxInterval + 1, kValidMaxInterval, 0, kValidTimeout)
                   .IsValid());
}

TEST(LEPreferredConnectionParametersTest, MinIntervalBelowRangeIsInvalid) {
  EXPECT_FALSE(LEPreferredConnectionParameters(kLEConnectionIntervalMin - 1,
                                               kValidMaxInterval,
                                               kValidLatency,
                                               kValidTimeout)
                   .IsValid());
}

TEST(LEPreferredConnectionParametersTest, MaxIntervalAboveRangeIsInvalid) {
  EXPECT_FALSE(
      LEPreferredConnectionParameters(kValidMinInterval,
                                      kLEConnectionIntervalMax + 1,
                                      /*max_latency=*/0,
                                      kLEConnectionSupervisionTimeoutMax)
          .IsValid());
}

TEST(LEPreferredConnectionParametersTest, LatencyAboveRangeIsInvalid) {
  EXPECT_FALSE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMin,
                                      kLEConnectionLatencyMax + 1,
                                      kLEConnectionSupervisionTimeoutMax)
          .IsValid());
  // The maximum latency is valid with a small enough interval.
  EXPECT_TRUE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMin,
                                      kLEConnectionLatencyMax,
                                      kLEConnectionSupervisionTimeoutMax)
          .IsValid());
}

TEST(LEPreferredConnectionParametersTest,
     SupervisionTimeoutOutsideRangeIsInvalid) {
  EXPECT_FALSE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMin,
                                      /*max_latency=*/0,
                                      kLEConnectionSupervisionTimeoutMin - 1)
          .IsValid());
  EXPECT_FALSE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMin,
                                      /*max_latency=*/0,
                                      kLEConnectionSupervisionTimeoutMax + 1)
          .IsValid());
}

TEST(LEPreferredConnectionParametersTest,
     SupervisionTimeoutMustExceedLatencyAndMaxInterval) {
  // (1 + 2) * 0x40 * 1.25 ms * 2 = 480 ms, which is 48 in units of 10 ms. The
  // supervision timeout must be strictly larger than that.
  EXPECT_FALSE(LEPreferredConnectionParameters(
                   kValidMinInterval, kValidMaxInterval, kValidLatency, 48)
                   .IsValid());
  EXPECT_TRUE(LEPreferredConnectionParameters(
                  kValidMinInterval, kValidMaxInterval, kValidLatency, 49)
                  .IsValid());

  // Every field is at the edge of its range, but the combination is invalid.
  EXPECT_FALSE(
      LEPreferredConnectionParameters(kLEConnectionIntervalMin,
                                      kLEConnectionIntervalMax,
                                      kLEConnectionLatencyMax,
                                      kLEConnectionSupervisionTimeoutMax)
          .IsValid());
}

TEST(LEPreferredConnectionParametersTest, NoPreferenceValuesAreInvalid) {
  EXPECT_FALSE(LEPreferredConnectionParameters(
                   kNoPreference, kNoPreference, kValidLatency, kValidTimeout)
                   .IsValid());
  EXPECT_FALSE(
      LEPreferredConnectionParameters(
          kValidMinInterval, kNoPreference, kValidLatency, kValidTimeout)
          .IsValid());
  EXPECT_FALSE(
      LEPreferredConnectionParameters(
          kValidMinInterval, kValidMaxInterval, kNoPreference, kValidTimeout)
          .IsValid());
  EXPECT_FALSE(
      LEPreferredConnectionParameters(
          kValidMinInterval, kValidMaxInterval, kValidLatency, kNoPreference)
          .IsValid());
  EXPECT_FALSE(LEPreferredConnectionParameters(
                   kNoPreference, kNoPreference, kNoPreference, kNoPreference)
                   .IsValid());
}

}  // namespace
}  // namespace bt::hci_spec
