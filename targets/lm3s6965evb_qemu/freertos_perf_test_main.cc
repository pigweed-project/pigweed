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

#include "FreeRTOS.h"
#include "pw_boot/boot.h"
#include "pw_perf_test/logging_event_handler.h"
#include "pw_perf_test/perf_test.h"
#include "task.h"

namespace {

constexpr size_t kPerfTaskStackSizeWords = 2048;
StackType_t perf_task_stack[kPerfTaskStackSizeWords];
StaticTask_t perf_task_buffer;

void PerfTask(void*) {
  pw::perf_test::LoggingEventHandler handler;
  pw::perf_test::RunAllTests(handler);

  pw_boot_PostMain();
}

}  // namespace

int main() {
  xTaskCreateStatic(PerfTask,
                    "perf_task",
                    kPerfTaskStackSizeWords,
                    nullptr,
                    tskIDLE_PRIORITY + 1,
                    perf_task_stack,
                    &perf_task_buffer);

  vTaskStartScheduler();
  pw_boot_PostMain();
}
