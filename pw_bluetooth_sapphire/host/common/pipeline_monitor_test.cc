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

#include "pw_bluetooth_sapphire/internal/host/common/pipeline_monitor.h"

#include <gmock/gmock.h>
#include <pw_async/fake_dispatcher_fixture.h>

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/retire_log.h"

namespace bt {
namespace {

using Token = PipelineMonitor::Token;

using PipelineMonitorTest = pw::async::test::FakeDispatcherFixture;

const internal::RetireLog kRetireLogDefaultParams(/*min_depth=*/1,
                                                  /*max_depth=*/100);

TEST_F(PipelineMonitorTest, TokensCanOutliveMonitor) {
  auto monitor =
      std::make_unique<PipelineMonitor>(dispatcher(), kRetireLogDefaultParams);
  auto token = monitor->Issue(0);
  monitor.reset();
}

TEST_F(PipelineMonitorTest, SequentialTokensModifyCounts) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);
  EXPECT_EQ(0U, monitor.bytes_issued());
  EXPECT_EQ(0, monitor.tokens_issued());
  EXPECT_EQ(0U, monitor.bytes_in_flight());
  EXPECT_EQ(0, monitor.tokens_in_flight());
  EXPECT_EQ(0U, monitor.bytes_retired());
  EXPECT_EQ(0, monitor.tokens_retired());

  constexpr size_t kByteCount = 2;
  {
    auto token = monitor.Issue(kByteCount);
    EXPECT_EQ(kByteCount, monitor.bytes_issued());
    EXPECT_EQ(1, monitor.tokens_issued());
    EXPECT_EQ(kByteCount, monitor.bytes_in_flight());
    EXPECT_EQ(1, monitor.tokens_in_flight());
    EXPECT_EQ(0U, monitor.bytes_retired());
    EXPECT_EQ(0, monitor.tokens_retired());

    token.Retire();
    EXPECT_EQ(kByteCount, monitor.bytes_issued());
    EXPECT_EQ(1, monitor.tokens_issued());
    EXPECT_EQ(0U, monitor.bytes_in_flight());
    EXPECT_EQ(0, monitor.tokens_in_flight());
    EXPECT_EQ(kByteCount, monitor.bytes_retired());
    EXPECT_EQ(1, monitor.tokens_retired());

    // Test that a moved-from value is reusable and that it retires by going out
    // of scope
    token = monitor.Issue(kByteCount);
    EXPECT_EQ(2 * kByteCount, monitor.bytes_issued());
    EXPECT_EQ(2, monitor.tokens_issued());
    EXPECT_EQ(kByteCount, monitor.bytes_in_flight());
    EXPECT_EQ(1, monitor.tokens_in_flight());
    EXPECT_EQ(kByteCount, monitor.bytes_retired());
    EXPECT_EQ(1, monitor.tokens_retired());
  }

  EXPECT_EQ(2 * kByteCount, monitor.bytes_issued());
  EXPECT_EQ(2, monitor.tokens_issued());
  EXPECT_EQ(0U, monitor.bytes_in_flight());
  EXPECT_EQ(0, monitor.tokens_in_flight());
  EXPECT_EQ(2 * kByteCount, monitor.bytes_retired());
  EXPECT_EQ(2, monitor.tokens_retired());
}

TEST_F(PipelineMonitorTest, TokensCanBeMoved) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);
  EXPECT_EQ(0U, monitor.bytes_issued());
  EXPECT_EQ(0, monitor.tokens_issued());
  EXPECT_EQ(0U, monitor.bytes_in_flight());
  EXPECT_EQ(0, monitor.tokens_in_flight());
  EXPECT_EQ(0U, monitor.bytes_retired());
  EXPECT_EQ(0, monitor.tokens_retired());

  constexpr size_t kByteCount = 2;
  auto token0 = monitor.Issue(kByteCount);
  auto token1 = std::move(token0);
  EXPECT_EQ(kByteCount, monitor.bytes_issued());
  EXPECT_EQ(1, monitor.tokens_issued());
  EXPECT_EQ(kByteCount, monitor.bytes_in_flight());
  EXPECT_EQ(1, monitor.tokens_in_flight());
  EXPECT_EQ(0U, monitor.bytes_retired());
  EXPECT_EQ(0, monitor.tokens_retired());

  // both active token and moved-from token can be retired safely
  token0.Retire();
  token1.Retire();
  EXPECT_EQ(kByteCount, monitor.bytes_issued());
  EXPECT_EQ(1, monitor.tokens_issued());
  EXPECT_EQ(0U, monitor.bytes_in_flight());
  EXPECT_EQ(0, monitor.tokens_in_flight());
  EXPECT_EQ(kByteCount, monitor.bytes_retired());
  EXPECT_EQ(1, monitor.tokens_retired());
}

TEST_F(PipelineMonitorTest, SubscribeToMaxTokensAlert) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  std::optional<PipelineMonitor::MaxTokensInFlightAlert> received_alert;
  constexpr int kMaxTokensInFlight = 1;
  monitor.SetAlert(PipelineMonitor::MaxTokensInFlightAlert{kMaxTokensInFlight},
                   [&received_alert](auto alert) { received_alert = alert; });

  // First token does not exceed in-flight threshold.
  auto token0 = monitor.Issue(0);
  EXPECT_FALSE(received_alert);

  // Total issued (but not in-flight) exceeds threshold.
  token0.Retire();
  token0 = monitor.Issue(0);
  ASSERT_LT(kMaxTokensInFlight, monitor.tokens_issued());
  EXPECT_FALSE(received_alert);

  // Total in-flight exceeds threshold.
  auto token1 = monitor.Issue(0);
  ASSERT_TRUE(received_alert.has_value());
  EXPECT_EQ(kMaxTokensInFlight + 1, received_alert.value().value);

  // Alert has expired after firing once.
  received_alert.reset();
  auto token2 = monitor.Issue(0);
  EXPECT_FALSE(received_alert);
}

TEST_F(PipelineMonitorTest, SubscribeToMaxBytesAlert) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  std::optional<PipelineMonitor::MaxBytesInFlightAlert> received_alert;
  constexpr size_t kMaxBytesInFlight = 1;
  monitor.SetAlert(PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight},
                   [&received_alert](auto alert) { received_alert = alert; });

  // First token does not exceed total bytes in flight threshold.
  auto token0 = monitor.Issue(kMaxBytesInFlight);
  EXPECT_FALSE(received_alert);

  // Total in-flight exceeds threshold.
  auto token1 = monitor.Issue(1);
  ASSERT_TRUE(received_alert.has_value());
  EXPECT_EQ(kMaxBytesInFlight + 1, received_alert.value().value);
}

TEST_F(PipelineMonitorTest, SubscribeToMaxAgeAlert) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  std::optional<PipelineMonitor::MaxAgeRetiredAlert> received_alert;
  constexpr pw::chrono::SystemClock::duration kMaxAge =
      std::chrono::milliseconds(500);
  monitor.SetAlert(PipelineMonitor::MaxAgeRetiredAlert{kMaxAge},
                   [&received_alert](auto alert) { received_alert = alert; });

  // Token outlives threshold age, but doesn't signal alert until it's retired.
  auto token0 = monitor.Issue(0);
  RunFor(kMaxAge * 2);
  EXPECT_FALSE(received_alert);

  // Total in-flight exceeds threshold.
  token0.Retire();
  ASSERT_TRUE(received_alert.has_value());
  EXPECT_EQ(kMaxAge * 2, received_alert.value().value);
}

TEST_F(PipelineMonitorTest, SubscribeToAlertInsideHandler) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  std::optional<PipelineMonitor::MaxBytesInFlightAlert> received_alert;
  constexpr size_t kMaxBytesInFlight = 2;

  auto renew_subscription = [&monitor, &received_alert](auto) {
    // Same threshold, so it should be triggered eventually, but not
    // immediately.
    monitor.SetAlert(
        PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight - 1},
        [&received_alert](auto alert) { received_alert = alert; });
  };
  monitor.SetAlert(PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight},
                   renew_subscription);

  // Total in-flight exceeds threshold.
  auto token0 = monitor.Issue(kMaxBytesInFlight + 1);
  EXPECT_FALSE(received_alert);

  // Re-subscribed alert doesn't get called until the monitored value
  // potentially changes again.
  auto token1 = monitor.Issue(0);
  ASSERT_TRUE(received_alert.has_value());
  EXPECT_EQ(kMaxBytesInFlight + 1, received_alert.value().value);
}

TEST_F(PipelineMonitorTest,
       MultipleMaxBytesInFlightAlertsWithDifferentThresholds) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  std::optional<PipelineMonitor::MaxBytesInFlightAlert> received_alert_0;
  constexpr size_t kMaxBytesInFlight0 = 1;
  monitor.SetAlert(
      PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight0},
      [&received_alert_0](auto alert) { received_alert_0 = alert; });
  std::optional<PipelineMonitor::MaxBytesInFlightAlert> received_alert_1;
  constexpr size_t kMaxBytesInFlight1 = 2;
  monitor.SetAlert(
      PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight1},
      [&received_alert_1](auto alert) { received_alert_1 = alert; });

  // Total in-flight exceeds threshold 0.
  auto token0 = monitor.Issue(kMaxBytesInFlight0 + 1);
  ASSERT_TRUE(received_alert_0.has_value());
  EXPECT_LT(kMaxBytesInFlight0, received_alert_0.value().value);
  EXPECT_GE(kMaxBytesInFlight1, received_alert_0.value().value);
  EXPECT_FALSE(received_alert_1);

  // Total in-flight exceeds threshold 1.
  auto token1 = monitor.Issue(kMaxBytesInFlight1);
  ASSERT_TRUE(received_alert_1.has_value());
  EXPECT_LT(kMaxBytesInFlight1, received_alert_1.value().value);
}

TEST_F(PipelineMonitorTest, SubscribeToMultipleDissimilarAlerts) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  constexpr size_t kMaxBytesInFlight = 2;
  constexpr int kMaxTokensInFlight = 1;

  int listener_call_count = 0;
  int max_bytes_alerts = 0;
  int max_tokens_alerts = 0;
  auto alerts_listener = [&](auto alert_value) {
    listener_call_count++;
    if (std::holds_alternative<PipelineMonitor::MaxBytesInFlightAlert>(
            alert_value)) {
      max_bytes_alerts++;
    } else if (std::holds_alternative<PipelineMonitor::MaxTokensInFlightAlert>(
                   alert_value)) {
      max_tokens_alerts++;
    }
  };
  monitor.SetAlerts(
      alerts_listener,
      PipelineMonitor::MaxBytesInFlightAlert{kMaxBytesInFlight},
      PipelineMonitor::MaxTokensInFlightAlert{kMaxTokensInFlight});

  auto token0 = monitor.Issue(0);
  EXPECT_EQ(0, listener_call_count);

  auto token1 = monitor.Issue(0);
  EXPECT_EQ(1, listener_call_count);
  EXPECT_EQ(1, max_tokens_alerts);

  auto token2 = monitor.Issue(kMaxBytesInFlight + 1);
  EXPECT_EQ(2, listener_call_count);
  EXPECT_EQ(1, max_bytes_alerts);
}

TEST_F(PipelineMonitorTest, TokensRetireIntoRetireLog) {
  PipelineMonitor monitor(
      dispatcher(), internal::RetireLog(/*min_depth=*/1, /*max_depth=*/64));

  auto token = monitor.Issue(1);
  EXPECT_EQ(0U, monitor.retire_log().depth());

  const pw::chrono::SystemClock::duration kAge = std::chrono::milliseconds(10);
  RunFor(kAge);
  token.Retire();
  EXPECT_EQ(1U, monitor.retire_log().depth());
  const auto bytes_quantiles =
      monitor.retire_log().ComputeByteCountQuantiles(std::array{0., .5, 1.});
  ASSERT_TRUE(bytes_quantiles.has_value());
  EXPECT_THAT(*bytes_quantiles, testing::ElementsAre(1U, 1U, 1U));

  const auto age_quantiles =
      monitor.retire_log().ComputeAgeQuantiles(std::array{0., .5, 1.});
  ASSERT_TRUE(age_quantiles.has_value());
  EXPECT_THAT(*age_quantiles, testing::ElementsAre(kAge, kAge, kAge));
}

TEST_F(PipelineMonitorTest, TokensCanBeSplit) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  const size_t kSplits = 10;
  Token token_main = monitor.Issue(kSplits);

  const pw::chrono::SystemClock::duration kAge = std::chrono::milliseconds(10);
  RunFor(kAge);

  for (size_t i = 0; i < kSplits; i++) {
    Token split_token = token_main.Split(1);
    EXPECT_EQ(monitor.tokens_retired(), static_cast<int64_t>(i));
    if (i == kSplits - 1) {
      // token_main is moved to split_token when the final byte is taken.
      EXPECT_EQ(monitor.tokens_issued(),
                static_cast<int64_t>(i) +
                    1);  // split_token + ("i" previous split tokens)
    } else {
      EXPECT_EQ(
          monitor.tokens_issued(),
          static_cast<int64_t>(i) +
              2);  // token_main + split_token + ("i" previous split tokens)
    }
    EXPECT_EQ(monitor.bytes_retired(), i);
    EXPECT_EQ(kSplits - i, monitor.bytes_in_flight());
  }

  // Even though kSplits+1 Token objects were created, we should only see
  // kSplits retirements, which is how an PDU split into fragments for outbound
  // send would be modeled.
  EXPECT_EQ(static_cast<int64_t>(kSplits), monitor.tokens_retired());
  EXPECT_EQ(kSplits, monitor.bytes_retired());

  ASSERT_EQ(monitor.retire_log().depth(), kSplits);

  std::optional<std::array<size_t, 2>> byte_quantiles =
      monitor.retire_log().ComputeByteCountQuantiles(std::array{0., 1.});
  ASSERT_TRUE(byte_quantiles);
  EXPECT_EQ(byte_quantiles.value()[0], 1u);
  EXPECT_EQ(byte_quantiles.value()[1], 1u);

  std::optional<std::array<pw::chrono::SystemClock::duration, 2>>
      age_quantiles =
          monitor.retire_log().ComputeAgeQuantiles(std::array{0., 1.});
  ASSERT_TRUE(age_quantiles);
  EXPECT_EQ(age_quantiles.value()[0], kAge);
  EXPECT_EQ(age_quantiles.value()[1], kAge);
}

using PipelineMonitorDeathTest = PipelineMonitorTest;

TEST_F(PipelineMonitorDeathTest, SplittingTokenIntoMoreThanConstituentBytes) {
  PipelineMonitor monitor(dispatcher(), kRetireLogDefaultParams);

  auto token_main = monitor.Issue(1);

  EXPECT_DEATH(token_main.Split(2), "byte");
}

}  // namespace
}  // namespace bt
