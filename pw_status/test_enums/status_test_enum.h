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

#include <cstdint>

#include "pw_enum/generate.h"

namespace pw::status_test {

enum class GeneratedEnum : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kPermissionDenied = 2,
};

enum class TestStatusCode : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kPermissionDenied = 2,
};

enum class NonZeroOkStatusCode {
  kBusy = 0,
  kReady = 1,
  kError = 2,
};

enum UnscopedGeneratedError {
  kUnscopedOk = 10,
  kUnscopedFailed = 20,
};

// Has a negative value, so it cannot share a word with a message token.
enum class SignedGeneratedError : int16_t {
  kOk = 0,
  kNegative = -1,
};

// The widest codes that can share a word with a message token: 16 bits.
enum class SixteenBitGeneratedError : uint16_t {
  kOk = 0,
  kLast = 65535,
};

// Needs 17 bits, so it cannot share a word with a message token.
enum class SeventeenBitGeneratedError : uint32_t {
  kOk = 0,
  kLast = 65536,
};

}  // namespace pw::status_test

PW_ENUM(pw::status_test::GeneratedEnum, kOk, kNotFound, kPermissionDenied);
PW_ENUM(pw::status_test::UnscopedGeneratedError, kUnscopedOk, kUnscopedFailed);
PW_ENUM(pw::status_test::SignedGeneratedError, kOk, kNegative);
PW_ENUM(pw::status_test::SixteenBitGeneratedError, kOk, kLast);
PW_ENUM(pw::status_test::SeventeenBitGeneratedError, kOk, kLast);
PW_ENUM_STATUS(pw::status_test::TestStatus,
               TestStatusCode::kOk,
               kOk,
               kNotFound,
               kPermissionDenied);
PW_ENUM_STATUS(pw::status_test::NonZeroOkStatus,
               pw::status_test::NonZeroOkStatusCode::kReady,
               kBusy,
               kReady,
               kError);
