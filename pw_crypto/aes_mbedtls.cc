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

#include "pw_crypto/aes_mbedtls.h"

#include <mbedtls/aes.h>
#include <mbedtls/cmac.h>

#include "pw_assert/check.h"
#include "pw_crypto/aes.h"
#include "pw_status/status.h"

namespace {
/// Number of bits in a byte.
constexpr size_t kBits = 8;
}  // namespace

namespace pw::crypto::aes::backend {
namespace {
using CmacCtx = NativeCmacContext;
}  // namespace

Status DoInit(CmacCtx& ctx, ConstByteSpan key) {
  const auto key_data = reinterpret_cast<const unsigned char*>(key.data());

  const mbedtls_cipher_info_t* info;
  switch (key.size()) {
    case kKey128SizeBytes:
      info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
      break;
    case kKey192SizeBytes:
      info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_192_ECB);
      break;
    case kKey256SizeBytes:
      info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_ECB);
      break;
    default:
      PW_CRASH("Unsupported key size for Cmac (%zu bit)", key.size() * kBits);
  }

  if (mbedtls_cipher_setup(&ctx.cipher, info)) {
    return Status::Internal();
  }
  if (mbedtls_cipher_cmac_starts(&ctx.cipher, key_data, key.size() * kBits)) {
    return Status::Internal();
  }

  return OkStatus();
}

Status DoUpdate(CmacCtx& ctx, ConstByteSpan data) {
  const auto data_in = reinterpret_cast<const unsigned char*>(data.data());
  if (mbedtls_cipher_cmac_update(&ctx.cipher, data_in, data.size())) {
    return Status::Internal();
  }

  return OkStatus();
}

Status DoFinal(CmacCtx& ctx, BlockSpan out_mac) {
  const auto data_out = reinterpret_cast<unsigned char*>(out_mac.data());
  if (mbedtls_cipher_cmac_finish(&ctx.cipher, data_out)) {
    return Status::Internal();
  }

  return OkStatus();
}

Status DoEncryptBlock(ConstByteSpan key,
                      ConstBlockSpan plaintext,
                      BlockSpan out_ciphertext) {
  const auto key_data = reinterpret_cast<const unsigned char*>(key.data());
  const auto in = reinterpret_cast<const unsigned char*>(plaintext.data());
  const auto out = reinterpret_cast<unsigned char*>(out_ciphertext.data());

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  if (mbedtls_aes_setkey_enc(
          &aes, key_data, static_cast<unsigned int>(key.size() * kBits))) {
    mbedtls_aes_free(&aes);
    return Status::Internal();
  }
  if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out)) {
    mbedtls_aes_free(&aes);
    return Status::Internal();
  }

  mbedtls_aes_free(&aes);
  return OkStatus();
}

}  // namespace pw::crypto::aes::backend
