/*
 * Copyright 2026 The Pigweed Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t pd_bit_t;

#define kPDRUNCFG_PD_LPOSC 0U  // Arbitrary value for mock
#define kPDRUNCFG_PD_FFRO 0U   // Arbitrary value for mock

void POWER_DisablePD(pd_bit_t enable_bit);
void POWER_EnablePD(pd_bit_t enable_bit);

#ifdef __cplusplus
}
#endif
