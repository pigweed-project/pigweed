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

// Every RPC method gets a *method info* struct in its generated header, which
// names the method at compile time and describes it:
//
//   struct Echo : ::pw::rpc2::internal::MethodInfoTag {
//     static constexpr uint32_t kServiceId = 0x...;
//     static constexpr uint32_t kMethodId = 0x...;
//     static constexpr MethodType kType = MethodType::kUnary;
//     using Request = ...;
//     using Response = ...;
//   };
//
// APIs which operate on an arbitrary method are parameterized on this struct:
//
//   TestMethodContext<pw_rpc2::pwpb::EchoService::Echo, EchoServiceImpl> ctx;
//
// A method info struct is a name, not an object: it is never instantiated.

#include <type_traits>

#include "pw_rpc2/method_type.h"

namespace pw::rpc2::internal {

/// Marks a struct as a method info struct. Only codegen derives from this.
struct MethodInfoTag {};

/// Whether `T` is a generated method info struct. Provided so APIs can
/// `static_assert` on whether a template argument is a `MethodInfo`.
template <typename T>
inline constexpr bool kIsMethodInfo =
    std::is_base_of_v<MethodInfoTag, T> && !std::is_same_v<MethodInfoTag, T>;

}  // namespace pw::rpc2::internal
