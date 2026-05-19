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

#include <mbedtls/chacha20.h>

#include "pw_bytes/span.h"
#include "pw_crypto/chacha20_backend.h"
#include "pw_status/status.h"

namespace pw::crypto::chacha20 {
namespace backend {

Status DoChaCha20Crypt(span<const std::byte> key,
                       span<const std::byte> nonce,
                       span<const std::byte> input,
                       span<std::byte> output) {
  int ret = mbedtls_chacha20_crypt(
      reinterpret_cast<const unsigned char*>(key.data()),
      reinterpret_cast<const unsigned char*>(nonce.data()),
      0,
      input.size(),
      reinterpret_cast<const unsigned char*>(input.data()),
      reinterpret_cast<unsigned char*>(output.data()));

  return (ret != 0) ? Status::Internal() : OkStatus();
}

}  // namespace backend
}  // namespace pw::crypto::chacha20
