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
#include "pw_unit_test/framework.h"
#include "pw_unit_test/logging_event_handler.h"
#include "task.h"

namespace {

constexpr size_t kTestTaskStackSizeWords = 2048;
StackType_t test_task_stack[kTestTaskStackSizeWords];
StaticTask_t test_task_buffer;

void TestTask(void*) {
  pw::unit_test::LoggingEventHandler handler;
  pw::unit_test::RegisterEventHandler(&handler);
  int status = RUN_ALL_TESTS();
  (void)status;

  pw_boot_PostMain();
}

}  // namespace

int main() {
  int argc = 0;
  char* argv[] = {nullptr};
  testing::InitGoogleTest(&argc, argv);

  xTaskCreateStatic(TestTask,
                    "test_task",
                    kTestTaskStackSizeWords,
                    nullptr,
                    tskIDLE_PRIORITY + 1,
                    test_task_stack,
                    &test_task_buffer);

  vTaskStartScheduler();
  pw_boot_PostMain();
}
