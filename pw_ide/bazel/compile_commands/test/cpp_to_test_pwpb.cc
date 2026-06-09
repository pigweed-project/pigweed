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

#include "pw_ide/bazel/compile_commands/test/cpp_to_test_pwpb.h"

#include "pw_ide/bazel/compile_commands/test/test.pwpb.h"

void test_fun() {
  pw::ide::test::pwpb::TestMessage::Message msg;
  (void)msg;
}
