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

#pragma once
#include <pw_assert/check.h>
#include <pw_chrono/system_clock.h>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace bt::internal {

// This class is not thread-safe.
// TODO(fxbug.dev/42150683): Store each retirement's timestamp in order to
// provide information like how much time the log depth represents and overall
// throughput (bytes/sec and packets/sec)
class RetireLog final {
 public:
  // Create a bounded buffer intended to hold recently retired PipelineMonitor
  // tokens. It supports efficient querying of statistics about logged events.
  // |min_depth| specifies the number of entries that must be logged before
  // queries return meaningful results and must be non-zero. |max_depth| limits
  // the number of recent entries that are kept in memory. Each entry is
  // represented by Retired, so the log memory use is roughly sizeof(Retired) *
  // max_depth. The designed range is between 100 and 100,000 entries deep.
  explicit RetireLog(size_t min_depth, size_t max_depth);

  ~RetireLog() = default;

  // Add an entry to the log. If depth() is less than max_depth, the log is
  // expanded. Otherwise, the oldest entry is replaced. This is a fast operation
  // that does not allocate.
  void Retire(size_t byte_count, pw::chrono::SystemClock::duration age);

  // The current number of log entries.
  [[nodiscard]] size_t depth() const { return buffer_.size(); }

  // Compute the quantiles at cut points specified in |partitions| as numbers
  // between 0 and 1. Each partition specifies a point in the distribution of
  // |bytes_count|s in the log. Returns an array of |byte_count| entries
  // corresponding to those points, if |depth()| is at least min_depth as
  // provided to the ctor. Otherwise, returns std::nullopt.
  //
  // Cut points may, but do not need to, be evenly distributed, e.g. {.25, .5,
  // .75} for quartiles. If a cut point is "between" log entry values, the
  // nearest value is chosen without interpolation (e.g. for 0.5 with an even
  // log depth, a biased median is returned rather than the average of the true
  // median samples).
  //
  // TODO(fxbug.dev/42150683): Add a |max_age| parameter to window to only
  // samples that are recent enough to be relevant
  template <size_t NumQuantiles>
  [[nodiscard]] std::optional<std::array<size_t, NumQuantiles>>
  ComputeByteCountQuantiles(std::array<double, NumQuantiles> partitions) const {
    return ComputeQuantiles(partitions, &Retired::byte_count);
  }

  // Similar to ComputeByteCountQuantiles, but for the |age| durations logged in
  // |Retire|.
  template <size_t NumQuantiles>
  [[nodiscard]] std::optional<
      std::array<pw::chrono::SystemClock::duration, NumQuantiles>>
  ComputeAgeQuantiles(std::array<double, NumQuantiles> partitions) const {
    return ComputeQuantiles(partitions, &Retired::age);
  }

 private:
  struct Retired {
    size_t byte_count;
    pw::chrono::SystemClock::duration age;
  };

  // Helper function to build the index_sequence
  template <typename ArrayT, typename PointerT>
  [[nodiscard]] auto ComputeQuantiles(ArrayT partitions,
                                      PointerT element_ptr) const {
    return ComputeQuantilesImpl(
        partitions, element_ptr, std::make_index_sequence<partitions.size()>());
  }

  // Computes the indexes to sample the retire log as if it were sorted, then
  // returns those samples in the same order as specified. |element_ptr|
  // specifies which field in Retired to sort by. The log isn't actually fully
  // sorted; instead, a k-selection algorithm is used for each sample. Assuming
  // partitions.size() << depth(), this runs on average linearly to their
  // product.
  template <size_t NumQuantiles, typename ElementT, size_t... Indexes>
  [[nodiscard]] std::optional<std::array<ElementT, NumQuantiles>>
  ComputeQuantilesImpl(std::array<double, NumQuantiles> partitions,
                       ElementT Retired::* element_ptr,
                       std::index_sequence<Indexes...> /*unused*/) const {
    if (depth() < min_depth_) {
      return std::nullopt;
    }

    // Computing quantiles is done in-place with k-selection routines, so use a
    // working copy that is reused across invocations to prevent re-allocation
    // with each call
    std::vector<ElementT>& elements =
        std::get<std::vector<ElementT>>(quantile_scratchpads_);
    elements.resize(depth());
    std::transform(buffer_.begin(),
                   buffer_.end(),
                   elements.begin(),
                   std::mem_fn(element_ptr));

    // The k-selection we use is std::nth_element, which conveniently does a
    // partial sort about k. By pre-sorting values of k, each invocation of
    // nth_element selects only from the elements that are greater than or equal
    // to the previous selection. The values are sorted with their corresponding
    // indexes to remember the original order for the output
    std::array partitions_and_indexes = {
        std::pair{partitions[Indexes], Indexes}...};
    std::sort(partitions_and_indexes.begin(), partitions_and_indexes.end());

    std::array<ElementT, NumQuantiles> quantiles;  // output
    auto unsorted_begin = elements.begin();
    for (auto [partition, index] : partitions_and_indexes) {
      PW_CHECK(partition >= 0.);
      PW_CHECK(partition <= 1.);
      // Even though the last element is at index depth()-1, use depth() here
      // instead to ensure the final (max) element has sufficient range
      // representation.
      const size_t cut_point =
          static_cast<size_t>(static_cast<double>(depth()) * partition);
      PW_CHECK(cut_point <= depth());

      // In the case that partition = 1.0, cut_point = depth(). Saturate to the
      // final (max) element.
      const size_t selection_offset = std::min(cut_point, depth() - 1);
      const auto cut_iter =
          std::next(elements.begin(), static_cast<ptrdiff_t>(selection_offset));
      std::nth_element(unsorted_begin, cut_iter, elements.end());
      // Post-condition: the element at cut_iter is what it would be if
      // |elements| were sorted, with all preceding elements no greater than
      // *cut_iter and all successive elements no less than *cut_iter.
      quantiles[index] = *cut_iter;

      // Technically the next unsorted element is at cut_iter+1 but moving
      // unsorted_begin past cut_iter causes problems if multiple values of
      // |partition| are the same.
      unsorted_begin = cut_iter;
    }
    return quantiles;
  }

  const size_t min_depth_;
  const size_t max_depth_;

  // Circular buffer of recently retired entries, kept in increasing retirement
  // time order starting at the oldest at |next_insertion_index_|. Bounded to
  // |max_depth_| in size.
  std::vector<Retired> buffer_;
  size_t next_insertion_index_ = 0;

  // Used by ComputeQuantilesImpl to store type-dependent k-selection
  // computation working data. We could probably save memory by using the same
  // scratchpad for both byte_count and age, but it's not worth the extra code
  // complexity at this time.
  mutable std::tuple<std::vector<size_t>,
                     std::vector<pw::chrono::SystemClock::duration>>
      quantile_scratchpads_;
};

}  // namespace bt::internal
