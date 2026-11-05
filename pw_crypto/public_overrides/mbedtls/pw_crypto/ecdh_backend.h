// Copyright 2025 The Pigweed Authors
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

#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>

#include "pw_bytes/span.h"
#include "pw_status/status.h"

namespace pw::crypto::ecdh::backend {
// A RAII wrapper for Mbed-TLS types.
template <typename Type, void (*Init)(Type*), void (*Free)(Type*)>
class Wrapper {
 public:
  Wrapper() { Init(&value_); }
  Wrapper(const Wrapper&) = delete;
  Wrapper& operator=(const Wrapper&) = delete;

  Wrapper(Wrapper&& other) {
    Init(&value_);
    std::swap(value_, other.value_);
  }

  Wrapper& operator=(Wrapper&& other) {
    std::swap(value_, other.value_);
    return *this;
  }

  ~Wrapper() { Free(&value_); }

  Type* Get() { return &value_; }
  const Type* Get() const { return &value_; }

 private:
  Type value_;
};

using Point =
    Wrapper<mbedtls_ecp_point, mbedtls_ecp_point_init, mbedtls_ecp_point_free>;
using Mpi = Wrapper<mbedtls_mpi, mbedtls_mpi_init, mbedtls_mpi_free>;

struct NativeP256PublicKey {
  Point point;
};

struct NativeP256Keypair {
  Point public_key;
  Mpi private_key;
};

/// Interface for clients to use to provide a CSPRNG for generating keypairs.
class Csprng {
 public:
  enum class GenerateResult : int {
    // No error occurred during Generate.
    kSuccess = 0,
    // An error occurred during Generate.
    kFailure = MBEDTLS_ERR_ECP_RANDOM_FAILED,
  };

  virtual ~Csprng() = 0;

  /// Fill the specified span with cryptographically secure random bytes.
  virtual GenerateResult Generate(ByteSpan bytes) = 0;
};

/// Set the CSPRNG to use. This must only be set once to initialize the library.
/// The backend does not take ownership of the CSPRNG, and it must remain valid
/// and alive through any and all calls to Keypair::Generate and/or
/// Keypair::ComputeDiffieHellman.
void SetCsprng(Csprng* csprng);

/// Reset the CSPRNG to be unset. This should only be used in tests.
void ResetCsprngForTesting();

}  // namespace pw::crypto::ecdh::backend
