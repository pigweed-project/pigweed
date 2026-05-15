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

#include <openssl/base.h>
#include <openssl/ec.h>

namespace pw::crypto::ecdh::backend {

struct EcKeyDeleter final {
  void operator()(EC_KEY* key) { EC_KEY_free(key); }
};

#define DUPLICATE_NATIVE_P256 1

using NativeP256Keypair = std::unique_ptr<EC_KEY, EcKeyDeleter>;
using NativeP256PublicKey = std::unique_ptr<EC_KEY, EcKeyDeleter>;

}  // namespace pw::crypto::ecdh::backend
