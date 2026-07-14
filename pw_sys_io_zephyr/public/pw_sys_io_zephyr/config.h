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
#pragma once

#include <type_traits>

#if defined(CONFIG_PIGWEED_SYS_IO)

// When Kconfig is in use, all configuration MUST come through Kconfig.
#if defined(PW_SYS_IO_ZEPHYR_CONFIG_HEADER) ||        \
    defined(PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY) || \
    defined(PW_SYS_IO_ZEPHYR_CONFIG_USB)
#error \
    "Do not define PW_SYS_IO_ZEPHYR_CONFIG options or PW_SYS_IO_ZEPHYR_CONFIG_HEADER when CONFIG_PIGWEED_SYS_IO is enabled in Kconfig. Configure via Kconfig instead."
#endif  // defined(PW_SYS_IO_ZEPHYR_CONFIG_...)

#include <zephyr/sys/util.h>

#define PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY \
  CONFIG_PIGWEED_SYS_IO_INIT_PRIORITY
#define PW_SYS_IO_ZEPHYR_CONFIG_USB IS_ENABLED(CONFIG_PIGWEED_SYS_IO_USB)

#else  // !defined(CONFIG_PIGWEED_SYS_IO)

// When Kconfig is not in use (e.g., standalone GN/Bazel builds), allow
// PW_SYS_IO_ZEPHYR_CONFIG_HEADER to provide overrides, falling back to C++
// defaults.
#if defined(PW_SYS_IO_ZEPHYR_CONFIG_HEADER)
#include PW_SYS_IO_ZEPHYR_CONFIG_HEADER
#endif  // defined(PW_SYS_IO_ZEPHYR_CONFIG_HEADER)

#ifndef PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY
#define PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY 1
#endif  // PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY
static_assert(
    std::is_integral_v<decltype(PW_SYS_IO_ZEPHYR_CONFIG_INIT_PRIORITY)>);

#ifndef PW_SYS_IO_ZEPHYR_CONFIG_USB
#define PW_SYS_IO_ZEPHYR_CONFIG_USB 0
#endif  // PW_SYS_IO_ZEPHYR_CONFIG_USB

#endif  // defined(CONFIG_PIGWEED_SYS_IO)

namespace pw::sys_io::zephyr::config {

inline constexpr bool kUseUsb = PW_SYS_IO_ZEPHYR_CONFIG_USB;

}  // namespace pw::sys_io::zephyr::config
