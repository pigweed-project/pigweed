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
#pragma once

#include <optional>

#include "pw_assert/check.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_metric/list.h"
#include "pw_metric/metric.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::metric {

// Streaming RPC Writer
class MetricWriter {
 public:
  virtual ~MetricWriter() = default;
  virtual Status Write(const UntypedMetric& metric,
                       const Vector<Token>& path) = 0;
};

// Walk a metric tree recursively; passing metrics with their path (names) to a
// MetricWriter that can consume them.
class MetricWalker {
 public:
  MetricWalker(MetricWriter& writer) : writer_(writer) {}

  Status Walk(const MetricList& metrics) {
    for (const auto& m : metrics.list()) {
      ScopedName scoped_name(m.name(), *this);
      PW_TRY(writer_.Write(m, path_));
    }
    return OkStatus();
  }

  Status Walk(const GroupList& groups) {
    for (const auto& g : groups.list()) {
      PW_TRY(Walk(g));
    }
    return OkStatus();
  }

  Status Walk(const Group& group) {
    ScopedName scoped_name(group.name(), *this);
    PW_TRY(Walk(group.children()));
    PW_TRY(Walk(group.metrics()));
    return OkStatus();
  }

 private:
  // Exists to safely push/pop parent groups from the explicit stack.
  struct ScopedName {
    ScopedName(Token name, MetricWalker& rhs) : walker(rhs) {
      // Metrics are too deep; bump path_ capacity.
      PW_ASSERT(walker.path_.size() < walker.path_.capacity());
      walker.path_.push_back(name);
    }
    ~ScopedName() { walker.path_.pop_back(); }
    MetricWalker& walker;
  };

  Vector<Token, /*capacity=*/4> path_;
  MetricWriter& writer_;
};

// A metric writer for the paginated Walk RPC.
class UnaryMetricWriter {
 public:
  virtual ~UnaryMetricWriter() = default;

  // `Write` returns `RESOURCE_EXHAUSTED` to signal a full buffer.
  virtual Status Write(const UntypedMetric& metric,
                       const Vector<Token>& path) = 0;
};

// A walker that can be resumed from a cursor (address).
class ResumableMetricWalker {
 public:
  ResumableMetricWalker(UnaryMetricWriter& writer);

  // Walks the metrics and groups, starting from the metric at the provided
  // cursor address. Returns the address of the next metric, or NOT_FOUND if
  // the provided cursor is invalid.
  Result<uint64_t> Walk(const MetricList& metrics,
                        const GroupList& groups,
                        std::optional<uint64_t> cursor);

  // When Walk() returns RESOURCE_EXHAUSTED, this method provides the cursor
  // for the next page.
  uint64_t next_cursor() const { return next_cursor_; }

 private:
  // Forward declaration for the private implementation detail.
  struct ScopedName;

  // Helper that recursively walks the metrics and groups.
  Status RecursiveWalkHelper(const MetricList& metrics,
                             const GroupList& groups);

  Vector<Token, /*capacity=*/4> path_;
  UnaryMetricWriter& writer_;

  // State for the walk, stored as members to avoid large lambda captures.
  uint64_t start_cursor_ = 0;
  bool writing_phase_ = false;
  uint64_t next_cursor_ = 0;
};

}  // namespace pw::metric
