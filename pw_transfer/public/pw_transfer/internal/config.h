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

// Configuration macros for the transfer module.
#pragma once

#include <chrono>
#include <cinttypes>
#include <limits>

#include "pw_chrono/system_clock.h"
#include "pw_preprocessor/compiler.h"

/// @module{pw_transfer}

/// Configurable log level for the `pw_transfer` module. Logs below this level
/// are omitted.
#ifndef PW_TRANSFER_CONFIG_LOG_LEVEL
#define PW_TRANSFER_CONFIG_LOG_LEVEL PW_LOG_LEVEL_INFO
#endif  // PW_TRANSFER_CONFIG_LOG_LEVEL

/// Turns on logging of individual non-data or non-parameter chunks. Defaults to
/// `0` (disabled). Data and parameter chunk logging has additional
/// configuration (`PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS`).
#ifndef PW_TRANSFER_CONFIG_DEBUG_CHUNKS
#define PW_TRANSFER_CONFIG_DEBUG_CHUNKS 0
#endif  // PW_TRANSFER_CONFIG_DEBUG_CHUNKS

/// Turns on logging of individual data and parameter chunks. Defaults to `0`
/// (disabled). These chunks are rate-limited by
/// `PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS`.
#ifndef PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS
#define PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS 0
#endif  // PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS

#ifdef PW_TRANSFER_DEFAULT_MAX_RETRIES
#pragma message(                                      \
    "PW_TRANSFER_DEFAULT_MAX_RETRIES is deprecated; " \
    "Use PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES and " \
    "PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES instead.")
#endif  // PW_TRANSFER_DEFAULT_MAX_RETRIES

#ifdef PW_TRANSFER_DEFAULT_TIMEOUT_MS
#pragma message(                                     \
    "PW_TRANSFER_DEFAULT_TIMEOUT_MS is deprecated; " \
    "Use PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS and " \
    "PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS instead.")
#endif  // PW_TRANSFER_DEFAULT_TIMEOUT_MS

/// The default maximum number of times a transfer client should retry sending a
/// chunk when no response is received. Can later be configured per-transfer
/// when starting one.
#ifndef PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES

// Continue to accept the old deprecated setting until projects have migrated.
#ifdef PW_TRANSFER_DEFAULT_MAX_RETRIES
#define PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES PW_TRANSFER_DEFAULT_MAX_RETRIES
#else
#define PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES 3
#endif  // PW_TRANSFER_DEFAULT_MAX_RETRIES

#endif  // PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES

static_assert(PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES >= 0 &&
              PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES <=
                  static_cast<uint32_t>(std::numeric_limits<uint8_t>::max()));

/// The default maximum number of times a transfer server should retry sending a
/// chunk when no response is received.
///
/// In typical setups, retries are driven by the client, and timeouts on the
/// server are used only to clean up resources, so this defaults to `0`.
#ifndef PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES

// Continue to accept the old deprecated setting until projects have migrated.
#ifdef PW_TRANSFER_DEFAULT_MAX_RETRIES
#define PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES PW_TRANSFER_DEFAULT_MAX_RETRIES
#else
#define PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES 0
#endif  // PW_TRANSFER_DEFAULT_MAX_RETRIES

#endif  // PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES

// GCC emits spurious -Wtype-limits warnings for the static_assert.
PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC_GCC(ignored, "-Wtype-limits");
static_assert(PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES >= 0 &&
              PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES <=
                  std::numeric_limits<uint8_t>::max());
PW_MODIFY_DIAGNOSTICS_POP();

/// The default maximum number of times a transfer should retry sending any
/// chunk over the course of its entire lifetime.
///
/// This number should be high, particularly if long-running transfers are
/// expected. Its purpose is to prevent transfers from getting stuck in an
/// infinite loop.
#ifndef PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES
#define PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES \
  (static_cast<uint32_t>(PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES) * 1000u)
#endif  // PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES

static_assert(PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES >
                  PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES &&
              PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES <=
                  std::numeric_limits<uint32_t>::max());

/// The default amount of time, in milliseconds, to wait for a chunk to arrive
/// in a transfer client before retrying. This can later be configured
/// per-transfer.
#ifndef PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS

// Continue to accept the old deprecated setting until projects have migrated.
#ifdef PW_TRANSFER_DEFAULT_TIMEOUT_MS
#define PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS PW_TRANSFER_DEFAULT_TIMEOUT_MS
#else
#define PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS 2000
#endif  // PW_TRANSFER_DEFAULT_TIMEOUT_MS

#endif  // PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS

static_assert(PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS > 0);

/// The default amount of time, in milliseconds, to wait for a chunk to arrive
/// on the server before retrying. This can later be configured per-transfer.
#ifndef PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS

// Continue to accept the old deprecated setting until projects have migrated.
#ifdef PW_TRANSFER_DEFAULT_TIMEOUT_MS
#define PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS PW_TRANSFER_DEFAULT_TIMEOUT_MS
#else
#define PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS \
  (static_cast<uint32_t>(PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS) * 5u)
#endif  // PW_TRANSFER_DEFAULT_TIMEOUT_MS

#endif  // PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS

static_assert(PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS > 0);

/// The default amount of time, in milliseconds, for a client to wait for an
/// initial response from the transfer server before retrying. This can later be
/// configured per-transfer.
///
/// This is set separately from `PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS` as
/// transfers may require additional time for resource initialization (e.g.
/// erasing a flash region before writing to it).
#ifndef PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS
#define PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS \
  PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS
#endif  // PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS

static_assert(PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS > 0);

/// The fractional position within a window at which a receive transfer should
/// extend its window size to minimize the amount of time the transmitter
/// spends blocked.
///
/// For example, a divisor of `2` will extend the window when half of the
/// requested data has been received, a divisor of `3` will extend at a third
/// of the window, and so on.
#ifndef PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR
#define PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR 2
#endif  // PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR

static_assert(PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR > 1);

/// Number of chunks to send repetitive logs at full rate before reducing to
/// rate-limited logging. Retransmit parameter chunks restart at this chunk
/// count limit.
///
/// Defaults to sending the first `10` parameter logs, then reducing to one log
/// every `PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS`.
#ifndef PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT
#define PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT 10
#endif  // PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT

static_assert(PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT > 0);

/// The minimum time, in milliseconds, between repetitive logs after the rate
/// limit has been applied (after
/// `PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT` parameter chunks).
///
/// Defaults to `10000` (once every 10 seconds).
#ifndef PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS
#define PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS 10000
#endif  // PW_TRANSFER_DEFAULT_MIN_RATE_PERIOD_MS

static_assert(PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS >= 0);

/// Maximum time, in milliseconds, to wait for a transfer event to be processed
/// before dropping further queued events. In systems which can perform
/// long-running operations to process transfer data, this can be used to
/// prevent threads from blocking for extended periods. A value of `0` results
/// in indefinite blocking.
#ifndef PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS
#define PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS \
  PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS
#endif  // PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS

/// @endmodule

static_assert(PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS >= 0);

namespace pw::transfer::cfg {

inline constexpr uint8_t kDefaultMaxClientRetries =
    PW_TRANSFER_DEFAULT_MAX_CLIENT_RETRIES;
inline constexpr uint8_t kDefaultMaxServerRetries =
    PW_TRANSFER_DEFAULT_MAX_SERVER_RETRIES;
inline constexpr uint16_t kDefaultMaxLifetimeRetries =
    PW_TRANSFER_DEFAULT_MAX_LIFETIME_RETRIES;

inline constexpr chrono::SystemClock::duration kDefaultClientTimeout =
    chrono::SystemClock::for_at_least(
        std::chrono::milliseconds(PW_TRANSFER_DEFAULT_CLIENT_TIMEOUT_MS));
inline constexpr chrono::SystemClock::duration kDefaultServerTimeout =
    chrono::SystemClock::for_at_least(
        std::chrono::milliseconds(PW_TRANSFER_DEFAULT_SERVER_TIMEOUT_MS));

inline constexpr chrono::SystemClock::duration kDefaultInitialChunkTimeout =
    chrono::SystemClock::for_at_least(
        std::chrono::milliseconds(PW_TRANSFER_DEFAULT_INITIAL_TIMEOUT_MS));

inline constexpr uint32_t kDefaultExtendWindowDivisor =
    PW_TRANSFER_DEFAULT_EXTEND_WINDOW_DIVISOR;

inline constexpr uint16_t kLogDefaultChunksBeforeRateLimit =
    PW_TRANSFER_LOG_DEFAULT_CHUNKS_BEFORE_RATE_LIMIT;
inline constexpr chrono::SystemClock::duration kLogDefaultRateLimit =
    chrono::SystemClock::for_at_least(
        std::chrono::milliseconds(PW_TRANSFER_LOG_DEFAULT_RATE_PERIOD_MS));

inline constexpr bool kWaitForEventProcessingIndefinitely =
    PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS == 0;
inline constexpr chrono::SystemClock::duration kEventProcessingTimeout =
    chrono::SystemClock::for_at_least(
        std::chrono::milliseconds(PW_TRANSFER_EVENT_PROCESSING_TIMEOUT_MS));

}  // namespace pw::transfer::cfg
