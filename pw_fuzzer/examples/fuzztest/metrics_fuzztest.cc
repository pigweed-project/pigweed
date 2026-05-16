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

#include <algorithm>
#include <string_view>
#include <vector>

#include "metrics.h"
#include "pw_fuzzer/fuzztest.h"
#include "pw_unit_test/framework.h"

namespace pw::fuzzer::examples {

// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest1]
void ArbitrarySerializeAndDeserialize(const Vector<Metric>& metrics) {
  std::array<std::byte, Metrics::kMaxSerializedSize> buffer;

  // Add and copy the names only.
  Metrics src, dst;
  for (const auto& metric : metrics) {
    EXPECT_TRUE(src.SetValue(metric.name, 0).ok());
  }
  EXPECT_TRUE(dst.SetMetrics(src.GetMetrics()).ok());

  // Modify the values.
  for (const auto& metric : metrics) {
    EXPECT_TRUE(src.SetValue(metric.name, metric.value).ok());
  }

  // Transfer the data and check.
  EXPECT_TRUE(src.Serialize(buffer).ok());
  EXPECT_TRUE(dst.Deserialize(buffer).ok());
  for (const auto& metric : metrics) {
    auto src_value = src.GetValue(metric.name);
    ASSERT_TRUE(src_value.has_value());

    auto dst_value = dst.GetValue(metric.name);
    ASSERT_TRUE(dst_value.has_value());

    EXPECT_EQ(*dst_value, *src_value);
  }
}

// This unit test will run on host and may run on target devices (if supported).
TEST(MetricsTest, SerializeAndDeserialize) {
  Vector<Metric, 3> metrics;
  metrics.emplace_back("one", 1u);
  metrics.emplace_back("two", 2u);
  metrics.emplace_back("three", 3u);
  ArbitrarySerializeAndDeserialize(metrics);
}
// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest1]

// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest2]
auto ArbitraryMetric() {
  return ConstructorOf<Metric>(PrintableAsciiString<Metric::kMaxNameLen>(),
                               Arbitrary<uint32_t>());
}

// This fuzz test will only run on host.
FUZZ_TEST(MetricsTest, ArbitrarySerializeAndDeserialize)
    .WithDomains(VectorOf<Metrics::kMaxMetrics>(ArbitraryMetric()));
// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest2]

// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest3]
void ArbitraryDeserialize(pw::ConstByteSpan buffer) {
  // Just make sure this does not crash.
  Metrics dst;
  dst.Deserialize(buffer).IgnoreError();
}

// This unit test will run on host and may run on target devices (if supported).
TEST(MetricsTest, DeserializeDoesNotCrash) {
  ArbitraryDeserialize(std::vector<std::byte>(100, std::byte(0x5C)));
}
// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest3]

// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest4]
// This fuzz test will only run on host.
FUZZ_TEST(MetricsTest, ArbitraryDeserialize)
    .WithDomains(VectorOf<Metrics::kMaxSerializedSize>(Arbitrary<std::byte>()));
// DOCSTAG: [pwfuzzer_examples_fuzztest-metrics_fuzztest4]

}  // namespace pw::fuzzer::examples
