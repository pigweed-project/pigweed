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

#include <openssl/base.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/nid.h>

#include "pw_crypto/ecdh.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::crypto::ecdh::backend {
namespace {
constexpr int kBsslSuccess = 1;

// Manage ownership and lifetime of a BoringSSL BIGNUM.
class Bignum {
 public:
  Bignum() { BN_init(&value_); }
  Bignum(const Bignum&) = delete;
  Bignum& operator=(const Bignum&) = delete;

  Bignum(Bignum&& other) {
    BN_init(&value_);
    std::swap(value_, other.value_);
  }

  Bignum& operator=(Bignum&& other) {
    std::swap(value_, other.value_);
    return *this;
  }

  ~Bignum() { BN_free(&value_); }

  BIGNUM* Get() { return &value_; }
  const BIGNUM* Get() const { return &value_; }

  static_assert(endian::native == endian::little ||
                    endian::native == endian::big,
                "Mixed endianness not supported.");

  Status Convert(span<std::byte> out, endian endianness) {
    auto do_convert =
        (endianness == endian::little) ? BN_bn2le_padded : BN_bn2bin_padded;

    if (do_convert(reinterpret_cast<uint8_t*>(out.data()),
                   out.size(),
                   &value_) != kBsslSuccess) {
      // This operation returns failure only if the buffer is too small.
      return Status::OutOfRange();
    }

    return OkStatus();
  }

  static Result<Bignum> From(span<const std::byte> in, endian endianness) {
    auto do_convert = (endianness == endian::little) ? BN_le2bn : BN_bin2bn;
    Bignum result;

    if (do_convert(reinterpret_cast<const uint8_t*>(in.data()),
                   in.size(),
                   result.Get()) == nullptr) {
      // This operation returns failure only on allocation failure.
      return Status::ResourceExhausted();
    }

    return result;
  }

 private:
  BIGNUM value_;
};

}  // namespace

Status DoGetX(const NativeP256Keypair& ctx,
              P256Coordinate out,
              endian endianness) {
  if (!ctx) {
    return Status::InvalidArgument();
  }

  Bignum x;
  if (EC_POINT_get_affine_coordinates(EC_KEY_get0_group(ctx.get()),
                                      EC_KEY_get0_public_key(ctx.get()),
                                      x.Get(),
                                      /*y=*/nullptr,
                                      /*ctx=*/nullptr) != kBsslSuccess) {
    return Status::Internal();
  }

  return x.Convert(out, endianness);
}

Status DoGetY(const NativeP256Keypair& ctx,
              P256Coordinate out,
              endian endianness) {
  if (!ctx) {
    return Status::InvalidArgument();
  }

  Bignum y;
  if (EC_POINT_get_affine_coordinates(EC_KEY_get0_group(ctx.get()),
                                      EC_KEY_get0_public_key(ctx.get()),
                                      /*x=*/nullptr,
                                      y.Get(),
                                      /*ctx=*/nullptr) != kBsslSuccess) {
    return Status::Internal();
  }

  return y.Convert(out, endianness);
  return OkStatus();
}

Status DoGenerate(NativeP256Keypair& ctx) {
  ctx = NativeP256Keypair(EC_KEY_new_by_curve_name(EC_curve_nist2nid("P-256")));
  if (!ctx) {
    return Status::Internal();
  }
  if (EC_KEY_generate_key(ctx.get()) != kBsslSuccess) {
    return Status::Internal();
  }
  return OkStatus();
}

Status DoImport(NativeP256Keypair& ctx,
                P256ConstPrivateKey private_key,
                P256ConstCoordinate x,
                P256ConstCoordinate y,
                endian endianness) {
  // Import the public key (This also initializes ctx).
  PW_TRY(DoImport(ctx, x, y, endianness));

  // Import the private key.
  PW_TRY_ASSIGN(Bignum private_value, Bignum::From(private_key, endianness));
  if (EC_KEY_set_private_key(ctx.get(), private_value.Get()) != kBsslSuccess) {
    return Status::Internal();
  }

  return OkStatus();
}

Status DoImport(NativeP256PublicKey& ctx,
                [[maybe_unused]] P256ConstCoordinate x,
                [[maybe_unused]] P256ConstCoordinate y,
                [[maybe_unused]] endian endianness) {
  ctx =
      NativeP256PublicKey(EC_KEY_new_by_curve_name(EC_curve_nist2nid("P-256")));
  if (!ctx) {
    return Status::Internal();
  }

  PW_TRY_ASSIGN(Bignum x_value, Bignum::From(x, endianness));
  PW_TRY_ASSIGN(Bignum y_value, Bignum::From(y, endianness));

  if (EC_KEY_set_public_key_affine_coordinates(
          ctx.get(), x_value.Get(), y_value.Get()) != kBsslSuccess) {
    return Status::Internal();
  }

  return OkStatus();
}

Status ComputeDiffieHellman(const NativeP256Keypair& key,
                            const NativeP256PublicKey& other_key,
                            P256DhKey out) {
  if (!key || !other_key) {
    return Status::InvalidArgument();
  }

  if (ECDH_compute_key(reinterpret_cast<uint8_t*>(out.data()),
                       out.size(),
                       EC_KEY_get0_public_key(other_key.get()),
                       key.get(),
                       /*kdf=*/nullptr) != static_cast<int>(out.size())) {
    return Status::Internal();
  }

  return OkStatus();
}

// No additional setup required for BoringSSL for testing.
void SetUpForTesting() {}

}  // namespace pw::crypto::ecdh::backend
