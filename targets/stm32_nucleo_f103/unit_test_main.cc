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

#include <string_view>

#include "FreeRTOS.h"
#include "pw_boot/boot.h"
#include "pw_span/span.h"
#include "pw_sys_io/sys_io.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/simple_printing_event_handler.h"
#include "task.h"

namespace {

// 1,408 words (5,632 B) accommodates TestTask/RunAllTests (~560 B), tests that
// stack-allocate two 384-word TestThreadContexts (e.g. pw_async2:task_test at
// ~4,984 B), and SimplePrintingEventHandler::WriteLine (~440 B) while leaving
// >=1.2 KB for the MSP .stack in 20 KB SRAM.
constexpr size_t kTestTaskStackSizeWords = 1408;
StackType_t test_task_stack[kTestTaskStackSizeWords];
StaticTask_t test_task_buffer;

void TestTask(void*) {
  pw::unit_test::SimplePrintingEventHandler handler(
      [](std::string_view s, bool append_newline) {
        if (append_newline) {
          pw::sys_io::WriteLine(s).IgnoreError();
        } else {
          pw::sys_io::WriteBytes(pw::as_bytes(pw::span(s))).IgnoreError();
        }
      });
  pw::unit_test::RegisterEventHandler(&handler);
  const int status = RUN_ALL_TESTS();
  static_cast<void>(status);

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
