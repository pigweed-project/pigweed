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

#include <iterator>

#include "FreeRTOS.h"
#include "pico/stdlib.h"
#include "pw_log/log.h"
#include "pw_thread_freertos/context.h"
#include "pw_thread_freertos/options.h"
#include "targets/pw_rp2350/unit_test_server.h"
#include "task.h"

namespace pw::unit_test {

thread::freertos::StaticContextWithStack<4096> unit_test_thread_context;

const thread::Options& UnitTestThreadOptions() {
  static constexpr auto kOptions =
      thread::freertos::Options()
          .set_name("UnitTestThread")
          .set_static_context(unit_test_thread_context)
          .set_priority(tskIDLE_PRIORITY + 1);
  return kOptions;
}

constexpr size_t kRpcTaskStackSizeWords = 1024;
static_assert(kRpcTaskStackSizeWords > configMINIMAL_STACK_SIZE,
              "RPC stack size must be greater than configMINIMAL_STACK_SIZE");

StackType_t rpc_task_stack[kRpcTaskStackSizeWords];
StaticTask_t rpc_task_buffer;

void RpcTask(void*) {
  StartUnittestServer(UnitTestThreadOptions());
  vTaskDelete(nullptr);
}

}  // namespace pw::unit_test

int main() {
  stdio_init_all();
  PW_LOG_INFO("Starting FreeRTOS Unit Test Server");

  xTaskCreateStatic(pw::unit_test::RpcTask,
                    "rpc_task",
                    std::size(pw::unit_test::rpc_task_stack),
                    nullptr,
                    tskIDLE_PRIORITY + 2,
                    pw::unit_test::rpc_task_stack,
                    &pw::unit_test::rpc_task_buffer);

  vTaskStartScheduler();
  PW_UNREACHABLE;
}
