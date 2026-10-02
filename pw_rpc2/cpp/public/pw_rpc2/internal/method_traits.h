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

// Compile-time classification of RPC method signatures.
//
// This header is pure type computation: it maps the signature a service
// implementation wrote onto the RPC shape it describes, and maps an RPC shape
// back onto the handle types a method of that shape receives. It depends only
// on `MethodType` and the handle types, never on the call runtime, so the
// rules that decide "is this a valid RPC method?" can be read --- and tested
// --- without the invocation machinery in `method_invoker.h`.
//
// Only `MethodTraits` and `InvocationTraits` are used outside this header; the
// rest of the classification is in `namespace detail`.

#include <type_traits>

#include "lib/stdcompat/type_traits.h"
#include "pw_buf/buf.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"

namespace pw::async2 {
class CoroContext;
}  // namespace pw::async2

namespace pw::rpc2::internal {
namespace detail {

// ============================================================================
// RPC method signature classification
// ============================================================================

/// Classifies the request/response arguments of an RPC method. `Args` must
/// already have cv-qualifiers and references stripped, so that a request
/// passed as `const Req&` and one passed as `Req` by value classify
/// identically.
///
/// The primary template covers everything which is not a recognized RPC
/// signature.
template <typename... Args>
struct RpcArgTraits {
  static constexpr bool kValid = false;
};

/// `(request, UnaryWriter<Resp>)` is a unary method.
template <typename Req, typename Resp>
struct RpcArgTraits<Req, UnaryWriter<Resp>> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kUnary;
  using Request = Req;
  using Response = Resp;
};

template <typename Req>
struct RpcArgTraits<Req, RawUnaryWriter> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kUnary;
  using Request = Req;
  using Response = ConstBuf;
};

/// `(request, Writer<Resp>)` is a server streaming method.
template <typename Req, typename Resp>
struct RpcArgTraits<Req, Writer<Resp>> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kServerStreaming;
  using Request = Req;
  using Response = Resp;
};

template <typename Req>
struct RpcArgTraits<Req, RawWriter> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kServerStreaming;
  using Request = Req;
  using Response = ConstBuf;
};

/// `(Reader<Req>, UnaryWriter<Resp>)` is a client streaming method. This is
/// more specialized than the unary case above, so it is preferred for readers.
template <typename Req, typename Resp>
struct RpcArgTraits<Reader<Req>, UnaryWriter<Resp>> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kClientStreaming;
  using Request = Req;
  using Response = Resp;
};

template <typename Req>
struct RpcArgTraits<Reader<Req>, RawUnaryWriter> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kClientStreaming;
  using Request = Req;
  using Response = ConstBuf;
};

/// `(Reader<Req>, Writer<Resp>)` is a bidirectional streaming method.
template <typename Req, typename Resp>
struct RpcArgTraits<Reader<Req>, Writer<Resp>> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kBidirectionalStreaming;
  using Request = Req;
  using Response = Resp;
};

template <typename Req>
struct RpcArgTraits<Reader<Req>, RawWriter> {
  static constexpr bool kValid = true;
  static constexpr MethodType kType = MethodType::kBidirectionalStreaming;
  using Request = Req;
  using Response = ConstBuf;
};

/// Strips a leading `async2::CoroContext` argument, if present, then
/// classifies the remaining arguments.
template <typename... Args>
struct StripCoroContext : RpcArgTraits<Args...> {
  static constexpr bool kTakesCoroContext = false;
};

template <typename... Args>
struct StripCoroContext<async2::CoroContext, Args...> : RpcArgTraits<Args...> {
  static constexpr bool kTakesCoroContext = true;
};

/// Identifies the call handles that a method implementation takes ownership of.
template <typename T>
struct IsRpcHandle : std::false_type {};
template <typename T>
struct IsRpcHandle<Reader<T>> : std::true_type {};
template <typename T>
struct IsRpcHandle<Writer<T>> : std::true_type {};
template <>
struct IsRpcHandle<RawWriter> : std::true_type {};
template <typename T>
struct IsRpcHandle<UnaryWriter<T>> : std::true_type {};
template <>
struct IsRpcHandle<RawUnaryWriter> : std::true_type {};

/// Normalizes one argument of an RPC method implementation for classification.
///
/// The request may be taken by value or as a `const Req&`, so cv-qualifiers
/// and references are stripped from it and both forms classify identically.
/// The reader and writer handles are moved into the method, so they must
/// be taken by value: a reference to one is left as-is, matches no signature,
/// and is reported by `MethodInvoker`'s static_assert rather than failing
/// later inside the invocation machinery.
template <typename T>
using NormalizeRpcArg =
    std::conditional_t<IsRpcHandle<cpp20::remove_cvref_t<T>>::value,
                       T,
                       cpp20::remove_cvref_t<T>>;

/// Classifies the argument list of an RPC method implementation.
template <typename... Args>
using RpcArgs = StripCoroContext<NormalizeRpcArg<Args>...>;

/// Whether the method's request argument was declared as a reference.
///
/// `NormalizeRpcArg` strips references before classification, so this inspects
/// the argument list as the user wrote it. A reference request is safe for a
/// future (whose constructor runs immediately, while the request is still
/// alive) but not for a coroutine, whose body does not run until the first
/// poll; see the assertion in `MethodInvoker::InvokeIntoCall`.
template <typename... Args>
struct RequestIsReference : std::false_type {};

template <typename First, typename... Rest>
struct RequestIsReference<First, Rest...> : std::is_reference<First> {};

template <typename... Rest>
struct RequestIsReference<async2::CoroContext, Rest...>
    : RequestIsReference<Rest...> {};

template <typename Svc,
          typename Fut,
          typename Req,
          typename Resp,
          MethodType Type,
          bool TakesCoro>
struct BaseMethodTraits {
  static constexpr bool kValid = true;
  static constexpr bool kTakesCoroContext = TakesCoro;
  static constexpr MethodType kType = Type;
  static constexpr bool kIsRaw =
      std::is_same_v<cpp20::remove_cvref_t<Req>, ConstBuf> &&
      std::is_same_v<cpp20::remove_cvref_t<Resp>, ConstBuf>;
  using Service = Svc;
  using Request = cpp20::remove_cvref_t<Req>;
  using Response = Resp;
  using Future = Fut;
};

}  // namespace detail

/// Describes the RPC a service implementation's member function implements:
/// its method type, its request and response types, whether it is a raw
/// (`pw::ConstBuf`) method, and the future it returns.
///
/// `kValid` is false for any signature which is not a recognized RPC method.
template <typename T, typename = void>
struct MethodTraits {
  static constexpr bool kValid = false;
  static constexpr bool kIsRaw = false;
  static constexpr bool kTakesCoroContext = false;
  using Future = void;
};

// Non-const member functions.
template <typename Svc, typename Fut, typename... Args>
struct MethodTraits<Fut (Svc::*)(Args...),
                    std::enable_if_t<detail::RpcArgs<Args...>::kValid>>
    : detail::BaseMethodTraits<Svc,
                               Fut,
                               typename detail::RpcArgs<Args...>::Request,
                               typename detail::RpcArgs<Args...>::Response,
                               detail::RpcArgs<Args...>::kType,
                               detail::RpcArgs<Args...>::kTakesCoroContext> {
  static constexpr bool kRequestIsReference =
      detail::RequestIsReference<Args...>::value;
};

// Const member functions behave identically to their non-const counterparts.
template <typename Svc, typename Fut, typename... Args>
struct MethodTraits<Fut (Svc::*)(Args...) const,
                    std::enable_if_t<detail::RpcArgs<Args...>::kValid>>
    : MethodTraits<Fut (Svc::*)(Args...)> {};

/// The concrete handle types an RPC of shape `<kExpectedType, kIsRaw, Req,
/// Resp>` hands to the user's method.
template <MethodType kExpectedType, bool kIsRaw, typename Req, typename Resp>
struct InvocationTraits {
  static_assert(kExpectedType == MethodType::kUnary ||
                    kExpectedType == MethodType::kServerStreaming ||
                    kExpectedType == MethodType::kClientStreaming ||
                    kExpectedType == MethodType::kBidirectionalStreaming,
                "Unsupported RPC method type");

  // Methods with a single response take a `UnaryWriter`; streaming
  // responses are sent through a `Writer`.
  static constexpr bool kUnaryResponse =
      kExpectedType == MethodType::kUnary ||
      kExpectedType == MethodType::kClientStreaming;

  // Streaming requests are read from the client through a `Reader`; unary
  // requests are passed by value, raw ones without deserializing.
  static constexpr bool kStreamingRequest =
      kExpectedType == MethodType::kClientStreaming ||
      kExpectedType == MethodType::kBidirectionalStreaming;

  using Responder = std::conditional_t<
      kUnaryResponse,
      std::conditional_t<kIsRaw, RawUnaryWriter, UnaryWriter<Resp>>,
      std::conditional_t<kIsRaw, RawWriter, Writer<Resp>>>;

  using Request =
      std::conditional_t<kStreamingRequest,
                         std::conditional_t<kIsRaw, RawReader, Reader<Req>>,
                         std::conditional_t<kIsRaw, ConstBuf, Req>>;
};

}  // namespace pw::rpc2::internal
