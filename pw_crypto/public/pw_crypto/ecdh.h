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

#include "pw_bytes/endian.h"
#include "pw_crypto/ecdh_backend.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::crypto::ecdh {

constexpr size_t kP256CoordSize = 256 / 8;  // 32-byte uncompressed coordinates.
constexpr size_t kP256DiffieHellmanKeySize = 256 / 8;  // 32-byte key.
constexpr size_t kP256PrivateKeySize = 256 / 8;        // 32-byte private key.

using P256Coordinate = pw::span<std::byte, kP256CoordSize>;
using P256ConstCoordinate = pw::span<const std::byte, kP256CoordSize>;
using P256DhKey = pw::span<std::byte, kP256DiffieHellmanKeySize>;
using P256ConstDhKey = pw::span<const std::byte, kP256DiffieHellmanKeySize>;
using P256PrivateKey = pw::span<std::byte, kP256PrivateKeySize>;
using P256ConstPrivateKey = pw::span<const std::byte, kP256PrivateKeySize>;

namespace backend {

// Backends must implement all of these operations.

/// Get X coordinate from a keypair.
Status DoGetX(const NativeP256Keypair& ctx,
              P256Coordinate out,
              endian endianness);
/// Get X coordinate from a public key.
#ifndef DUPLICATE_NATIVE_P256
Status DoGetX(const NativeP256PublicKey& ctx,
              P256Coordinate out,
              endian endianness);
#endif
/// Get Y coordinate from a keypair.
Status DoGetY(const NativeP256Keypair& ctx,
              P256Coordinate out,
              endian endianness);
/// Get Y coordinate from a public key.
#ifndef DUPLICATE_NATIVE_P256
Status DoGetY(const NativeP256PublicKey& ctx,
              P256Coordinate out,
              endian endianness);
#endif
/// Generate a keypair.
Status DoGenerate(NativeP256Keypair& ctx);
/// Convert an externally-generated private key into a native keypair.
Status DoImport(NativeP256Keypair& ctx,
                P256ConstPrivateKey key,
                P256ConstCoordinate x,
                P256ConstCoordinate y,
                endian endianness);
/// Convert public key information into a native public key.
Status DoImport(NativeP256PublicKey& ctx,
                P256ConstCoordinate x,
                P256ConstCoordinate y,
                endian endianness);
/// Compute a symmetric key using ECDH.
Status ComputeDiffieHellman(const NativeP256Keypair& key,
                            const NativeP256PublicKey& other_key,
                            P256DhKey out);
void SetUpForTesting();

}  // namespace backend

/// Operations that are supported on a public key. This is a pure virtual
/// interface as keypairs (containing a private key) contain a public key, but
/// a backend may use separate types to represent just a public key vs. a full
/// keypair.
class P256PublicKeyOps {
 public:
  virtual ~P256PublicKeyOps() = default;

  /// Get the X coordinate of the public key as a 256-bit integer in the
  /// specified endianness.
  virtual Status GetX(P256Coordinate buffer, endian endianness) const = 0;
  Status GetX(P256Coordinate buffer) const { return GetX(buffer, endian::big); }

  /// Get the Y coordinate of the public key as a 256-bit integer in the
  /// specified endianness.
  virtual Status GetY(P256Coordinate buffer, endian endianness) const = 0;
  Status GetY(P256Coordinate buffer) const { return GetY(buffer, endian::big); }
};

/// A public key for ECDH using the P256 curve. Contains an X and Y coordinate.
class P256PublicKey final : public P256PublicKeyOps {
 public:
  P256PublicKey(const P256PublicKey&) = delete;
  P256PublicKey(P256PublicKey&&) = default;
  P256PublicKey& operator=(const P256PublicKey&) = delete;
  P256PublicKey& operator=(P256PublicKey&&) = default;

  /// Import a public key, converting it to a P256PublicKey.
  static Result<P256PublicKey> Import(P256ConstCoordinate x,
                                      P256ConstCoordinate y,
                                      endian endianness = endian::big) {
    backend::NativeP256PublicKey native;
    PW_TRY(backend::DoImport(native, x, y, endianness));
    return P256PublicKey(std::move(native));
  }

  Status GetX(P256Coordinate out, endian endianness) const override {
    return backend::DoGetX(native_, out, endianness);
  }

  Status GetY(P256Coordinate out, endian endianness) const override {
    return backend::DoGetY(native_, out, endianness);
  }

  Status GetX(P256Coordinate buffer) const { return GetX(buffer, endian::big); }
  Status GetY(P256Coordinate buffer) const { return GetY(buffer, endian::big); }

 private:
  friend class P256Keypair;
  explicit P256PublicKey(backend::NativeP256PublicKey&& native)
      : native_(std::move(native)) {}

  // Backend-specific type.
  backend::NativeP256PublicKey native_;
};

/// A key pair for ECDH using the P256 curve.
class P256Keypair final : public P256PublicKeyOps {
 public:
  P256Keypair(const P256Keypair&) = delete;
  P256Keypair(P256Keypair&&) = default;
  P256Keypair& operator=(const P256Keypair&) = delete;
  P256Keypair& operator=(P256Keypair&&) = default;

  /// Generate a new key pair using a backend-specific generator. The backend
  /// must be set up to support cryptographically secure random number
  /// generation with sufficient entropy.
  static pw::Result<P256Keypair> Generate() {
    backend::NativeP256Keypair native;
    PW_TRY(backend::DoGenerate(native));
    return P256Keypair(std::move(native));
  }

  /// Import a private key, creating a new P256Keypair containing the private
  /// key and associated public key. Intended for testing only.
  static pw::Result<P256Keypair> ImportForTesting(
      P256ConstPrivateKey key,
      P256ConstCoordinate x,
      P256ConstCoordinate y,
      endian endianness = endian::big) {
    backend::NativeP256Keypair native;
    PW_TRY(backend::DoImport(native, key, x, y, endianness));
    return P256Keypair(std::move(native));
  }

  Status GetX(P256Coordinate out, endian endianness) const override {
    return backend::DoGetX(native_, out, endianness);
  }

  Status GetY(P256Coordinate out, endian endianness) const override {
    return backend::DoGetY(native_, out, endianness);
  }

  Status GetX(P256Coordinate buffer) const { return GetX(buffer, endian::big); }
  Status GetY(P256Coordinate buffer) const { return GetY(buffer, endian::big); }

  /// Compute a symmetric key using ECDH.
  Status ComputeDiffieHellman(const P256PublicKey& other_key,
                              P256DhKey out) const {
    return backend::ComputeDiffieHellman(native_, other_key.native_, out);
  }

 private:
  explicit P256Keypair(backend::NativeP256Keypair&& native)
      : native_(std::move(native)) {}

  // Backend-specific type.
  backend::NativeP256Keypair native_;
};

/// Configure the ECDH backend for testing.
/// WARNING: Production code MUST NEVER call this!
inline void SetUpBackendForTesting() { backend::SetUpForTesting(); }

}  // namespace pw::crypto::ecdh
