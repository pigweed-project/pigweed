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

#include <cstddef>
#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_crypto/chacha20_backend.h"
#include "pw_status/status.h"

namespace pw::crypto {

/// ChaCha20 stream cipher.
///
/// This module provides a backend-agnostic API for the ChaCha20 stream cipher
/// as defined in RFC 8439.
namespace chacha20 {

inline constexpr size_t kKeySizeBytes = 32;    // 256 bits
inline constexpr size_t kNonceSizeBytes = 12;  // 96 bits

/// Performs ChaCha20 encryption or decryption. Since ChaCha20 is a stream
/// cipher, encryption and decryption are the same operation.
/// InitialCounter had been removed from the cacnonical API and is hardcoded
/// to 0 for simplicity of a one-shot only implementation.
///
/// @param[in] key The 256-bit secret key.
/// @param[in] nonce The 96-bit nonce. MUST be unique for each encryption with
/// the same key.
/// @param[in] input: The data to encrypt or decrypt.
/// @param[out] output: The buffer to store the result. Must be at least as
/// large as input. In-place operation is allowed (input == output).
///
/// @returns `OkStatus()` on success. or `Status::InvalidArgument()` if key,
/// nonce, or buffer sizes are incorrect. `Status::Internal()` for any other
/// backend-related errors.
inline Status Crypt(span<const std::byte> key,
                    span<const std::byte> nonce,
                    span<const std::byte> input,
                    span<std::byte> output) {
  if (key.size() != kKeySizeBytes) {
    return Status::InvalidArgument();
  }
  if (nonce.size() != kNonceSizeBytes) {
    return Status::InvalidArgument();
  }
  if (output.size() < input.size()) {
    return Status::InvalidArgument();
  }

  return backend::DoChaCha20Crypt(key, nonce, input, output);
}

}  // namespace chacha20
}  // namespace pw::crypto
