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

#include "pw_build_info/build_origin.h"

#include "pw_unit_test/framework.h"

namespace pw::build_info {
namespace {

TEST(BuildOrigin, Matches) {
  // Should match what's written by build_origin_test_workspace_status.sh
  EXPECT_STREQ(kBuildOrigin.c_str(), "test/8675309");
}

}  // namespace
}  // namespace pw::build_info
