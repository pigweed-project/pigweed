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

#include "pw_rpc2/internal/method_traits.h"

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

// Classification is pure type computation, so every check here is a
// `static_assert`: if this file compiles, it passes. It deliberately includes
// only `method_traits.h`, which keeps the classification rules honest about
// not depending on the invocation machinery or the call runtime.

namespace pw::async2 {
template <typename T>
class Coro;
}  // namespace pw::async2

namespace pw::rpc2::internal {

// Stands in for a method's return type. `MethodTraits` only names it, so it
// need not actually be pendable.
class MockFuture {
 public:
  using value_type = void;
};

struct StubMsg {
  int value = 0;
};

// Signatures used to check `MethodTraits`. These methods are never called;
// only their types matter.
class TraitsService {
 public:
  // Not an RPC signature at all: no responder, non-message argument.
  void NotAnRpc(size_t);

  MockFuture UnaryConstRef(const StubMsg&, UnaryWriter<StubMsg>);
  MockFuture UnaryByValue(StubMsg, UnaryWriter<StubMsg>);
  MockFuture UnaryConstMethod(const StubMsg&, UnaryWriter<StubMsg>) const;
  async2::Coro<void> UnaryCoro(async2::CoroContext,
                               const StubMsg&,
                               UnaryWriter<StubMsg>);
  async2::Coro<void> UnaryCoroConstMethod(async2::CoroContext,
                                          StubMsg,
                                          UnaryWriter<StubMsg>) const;

  MockFuture ServerStreaming(const StubMsg&, Writer<StubMsg>);
  async2::Coro<void> ServerStreamingCoro(async2::CoroContext,
                                         StubMsg,
                                         Writer<StubMsg>);

  MockFuture ClientStreaming(Reader<StubMsg>, UnaryWriter<StubMsg>);
  async2::Coro<void> ClientStreamingCoro(async2::CoroContext,
                                         Reader<StubMsg>,
                                         UnaryWriter<StubMsg>) const;

  MockFuture Bidi(Reader<StubMsg>, Writer<StubMsg>);
  async2::Coro<void> BidiCoro(async2::CoroContext,
                              Reader<StubMsg>,
                              Writer<StubMsg>) const;

  MockFuture RawUnary(ConstBuf, RawUnaryWriter);
  MockFuture RawBidi(RawReader, RawWriter);

  // None of the following are RPC method signatures.
  MockFuture NoRequest(UnaryWriter<StubMsg>);
  MockFuture TooManyArgs(StubMsg, StubMsg, UnaryWriter<StubMsg>);
  MockFuture NoResponder(StubMsg, StubMsg);
  MockFuture CoroContextNotFirst(StubMsg,
                                 async2::CoroContext,
                                 UnaryWriter<StubMsg>);
  MockFuture NoArgs();
};

namespace {

template <auto kMethod,
          MethodType kType,
          bool kTakesCoroContext,
          bool kIsRaw,
          typename Req,
          typename Resp>
constexpr bool TraitsMatch() {
  using Traits = MethodTraits<decltype(kMethod)>;
  static_assert(Traits::kValid);
  static_assert(Traits::kType == kType);
  static_assert(Traits::kTakesCoroContext == kTakesCoroContext);
  static_assert(Traits::kIsRaw == kIsRaw);
  static_assert(std::is_same_v<typename Traits::Service, TraitsService>);
  static_assert(std::is_same_v<typename Traits::Request, Req>);
  static_assert(std::is_same_v<typename Traits::Response, Resp>);
  return true;
}

// The request may be taken by value or by const reference, and the method may
// be const or non-const, without changing how it is classified.
static_assert(TraitsMatch<&TraitsService::UnaryConstRef,
                          MethodType::kUnary,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::UnaryByValue,
                          MethodType::kUnary,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::UnaryConstMethod,
                          MethodType::kUnary,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::UnaryCoro,
                          MethodType::kUnary,
                          true,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::UnaryCoroConstMethod,
                          MethodType::kUnary,
                          true,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::ServerStreaming,
                          MethodType::kServerStreaming,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::ServerStreamingCoro,
                          MethodType::kServerStreaming,
                          true,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::ClientStreaming,
                          MethodType::kClientStreaming,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::ClientStreamingCoro,
                          MethodType::kClientStreaming,
                          true,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::Bidi,
                          MethodType::kBidirectionalStreaming,
                          false,
                          false,
                          StubMsg,
                          StubMsg>());
static_assert(TraitsMatch<&TraitsService::BidiCoro,
                          MethodType::kBidirectionalStreaming,
                          true,
                          false,
                          StubMsg,
                          StubMsg>());

// Methods which use `ConstBuf` for both the request and response are raw.
static_assert(TraitsMatch<&TraitsService::RawUnary,
                          MethodType::kUnary,
                          false,
                          true,
                          ConstBuf,
                          ConstBuf>());
static_assert(TraitsMatch<&TraitsService::RawBidi,
                          MethodType::kBidirectionalStreaming,
                          false,
                          true,
                          ConstBuf,
                          ConstBuf>());

static_assert(
    std::is_same_v<MethodTraits<decltype(&TraitsService::Bidi)>::Future,
                   MockFuture>);
static_assert(
    std::is_same_v<MethodTraits<decltype(&TraitsService::BidiCoro)>::Future,
                   async2::Coro<void>>);

template <auto kMethod>
constexpr bool TraitsAreInvalid() {
  using Traits = MethodTraits<decltype(kMethod)>;
  static_assert(!Traits::kValid);
  static_assert(!Traits::kIsRaw);
  static_assert(!Traits::kTakesCoroContext);
  static_assert(std::is_same_v<typename Traits::Future, void>);
  return true;
}

static_assert(TraitsAreInvalid<&TraitsService::NoRequest>());
static_assert(TraitsAreInvalid<&TraitsService::TooManyArgs>());
static_assert(TraitsAreInvalid<&TraitsService::NoResponder>());
static_assert(TraitsAreInvalid<&TraitsService::CoroContextNotFirst>());
static_assert(TraitsAreInvalid<&TraitsService::NoArgs>());
static_assert(TraitsAreInvalid<&TraitsService::NotAnRpc>());

}  // namespace
}  // namespace pw::rpc2::internal
