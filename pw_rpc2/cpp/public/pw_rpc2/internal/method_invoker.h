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

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/internal/method.h"
#include "pw_rpc2/internal/method_future.h"
#include "pw_rpc2/internal/method_info.h"
#include "pw_rpc2/internal/method_traits.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
#include "pw_async2/coro.h"
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

namespace pw::rpc2::internal {

/// Always false, but only after `Impl` is known, so that a `static_assert`
/// in an uninstantiated template does not fire eagerly.
///
/// Named by generated code, which uses it to report a missing method.
template <typename...>
inline constexpr bool kAlwaysFalse = false;

/// Constructs `Fut` from the RPC's arguments, passing the service as the first
/// argument if the future's constructor accepts it.
///
/// Named by generated code: the default method implementation the generated
/// service base class provides is a call to this.
template <typename Fut, typename ServiceClass, typename... Args>
Fut CreateFuture(ServiceClass& service, Args&&... args) {
  if constexpr (std::is_constructible_v<Fut, ServiceClass&, Args...>) {
    return Fut(service, std::forward<Args>(args)...);
  } else {
    return Fut(std::forward<Args>(args)...);
  }
}

namespace detail {

// =============================================================================
// Raw vs. typed API detection for declarative `<Method>Future` types
// =============================================================================

// True if `Fut` can be built from `Args`, either with or without a leading
// reference to the service.
template <typename Fut, typename Service, typename... Args>
inline constexpr bool kFutureCreatableFrom =
    std::is_constructible_v<Fut, Service&, Args...> ||
    std::is_constructible_v<Fut, Args...>;

// True if `Fut` accepts the typed protobuf arguments for the method.
template <typename Fut,
          typename Service,
          MethodType kType,
          typename Req,
          typename Resp>
inline constexpr bool kFutureUsesTypedApi = kFutureCreatableFrom<
    Fut,
    Service,
    typename InvocationTraits<kType, false, Req, Resp>::Request,
    typename InvocationTraits<kType, false, Req, Resp>::Responder>;

template <typename Fut, typename Service, MethodType kType>
inline constexpr bool
    kFutureUsesTypedApi<Fut, Service, kType, ConstBuf, ConstBuf> = false;

// True if `Fut` accepts the raw (`pw::ConstBuf`) arguments for the method.
template <typename Fut, typename Service, MethodType kType>
inline constexpr bool kFutureUsesRawApi = kFutureCreatableFrom<
    Fut,
    Service,
    typename InvocationTraits<kType, true, ConstBuf, ConstBuf>::Request,
    typename InvocationTraits<kType, true, ConstBuf, ConstBuf>::Responder>;

/// The future type returned by a method, or `BoxedMethodFuture` if the method's
/// signature was not recognized. Substituting a valid future type keeps an
/// unrecognized signature from burying the `kValid` assertion in unrelated
/// template errors.
template <typename Traits>
using MethodFutureOr = std::
    conditional_t<Traits::kValid, typename Traits::Future, BoxedMethodFuture>;

}  // namespace detail

/// Functionality shared by the method invokers: it computes the storage
/// requirements of the method's future and provides the `Method` descriptor,
/// which forwards invocation to `Derived::InvokeIntoCall()`.
template <typename Derived, typename Fut, MethodType kType>
class MethodInvokerBase {
 public:
  static_assert(async2::Future<Fut>,
                "RPC method return type must satisfy pw::async2::Future");

  using FutureType = MethodFutureImpl<Fut>;

  static_assert(sizeof(FutureType) <= Method::kMaxSizeBytes,
                "This RPC method's future is too large to store inline in a "
                "call. Its storage is allocated for every in-flight call, so "
                "it must stay small. Move large members into the service "
                "object, or split the future into smaller pieces.");
  static_assert(
      alignof(FutureType) <= alignof(std::max_align_t),
      "This RPC method's future is over-aligned "
      "(alignof(FutureType) > alignof(std::max_align_t)). Inline call future "
      "storage only guarantees alignof(std::max_align_t) alignment. Remove "
      "over-aligned members (or alignas specifiers) from the future or move "
      "them into the service object.");

  static constexpr size_t kFutureStorageSize = sizeof(FutureType);

  template <typename ServiceClass>
  static ProtocolStatus InvokeMethod(Service& service,
                                     ServerCall& call,
                                     ConstBuf&& request_payload) {
    return Derived::template InvokeIntoCall<ServiceClass>(
        static_cast<ServiceClass&>(service), call, std::move(request_payload));
  }

  template <typename ServiceClass>
  static constexpr Method CreateMethod(uint32_t method_id) {
    return Method(
        method_id, kType, kFutureStorageSize, &InvokeMethod<ServiceClass>);
  }

 protected:
  template <typename Invocation, bool kIsRaw, typename Req, typename InvokeFn>
  static ProtocolStatus PrepareRequestAndInvoke(ServerCall& call,
                                                ConstBuf&& request_payload,
                                                InvokeFn&& invoke) {
    if constexpr (Invocation::kStreamingRequest) {
      return invoke([&call] {
        return CallAccess::Create<typename Invocation::Request>(
            call.shared_call());
      });
    } else {
      call.CloseRead();
      if constexpr (kIsRaw) {
        return invoke([&request_payload]() -> ConstBuf&& {
          return std::move(request_payload);
        });
      } else {
        Result<Req> request = Deserialize<Req>(request_payload);
        if (!request.ok()) {
          return ProtocolStatus::kInvalidRequestPayload;
        }
        return invoke([&request]() -> Req&& { return std::move(*request); });
      }
    }
  }
};

/// Invokes an RPC on a member function of a service implementation.
template <auto kMethod,
          MethodType kExpectedType,
          typename Req = pw::ConstBuf,
          typename Resp = pw::ConstBuf>
class MethodInvoker
    : public MethodInvokerBase<
          MethodInvoker<kMethod, kExpectedType, Req, Resp>,
          detail::MethodFutureOr<MethodTraits<decltype(kMethod)>>,
          kExpectedType> {
 public:
  using Traits = MethodTraits<decltype(kMethod)>;

  static_assert(Traits::kValid,
                "Invalid RPC method signature. Methods must return either a "
                "pw::async2::Future (with value_type = void) or "
                "pw::async2::Coro<void> "
                "(accepting pw::async2::CoroContext as first parameter).");

  static_assert(
      Traits::kType == kExpectedType,
      "Method signature does not match the expected RPC method type.");

  // The request passed to the method is a temporary owned by
  // `InvokeIntoCall`. A future binds it in its constructor, which runs
  // before that temporary dies, but a coroutine body does not run until the
  // call is first polled, long after. A reference parameter would
  // dangle for the entire life of the coroutine.
  static_assert(
      !Traits::kTakesCoroContext || !Traits::kRequestIsReference,
      "A coroutine RPC method must take its request by value. Change the "
      "parameter from 'const Request&' to 'Request'.");

  template <typename ServiceClass>
  static ProtocolStatus InvokeIntoCall(ServiceClass& service,
                                       ServerCall& call,
                                       pw::ConstBuf&& request_payload) {
    if constexpr (!Traits::kIsRaw) {
      static_assert(std::is_same_v<typename Traits::Request, Req>,
                    "Method request type does not match the generated service. "
                    "Use the request type from the generated service header "
                    "or pw::ConstBuf for a raw method.");
      static_assert(std::is_same_v<typename Traits::Response, Resp>,
                    "Method response type does not match the generated "
                    "service. Use the response type from the generated service "
                    "header or pw::ConstBuf for a raw method.");
    }

    using Invocation =
        InvocationTraits<kExpectedType, Traits::kIsRaw, Req, Resp>;
    return MethodInvoker::
        template PrepareRequestAndInvoke<Invocation, Traits::kIsRaw, Req>(
            call, std::move(request_payload), [&](auto&& make_request) {
              return InvokeWithRequest(
                  service,
                  call,
                  std::forward<decltype(make_request)>(make_request));
            });
  }

 private:
  template <typename ServiceClass, typename RequestExprFn>
  static ProtocolStatus InvokeWithRequest(ServiceClass& service,
                                          ServerCall& call,
                                          RequestExprFn&& make_request) {
    using Invocation =
        InvocationTraits<kExpectedType, Traits::kIsRaw, Req, Resp>;
    using Responder = typename Invocation::Responder;
    return call.EmplaceFutureFromFactory<typename Traits::Future>([&] {
      if constexpr (Traits::kTakesCoroContext) {
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
        return (service.*kMethod)(
            async2::CoroContext(call.connection_task().allocator()),
            std::forward<RequestExprFn>(make_request)(),
            CallAccess::Create<Responder>(call.shared_call()));
#else
        static_assert(!Traits::kTakesCoroContext,
                      "Coroutine RPC methods require C++20.");
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
      } else {
        return (service.*kMethod)(
            std::forward<RequestExprFn>(make_request)(),
            CallAccess::Create<Responder>(call.shared_call()));
      }
    });
  }
};

template <auto kMethod, MethodType kExpectedType>
using RawMethodInvoker =
    MethodInvoker<kMethod, kExpectedType, pw::ConstBuf, pw::ConstBuf>;

/// `MethodInvoker` parameterized on a generated RPC method tag.
template <auto kMethod,
          typename MethodTag,
          typename Info = MethodInfo<MethodTag>>
struct MethodInvokerFor : MethodInvoker<kMethod,
                                        Info::kType,
                                        typename Info::Request,
                                        typename Info::Response> {
  template <typename ServiceClass>
  static constexpr Method CreateMethod() {
    return MethodInvoker<kMethod,
                         Info::kType,
                         typename Info::Request,
                         typename Info::Response>::
        template CreateMethod<ServiceClass>(Info::kMethodId);
  }
};

/// Invokes an RPC by constructing a future type provided by a service
/// implementation.
template <typename Fut,
          MethodType kExpectedType,
          typename Req = ConstBuf,
          typename Resp = ConstBuf>
class FutureMethodInvoker
    : public MethodInvokerBase<
          FutureMethodInvoker<Fut, kExpectedType, Req, Resp>,
          Fut,
          kExpectedType> {
 public:
  static_assert(std::is_same_v<typename Fut::value_type, void>,
                "RPC method return future must have a value_type of void");

  template <typename ServiceClass>
  static ProtocolStatus InvokeIntoCall(ServiceClass& service,
                                       ServerCall& call,
                                       ConstBuf&& request_payload) {
    // The protobuf API is deduced from the future's constructor. Both may be
    // mixed within a single service, but a single future must not accept both.
    //
    // When `Req` and `Resp` are `pw::ConstBuf`, the service came from the raw
    // codegen, whose typed and raw argument lists are identical; treat it as
    // raw so that no deserialization is attempted.
    constexpr bool kRawCodegen =
        std::is_same_v<Req, ConstBuf> && std::is_same_v<Resp, ConstBuf>;
    constexpr bool kIsTyped =
        !kRawCodegen &&
        detail::
            kFutureUsesTypedApi<Fut, ServiceClass, kExpectedType, Req, Resp>;
    constexpr bool kUsesRawApi =
        !kRawCodegen &&
        detail::kFutureUsesRawApi<Fut, ServiceClass, kExpectedType>;

    constexpr bool kIsUnambiguous = !(kIsTyped && kUsesRawApi);
    static_assert(
        kIsUnambiguous,
        "The '<Method>Future' type declared for this RPC is constructible from "
        "both the typed protobuf arguments and the raw (pw::ConstBuf) "
        "arguments, which is ambiguous. Provide only one constructor.");

    constexpr bool kCanConstruct =
        kRawCodegen
            ? detail::kFutureUsesRawApi<Fut, ServiceClass, kExpectedType>
            : (kIsTyped || kUsesRawApi);
    static_assert(
        kCanConstruct,
        "The '<Method>Future' type declared for this RPC cannot be constructed "
        "from the method's arguments. It must be constructible from either the "
        "typed protobuf arguments or the raw (pw::ConstBuf) arguments for this "
        "method type, optionally preceded by a reference to the service.");

    if constexpr (kIsUnambiguous && kCanConstruct) {
      constexpr bool kIsRaw = kRawCodegen || kUsesRawApi;

      using Invocation = InvocationTraits<kExpectedType, kIsRaw, Req, Resp>;
      return FutureMethodInvoker::
          template PrepareRequestAndInvoke<Invocation, kIsRaw, Req>(
              call, std::move(request_payload), [&](auto&& make_request) {
                return EmplaceWithRequest<kIsRaw>(
                    service,
                    call,
                    std::forward<decltype(make_request)>(make_request));
              });
    } else {
      static_cast<void>(service);
      static_cast<void>(call);
      static_cast<void>(request_payload);
      return ProtocolStatus::kInternal;
    }
  }

 private:
  template <bool kIsRaw, typename ServiceClass, typename RequestExprFn>
  static ProtocolStatus EmplaceWithRequest(ServiceClass& service,
                                           ServerCall& call,
                                           RequestExprFn&& make_request) {
    using Invocation = InvocationTraits<kExpectedType, kIsRaw, Req, Resp>;
    using Responder = typename Invocation::Responder;
    return call.EmplaceFutureFromFactory<Fut>([&] {
      return CreateFuture<Fut>(
          service,
          std::forward<RequestExprFn>(make_request)(),
          CallAccess::Create<Responder>(call.shared_call()));
    });
  }
};

template <typename Fut, MethodType kExpectedType>
using RawFutureMethodInvoker =
    FutureMethodInvoker<Fut, kExpectedType, ConstBuf, ConstBuf>;

/// `FutureMethodInvoker` parameterized on a generated RPC method tag.
template <typename Fut,
          typename MethodTag,
          typename Info = MethodInfo<MethodTag>>
struct FutureMethodInvokerFor : FutureMethodInvoker<Fut,
                                                    Info::kType,
                                                    typename Info::Request,
                                                    typename Info::Response> {
  template <typename ServiceClass>
  static constexpr Method CreateMethod() {
    return FutureMethodInvoker<Fut,
                               Info::kType,
                               typename Info::Request,
                               typename Info::Response>::
        template CreateMethod<ServiceClass>(Info::kMethodId);
  }
};

}  // namespace pw::rpc2::internal
