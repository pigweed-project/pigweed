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

#include "pw_log/log.h"
#include "pw_thread_zephyr/context.h"
#include "pw_thread_zephyr/options.h"
#include "targets/pw_rp2350/unit_test_server.h"

namespace {
pw::thread::zephyr::ContextWithStack<4096> unittest_thread_context;
}  // namespace

int main() {
  PW_LOG_INFO("Starting Zephyr Unit Test Server");

  pw::thread::zephyr::Options unittest_options(unittest_thread_context);
  unittest_options.set_name("unittest");
  unittest_options.set_priority(K_PRIO_PREEMPT(1));

  pw::unit_test::StartUnittestServer(unittest_options);

  return 0;
}
