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

// Every RPC method gets an empty, non-constructible tag type in its generated
// service namespace:
//
//   struct Echo final {
//     Echo() = delete;
//   };
//
// Generated code specializes `pw::rpc2::internal::MethodInfo<Method>` on this
// tag type to describe the method at compile time:
//
//   template <>
//   struct MethodInfo<pw_rpc2::pwpb::EchoService::Echo> {
//     static constexpr uint32_t kServiceId = 0x...;
//     static constexpr uint32_t kMethodId = 0x...;
//     static constexpr MethodType kType = MethodType::kUnary;
//     using Request = ...;
//     using Response = ...;
//   };
//
// APIs which operate on an arbitrary method are parameterized on the tag:
//
//   auto call = peer.ExpectInvocation<pw_rpc2::pwpb::EchoService::Echo>();

#include "pw_rpc2/method_type.h"

namespace pw::rpc2::internal {

template <typename>
inline constexpr bool kIsRpcMethod = false;

/// Compile-time metadata for a generated RPC method tag. Specialized by
/// generated code for each RPC method.
template <typename Method>
struct MethodInfo {
  static_assert(kIsRpcMethod<Method>,
                "Template argument must be a generated pw_rpc2 method tag, "
                "such as my_pkg::pw_rpc2::pwpb::MyService::MyMethod.");
};

}  // namespace pw::rpc2::internal
