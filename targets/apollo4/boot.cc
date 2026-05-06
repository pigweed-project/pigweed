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

#include "pw_boot_cortex_m/boot.h"

#include "pw_malloc/malloc.h"
#include "pw_memory/busy_wait_forever.h"
#include "pw_sys_io_ambiq_sdk/init.h"

PW_EXTERN_C void pw_boot_PreStaticMemoryInit() {}

PW_EXTERN_C void pw_boot_PreStaticConstructorInit() {
  if constexpr (PW_MALLOC_ACTIVE == 1) {
    pw_MallocInit(&pw_boot_heap_low_addr, &pw_boot_heap_high_addr);
  }

  pw_sys_io_Init();
}

PW_EXTERN_C void pw_boot_PreMainInit() {}

PW_EXTERN_C PW_NO_RETURN void pw_boot_PostMain() {
  // In case main() returns, just sit here until the device is reset.
  pw::BusyWaitForever();
}
