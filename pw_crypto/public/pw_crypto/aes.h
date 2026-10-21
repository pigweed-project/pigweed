// Copyright 2024 The Pigweed Authors
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

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_crypto/aes_backend.h"       // IWYU pragma: export
#include "pw_crypto/aes_backend_defs.h"  // IWYU pragma: export
#include "pw_log/log.h"
#include "pw_status/status.h"

namespace pw::crypto::aes {

/// Number of bytes in an AES block (16). This is independent of key size.
inline constexpr size_t kBlockSizeBytes = (128 / 8);
/// Number of bytes in a 128-bit key (16).
inline constexpr size_t kKey128SizeBytes = (128 / 8);
/// Number of bytes in a 192-bit key (24).
inline constexpr size_t kKey192SizeBytes = (192 / 8);
/// Number of bytes in a 256-bit key (32).
inline constexpr size_t kKey256SizeBytes = (256 / 8);

/// A single AES block.
using Block = std::array<std::byte, kBlockSizeBytes>;
/// A span of bytes the same size as an AES block.
using BlockSpan = span<std::byte, kBlockSizeBytes>;
/// A span of const bytes the same size as an AES block.
using ConstBlockSpan = span<const std::byte, kBlockSizeBytes>;

namespace internal {
/// Utility to get the appropriate `SupportedKeySize` from number of bytes.
constexpr backend::SupportedKeySize FromKeySizeBytes(size_t size) {
  switch (size) {
    case kKey128SizeBytes:
      return backend::SupportedKeySize::k128;
    case kKey192SizeBytes:
      return backend::SupportedKeySize::k192;
    case kKey256SizeBytes:
      return backend::SupportedKeySize::k256;
    default:
      return backend::SupportedKeySize::kUnsupported;
  }
}

template <backend::AesOperation op>
constexpr bool BackendSupports(backend::SupportedKeySize key_size) {
  return (backend::supported<op> & key_size) !=
         backend::SupportedKeySize::kUnsupported;
}

/// Utility to determine if an operation supports a particular key size.
template <backend::AesOperation op>
constexpr bool BackendSupports(size_t key_size_bytes) {
  return BackendSupports<op>(FromKeySizeBytes(key_size_bytes));
}

}  // namespace internal

namespace backend {

/// Initialize the backend context for `Cmac`.
Status DoInit(NativeCmacContext& ctx, ConstByteSpan key);
/// Update the backend context for `Cmac`.
Status DoUpdate(NativeCmacContext& ctx, ConstByteSpan data);
/// Finalize the backend context for `Cmac` and copy the resulting MAC to the
/// output.
Status DoFinal(NativeCmacContext& ctx, BlockSpan out_mac);

/// Implement `raw::EncryptBlock` in the backend. This function should not be
/// called directly, call `raw::EncryptBlock` instead.
///
/// @param[in] key A byte string containing the key to use to encrypt the block.
/// The key is guaranteed to be a length that is supported by the backend as
/// declared by `supports<kRawEncryptBlock>`.
///
/// @param[in] plaintext A 128-bit block of data to encrypt.
///
/// @param[in] out_ciphertext A 128-bit destination block in which to store the
/// encrypted data.
///
/// @return `pw::OkStatus()` for a successful encryption, or an error ``Status``
/// otherwise.
Status DoEncryptBlock(ConstByteSpan key,
                      ConstBlockSpan plaintext,
                      BlockSpan out_ciphertext);
}  // namespace backend

}  // namespace pw::crypto::aes
namespace pw::crypto::unsafe::aes {
/// Perform raw block-level AES encryption of a single AES block.
///
/// @warning This is a low-level operation that should be considered "unsafe" in
/// that users should know exactly what they are doing and must ensure that this
/// operation does not violate any safety bounds that more refined operations
/// usually ensure.
///
/// Example:
///
/// @code{.cpp}
/// #include "pw_crypto/aes.h"
///
/// // Encrypt a single block of data.
/// std::byte encrypted[16];
/// if (pw::crypto::aes::raw::EncryptBlock(key, message_block, encrypted)) {
///     // handle errors.
/// }
/// @endcode
///
/// @param[in] key A byte string containing the key to use to encrypt the block.
/// If `key` has a static extent then this will fail to compile if the key size
/// is not supported by the backend. If it has a dynamic extent, then this will
/// fail an assertion at runtime if it is not a supported size.
///
/// @param[in] plaintext A 128-bit block of data to encrypt.
///
/// @param[in] out_ciphertext A 128-bit destination block in which to store the
/// encrypted data.
///
/// @return `pw::OkStatus()` for a successful encryption, or an error ``Status``
/// otherwise.
template <size_t KeySize>
inline Status EncryptBlock(span<const std::byte, KeySize> key,
                           pw::crypto::aes::ConstBlockSpan plaintext,
                           pw::crypto::aes::BlockSpan out_ciphertext) {
  constexpr auto kThisOp =
      pw::crypto::aes::backend::AesOperation::kUnsafeEncryptBlock;
  static_assert(pw::crypto::aes::internal::BackendSupports<kThisOp>(KeySize),
                "Unsupported key size for EncryptBlock for backend.");
  return pw::crypto::aes::backend::DoEncryptBlock(
      key, plaintext, out_ciphertext);
}

// Specialization for dynamically sized spans.
template <>
inline Status EncryptBlock<dynamic_extent>(
    span<const std::byte, dynamic_extent> key,
    pw::crypto::aes::ConstBlockSpan plaintext,
    pw::crypto::aes::BlockSpan out_ciphertext) {
  constexpr auto kThisOp =
      pw::crypto::aes::backend::AesOperation::kUnsafeEncryptBlock;
  PW_ASSERT(pw::crypto::aes::internal::BackendSupports<kThisOp>(key.size()));
  return pw::crypto::aes::backend::DoEncryptBlock(
      key, plaintext, out_ciphertext);
}

}  // namespace pw::crypto::unsafe::aes
