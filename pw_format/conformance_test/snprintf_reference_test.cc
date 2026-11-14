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

/// @file snprintf_reference_test.cc
///
/// Reference test that executes generated `TEST(PrintfConformance, <test_id>)`
/// cases from `printf_cases.yaml` (excluding cases with a `skip_snprintf`
/// reason) using typed, literal `std::snprintf` calls to cross-check that
/// conformance expectations match C standard library printf behavior.

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace pw::format::conformance {
namespace {

bool LibcUses0xPointerPrefix() {
  // Probe host C library '%p' formatting (ISO C11 7.21.6.1p8 allows
  // implementation-defined pointer representations, e.g., "0x1" on POSIX vs
  // uppercase 16-hex digits without a prefix on Windows MSVC UCRT).
  char ptr_probe[32];
  const int probe_len =
      std::snprintf(ptr_probe,
                    sizeof(ptr_probe),
                    "%p",
                    reinterpret_cast<const void*>(std::uintptr_t{1}));
  return probe_len == 3 && std::string_view(ptr_probe, 3) == "0x1";
}

}  // namespace
}  // namespace pw::format::conformance

#include "pw_format/conformance_test/printf_cases.h"
