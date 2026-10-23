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

#include <array>
#include <cstdint>

#include "pw_allocator/bucket/unordered.h"
#include "pw_allocator/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/size_report/intrusive_forward_list.h"
#include "pw_containers/size_report/size_report.h"

namespace pw::allocator::size_report {

using ::pw::containers::size_report::kNumItems;
using ::pw::containers::size_report::MeasureIntrusiveForwardList;

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  if (int rc = MeasureBlock<BlockType>(mask); rc != 0) {
    return rc;
  }

  static std::array<UnorderedItem, kNumItems> items;
  if (int rc = MeasureIntrusiveForwardList<UnorderedItem>(
          items.begin(), items.end(), mask);
      rc != 0) {
    return rc;
  }

#ifndef PW_ALLOCATOR_SIZE_REPORT_BASE
  static UnorderedBucket<BlockType> bucket;
  if (int rc = MeasureBucket(bucket, mask); rc != 0) {
    return rc;
  }
#endif

  return 0;
}

}  // namespace pw::allocator::size_report

int main() { return pw::allocator::size_report::Measure(); }
