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

// Tests codegen for the protos in codegen_test_protos/. Every generated header
// is included in this one translation unit, which checks that headers for
// different files that use the same messages do not conflict.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/future_task.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/codegen_test_protos/common.pwpb.rpc2.h"
#include "pw_rpc2/codegen_test_protos/common.raw.rpc2.h"
#include "pw_rpc2/codegen_test_protos/names.pwpb.rpc2.h"
#include "pw_rpc2/codegen_test_protos/names.raw.rpc2.h"
#include "pw_rpc2/codegen_test_protos/no_package.pwpb.rpc2.h"
#include "pw_rpc2/codegen_test_protos/no_package.raw.rpc2.h"
#include "pw_rpc2/codegen_test_protos/other.pwpb.rpc2.h"
#include "pw_rpc2/codegen_test_protos/other.raw.rpc2.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

// The generated namespaces. Aliases are used throughout, since within a
// service implementation, member names such as `Ping` hide the message
// namespaces of the same name.
namespace names_pwpb = ::rpc2::codegen_test::pw_rpc2::pwpb;
namespace names_raw = ::rpc2::codegen_test::pw_rpc2::raw;
namespace other_pwpb = ::pw::rpc2::codegen_test::other::pw_rpc2::pwpb;
namespace other_raw = ::pw::rpc2::codegen_test::other::pw_rpc2::raw;
namespace no_package_pwpb = ::pw_rpc2::pwpb::NoPackage;
namespace no_package_raw = ::pw_rpc2::raw::NoPackage;

// The pw_protobuf message namespaces.
namespace names_msgs = ::rpc2::codegen_test::pwpb;
namespace common_msgs = ::pw::rpc2::codegen_test::common::pwpb;

using PingMsg = names_msgs::Ping::Message;
using PongMsg = names_msgs::Pong::Message;
using DeleteMsg = names_msgs::delete_::Message;
using InnerMsg = names_msgs::Outer::Inner::Message;
using SharedMsg = common_msgs::Shared::Message;
using CommonInnerMsg = common_msgs::Outer::Inner::Message;
using ::pw::rpc2::Client;
using ::pw::rpc2::MethodType;
using ::pw::rpc2::RawBidiStreamFuture;
using ::pw::rpc2::RawClientStreamFuture;
using ::pw::rpc2::RawReader;
using ::pw::rpc2::RawResponseFuture;
using ::pw::rpc2::RawServerStreamReserveFuture;
using ::pw::rpc2::RawUnaryReserveFuture;
using ::pw::rpc2::RawUnaryWriter;
using ::pw::rpc2::RawWriter;
using ::pw::rpc2::Reader;
using ::pw::rpc2::ReserveWriteFuture;
using ::pw::rpc2::ResponseFuture;
using ::pw::rpc2::Server;
using ::pw::rpc2::ServerStreamFuture;
using ::pw::rpc2::ServerStreamReserveFuture;
using ::pw::rpc2::ServiceClient;
using ::pw::rpc2::UnaryFuture;
using ::pw::rpc2::UnaryReserveFuture;
using ::pw::rpc2::UnaryWriter;
using ::pw::rpc2::WriteFuture;
using ::pw::rpc2::Writer;
namespace internal = ::pw::rpc2::internal;
namespace test = ::pw::rpc2::test;
namespace async2 = ::pw::async2;
namespace allocator = ::pw::allocator;

// =============================================================================
// Method tags and MethodInfo
// =============================================================================

template <typename Method,
          MethodType kType,
          typename Request,
          typename Response,
          typename Info = internal::MethodInfo<Method>>
constexpr bool kDescribes =
    std::is_empty_v<Method> && !std::is_default_constructible_v<Method> &&
    Info::kType == kType && std::is_same_v<typename Info::Request, Request> &&
    std::is_same_v<typename Info::Response, Response>;

// Methods named after the messages they use, keyword and nested messages.
static_assert(
    kDescribes<names_pwpb::Names::Ping, MethodType::kUnary, PingMsg, PongMsg>);
static_assert(
    kDescribes<names_pwpb::Names::Pong, MethodType::kUnary, PingMsg, PongMsg>);
static_assert(kDescribes<names_pwpb::Names::Delete,
                         MethodType::kUnary,
                         DeleteMsg,
                         DeleteMsg>);
static_assert(kDescribes<names_pwpb::Names::Nested,
                         MethodType::kUnary,
                         InnerMsg,
                         InnerMsg>);

// Every kind of method, using messages imported from another package.
static_assert(kDescribes<names_pwpb::Kinds::Unary,
                         MethodType::kUnary,
                         SharedMsg,
                         CommonInnerMsg>);
static_assert(kDescribes<names_pwpb::Kinds::ServerStream,
                         MethodType::kServerStreaming,
                         SharedMsg,
                         SharedMsg>);
static_assert(kDescribes<names_pwpb::Kinds::ClientStream,
                         MethodType::kClientStreaming,
                         SharedMsg,
                         SharedMsg>);
static_assert(kDescribes<names_pwpb::Kinds::Bidi,
                         MethodType::kBidirectionalStreaming,
                         SharedMsg,
                         SharedMsg>);
static_assert(kDescribes<other_pwpb::Other::Get,
                         MethodType::kUnary,
                         SharedMsg,
                         SharedMsg>);

// A file without a package.
static_assert(kDescribes<no_package_pwpb::Unary,
                         MethodType::kUnary,
                         SharedMsg,
                         CommonInnerMsg>);
static_assert(kDescribes<no_package_pwpb::ServerStream,
                         MethodType::kServerStreaming,
                         SharedMsg,
                         SharedMsg>);
static_assert(kDescribes<no_package_pwpb::ClientStream,
                         MethodType::kClientStreaming,
                         SharedMsg,
                         CommonInnerMsg>);
static_assert(kDescribes<no_package_pwpb::Bidi,
                         MethodType::kBidirectionalStreaming,
                         SharedMsg,
                         SharedMsg>);

// Raw descriptors carry the same kinds, with pw::ConstBuf payloads.
static_assert(kDescribes<names_raw::Names::Ping,
                         MethodType::kUnary,
                         pw::ConstBuf,
                         pw::ConstBuf>);
static_assert(kDescribes<names_raw::Kinds::ServerStream,
                         MethodType::kServerStreaming,
                         pw::ConstBuf,
                         pw::ConstBuf>);
static_assert(kDescribes<names_raw::Kinds::ClientStream,
                         MethodType::kClientStreaming,
                         pw::ConstBuf,
                         pw::ConstBuf>);
static_assert(kDescribes<names_raw::Kinds::Bidi,
                         MethodType::kBidirectionalStreaming,
                         pw::ConstBuf,
                         pw::ConstBuf>);
static_assert(kDescribes<no_package_raw::Bidi,
                         MethodType::kBidirectionalStreaming,
                         pw::ConstBuf,
                         pw::ConstBuf>);

template <typename PwpbMethod, typename RawMethod>
constexpr bool kSameIds = internal::MethodInfo<PwpbMethod>::kServiceId ==
                              internal::MethodInfo<RawMethod>::kServiceId &&
                          internal::MethodInfo<PwpbMethod>::kMethodId ==
                              internal::MethodInfo<RawMethod>::kMethodId;

// The raw and pwpb codegen agree on IDs, so either can talk to the other.
static_assert(kSameIds<names_pwpb::Names::Ping, names_raw::Names::Ping>);
static_assert(kSameIds<names_pwpb::Names::Pong, names_raw::Names::Pong>);
static_assert(kSameIds<names_pwpb::Names::Delete, names_raw::Names::Delete>);
static_assert(kSameIds<names_pwpb::Names::Nested, names_raw::Names::Nested>);
static_assert(kSameIds<names_pwpb::Kinds::Bidi, names_raw::Kinds::Bidi>);
static_assert(kSameIds<other_pwpb::Other::Get, other_raw::Other::Get>);
static_assert(kSameIds<no_package_pwpb::Unary, no_package_raw::Unary>);

// IDs are the pw_rpc 65599 hashes of the fully qualified service name and of
// the method name. A package-less service hashes its bare name.
static_assert(internal::MethodInfo<names_pwpb::Names::Ping>::kServiceId ==
              0x35c7080e);  // "rpc2.codegen_test.Names"
static_assert(internal::MethodInfo<names_pwpb::Names::Ping>::kMethodId ==
              0x860c5b16);  // Ping
static_assert(internal::MethodInfo<no_package_pwpb::Unary>::kServiceId ==
              0x450dc124);  // "NoPackage"

// Methods with the same name in different services share a method ID but not a
// service ID.
static_assert(internal::MethodInfo<names_pwpb::Kinds::Unary>::kMethodId ==
              internal::MethodInfo<no_package_pwpb::Unary>::kMethodId);
static_assert(internal::MethodInfo<names_pwpb::Kinds::Unary>::kServiceId !=
              internal::MethodInfo<no_package_pwpb::Unary>::kServiceId);

// The generated clients inherit from `pw::rpc2::ServiceClient` and have
// exactly the default and `pw::rpc2::Client` constructors.
static_assert(std::is_base_of_v<ServiceClient, names_pwpb::Names::Client>);
static_assert(std::is_base_of_v<ServiceClient, names_raw::Names::Client>);
static_assert(std::is_default_constructible_v<names_pwpb::Names::Client>);
static_assert(std::is_constructible_v<names_pwpb::Names::Client,
                                      const ::pw::rpc2::Client&>);
static_assert(!std::is_convertible_v<const ::pw::rpc2::Client&,
                                     names_pwpb::Names::Client>);
static_assert(!std::is_constructible_v<names_pwpb::Names::Client,
                                       const ::pw::rpc2::Client&,
                                       uint32_t>);
static_assert(std::is_constructible_v<names_raw::Names::Client,
                                      const ::pw::rpc2::Client&>);
static_assert(!std::is_constructible_v<names_raw::Names::Client,
                                       const ::pw::rpc2::Client&,
                                       uint32_t>);

// Method tags are empty, final classes that cannot be created. They are not
// bases of the generated Client.
static_assert(std::is_final_v<names_pwpb::Kinds::Unary>);
static_assert(std::is_final_v<names_raw::Kinds::ServerStream>);
static_assert(std::is_final_v<names_pwpb::Kinds::ClientStream>);
static_assert(std::is_final_v<names_raw::Kinds::Bidi>);
static_assert(
    !std::is_base_of_v<names_raw::Kinds::Unary, names_raw::Kinds::Client>);
static_assert(!std::is_base_of_v<names_pwpb::Kinds::ServerStream,
                                 names_pwpb::Kinds::Client>);

// `client.Method::Copy()` comes from the generated
// `pw_rpc2_internal::PwInternal_ClientCopyMethods` base. It is empty, so
// the Client is the size of its library base class, and only the Client can
// create or copy it.
static_assert(std::is_base_of_v<
              names_raw::Kinds::pw_rpc2_internal::PwInternal_ClientCopyMethods,
              names_raw::Kinds::Client>);
static_assert(std::is_base_of_v<
              names_pwpb::Kinds::pw_rpc2_internal::PwInternal_ClientCopyMethods,
              names_pwpb::Kinds::Client>);
static_assert(
    std::is_empty_v<
        names_pwpb::Kinds::pw_rpc2_internal::PwInternal_ClientCopyMethods>);
static_assert(
    !std::is_default_constructible_v<
        names_pwpb::Kinds::pw_rpc2_internal::PwInternal_ClientCopyMethods>);
static_assert(
    !std::is_copy_constructible_v<
        names_raw::Kinds::pw_rpc2_internal::PwInternal_ClientCopyMethods>);
static_assert(sizeof(names_pwpb::Kinds::Client) ==
              sizeof(internal::GeneratedServiceClient));
static_assert(sizeof(names_raw::Names::Client) ==
              sizeof(internal::GeneratedServiceClient));
static_assert(std::is_copy_constructible_v<names_raw::Names::Client>);
static_assert(std::is_copy_assignable_v<names_raw::Names::Client>);
static_assert(std::is_move_assignable_v<names_pwpb::Kinds::Client>);

// `Copy()` takes an rvalue `ConstBuf`: an owned buffer must be moved in.
template <typename MemberFunction>
struct CopySignature;

template <typename Class, typename Return, typename Param>
struct CopySignature<Return (Class::*)(Param) const> {
  using Result = Return;
  using Parameter = Param;
};

static_assert(std::is_same_v<
              CopySignature<
                  decltype(&names_raw::Names::Client::Ping::Copy)>::Parameter,
              pw::ConstBuf&&>);
static_assert(
    std::is_same_v<CopySignature<decltype(&names_pwpb::Kinds::Client::
                                              ServerStream::Copy)>::Parameter,
                   pw::ConstBuf&&>);

// Raw clients copy requests from a `ConstBuf` with `client.Method::Copy()`.
static_assert(
    std::is_same_v<decltype(std::declval<const names_raw::Names::Client&>()
                                .Ping::Copy(pw::ConstBuf())),
                   UnaryFuture<>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_raw::Kinds::Client&>()
                                .ServerStream::Copy(pw::ConstBuf())),
                   ServerStreamFuture<>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_raw::Kinds::Client&>()
                                .ClientStream()),
                   RawClientStreamFuture>);
static_assert(std::is_same_v<
              decltype(std::declval<const names_raw::Kinds::Client&>().Bidi()),
              RawBidiStreamFuture>);

// pwpb clients send a message, copied or moved into the future, or copy the
// encoded request from a `ConstBuf`.
static_assert(
    std::is_same_v<decltype(std::declval<const names_pwpb::Names::Client&>()
                                .Ping(std::declval<const PingMsg&>())),
                   UnaryFuture<PingMsg, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_pwpb::Names::Client&>()
                                .Ping(PingMsg{})),
                   UnaryFuture<PingMsg, PongMsg>>);
static_assert(
    std::is_same_v<
        decltype(std::declval<const names_pwpb::Names::Client&>().Ping({})),
        UnaryFuture<PingMsg, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_pwpb::Names::Client&>()
                                .Ping::Copy(pw::ConstBuf())),
                   UnaryFuture<pw::ConstBuf, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_pwpb::Kinds::Client&>()
                                .ServerStream::Copy(pw::ConstBuf())),
                   ServerStreamFuture<pw::ConstBuf, SharedMsg>>);

template <typename Client, typename Arg, typename = void>
constexpr bool kPingAccepts = false;

template <typename Client, typename Arg>
constexpr bool
    kPingAccepts<Client,
                 Arg,
                 std::void_t<decltype(std::declval<const Client&>().Ping(
                     std::declval<Arg>()))>> = true;

template <typename Client, typename = void>
constexpr bool kPingAcceptsBraces = false;

template <typename Client>
constexpr bool kPingAcceptsBraces<
    Client,
    std::void_t<decltype(std::declval<const Client&>().Ping({}))>> = true;

// `client.Method()` takes a size or (pwpb only) a message, but never a
// `ConstBuf` or a `bool`. Braces initialize a message rather than a size.
static_assert(kPingAccepts<names_raw::Names::Client, uint8_t>);
static_assert(kPingAccepts<names_pwpb::Names::Client, size_t>);
static_assert(!kPingAccepts<names_raw::Names::Client, pw::ConstBuf>);
static_assert(!kPingAccepts<names_pwpb::Names::Client, pw::ConstBuf>);
static_assert(!kPingAccepts<names_raw::Names::Client, bool>);
static_assert(!kPingAccepts<names_pwpb::Names::Client, bool>);
static_assert(!kPingAcceptsBraces<names_raw::Names::Client>);
static_assert(kPingAcceptsBraces<names_pwpb::Names::Client>);

// Methods may be named after the client API (`Copy`, `Reserve`, and the class
// that provides `Copy()`), unprefixed internal names (`internal`, `derived`,
// `kServiceId`, `ClientCopyMethods`), and base class members (`is_open`,
// `client`, `CallUnary`, etc.).
namespace api_pwpb = names_pwpb::ClientApiNames;
namespace api_raw = names_raw::ClientApiNames;

static_assert(std::is_same_v<
              decltype(std::declval<const api_pwpb::Client&>().Copy(PingMsg{})),
              UnaryFuture<PingMsg, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_pwpb::Client&>().Copy(8)),
                   UnaryReserveFuture<PongMsg>>);
static_assert(std::is_same_v<decltype(std::declval<const api_pwpb::Client&>()
                                          .Copy::Copy(pw::ConstBuf())),
                             UnaryFuture<pw::ConstBuf, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_pwpb::Client&>().Reserve(8)),
                   ServerStreamReserveFuture<PongMsg>>);
static_assert(std::is_same_v<decltype(std::declval<const api_pwpb::Client&>()
                                          .Reserve::Copy(pw::ConstBuf())),
                             ServerStreamFuture<pw::ConstBuf, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_raw::Client&>()
                                .UnaryCopyMethod::Copy(pw::ConstBuf())),
                   UnaryFuture<>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_raw::Client&>().Copy(8)),
                   RawUnaryReserveFuture>);
static_assert(std::is_same_v<decltype(std::declval<const api_raw::Client&>()
                                          .Copy::Copy(pw::ConstBuf())),
                             UnaryFuture<>>);
static_assert(std::is_same_v<decltype(std::declval<const api_pwpb::Client&>()
                                          .internal(PingMsg{})),
                             UnaryFuture<PingMsg, PongMsg>>);
static_assert(std::is_same_v<decltype(std::declval<const api_pwpb::Client&>()
                                          .derived(PingMsg{})),
                             UnaryFuture<PingMsg, PongMsg>>);
static_assert(std::is_same_v<
              decltype(std::declval<const api_pwpb::Client&>().Impl(PingMsg{})),
              UnaryFuture<PingMsg, PongMsg>>);
static_assert(std::is_same_v<
              decltype(std::declval<const api_pwpb::Client&>().Size(PingMsg{})),
              UnaryFuture<PingMsg, PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_pwpb::Client&>().Size(8)),
                   UnaryReserveFuture<PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_raw::Client&>().Size(8)),
                   RawUnaryReserveFuture>);
static_assert(
    std::is_same_v<decltype(std::declval<const api_raw::Client&>()
                                .ClientCopyMethods::Copy(pw::ConstBuf())),
                   UnaryFuture<>>);
static_assert(std::is_same_v<decltype(std::declval<const api_pwpb::Client&>()
                                          .is_open(PingMsg{})),
                             UnaryFuture<PingMsg, PongMsg>>);
static_assert(std::is_same_v<decltype(std::declval<const api_raw::Client&>()
                                          .client::Copy(pw::ConstBuf())),
                             UnaryFuture<>>);
static_assert(std::is_same_v<decltype(std::declval<const api_raw::Client&>()
                                          .CallUnary::Copy(pw::ConstBuf())),
                             UnaryFuture<>>);

// `client.Method()` reserves a buffer for writing the request in
// place, for both raw and pwpb clients.
static_assert(std::is_same_v<
              decltype(std::declval<const names_raw::Names::Client&>().Ping(0)),
              RawUnaryReserveFuture>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_raw::Kinds::Client&>()
                                .ServerStream(0)),
                   RawServerStreamReserveFuture>);
static_assert(
    std::is_same_v<
        decltype(std::declval<const names_pwpb::Names::Client&>().Ping(0)),
        UnaryReserveFuture<PongMsg>>);
static_assert(
    std::is_same_v<decltype(std::declval<const names_pwpb::Kinds::Client&>()
                                .ServerStream(0)),
                   ServerStreamReserveFuture<SharedMsg>>);
static_assert(std::is_same_v<
              decltype(std::declval<const no_package_raw::Client&>().Unary(0)),
              RawUnaryReserveFuture>);

// =============================================================================
// Futures used by the service implementations
// =============================================================================

// Finishes a typed unary RPC with `response`.
template <typename Response>
class RespondFuture : public test::TestFuture {
 public:
  RespondFuture() = default;
  RespondFuture(Response response, UnaryWriter<Response> writer)
      : test::TestFuture(true),
        response_(std::move(response)),
        writer_(std::move(writer)) {}

  async2::Poll<> Pend(async2::Context& cx) {
    if (!write_fut_.is_pendable()) {
      write_fut_ = writer_.Finish(response_);
    }
    PW_AWAIT(pw::Status status, write_fut_, cx);
    PW_TEST_EXPECT_OK(status);
    return Complete();
  }

 private:
  Response response_;
  UnaryWriter<Response> writer_;
  WriteFuture<Response> write_fut_;
};

// Finishes a typed unary RPC with `Response{.value = request.value + kAdd}`.
template <typename Request, typename Response, uint32_t kAdd>
class AddFuture : public RespondFuture<Response> {
 public:
  AddFuture() = default;
  AddFuture(const Request& request, UnaryWriter<Response> writer)
      : RespondFuture<Response>(Response{.value = request.value + kAdd},
                                std::move(writer)) {}
};

// Finishes a raw unary RPC with a copy of the request.
class RawEchoFuture : public test::TestFuture {
 public:
  RawEchoFuture() = default;
  RawEchoFuture(pw::ConstBuf request, RawUnaryWriter writer)
      : test::TestFuture(true),
        request_(std::move(request)),
        writer_(std::move(writer)) {}

  async2::Poll<> Pend(async2::Context& cx) {
    if (!reserve_fut_.is_pendable()) {
      reserve_fut_ = writer_.ReserveFinish(request_.size());
    }
    PW_AWAIT(auto reservation, reserve_fut_, cx);
    PW_TEST_EXPECT_OK(reservation);
    if (reservation.ok()) {
      PW_TEST_EXPECT_OK(test::CommitCopy(*reservation, request_));
    }
    return Complete();
  }

 private:
  pw::ConstBuf request_;
  RawUnaryWriter writer_;
  ReserveWriteFuture reserve_fut_;
};

// A future for methods that are compiled but never invoked. Constructible only
// from exactly `First` and `Second`, so that it unambiguously selects either
// the typed or the raw API.
template <typename First, typename Second>
class IdleFuture {
 public:
  using value_type = void;

  IdleFuture() = default;
  IdleFuture(First, Second) {}

  bool is_pendable() const { return false; }
  bool is_complete() const { return true; }
  async2::Poll<> Pend(async2::Context&) { return async2::Ready(); }
};

static_assert(async2::Future<RawEchoFuture>);
static_assert(async2::Future<IdleFuture<pw::ConstBuf, RawWriter>>);

// =============================================================================
// Service implementations
// =============================================================================

// Implements methods named after their messages, mixing future types and
// member functions.
class NamesService : public names_pwpb::Names::Service<NamesService> {
 public:
  using PingFuture = AddFuture<PingMsg, PongMsg, 1>;

  RespondFuture<PongMsg> Pong(PingMsg request, UnaryWriter<PongMsg> writer) {
    ++pong_calls_;
    return {PongMsg{.value = request.value * 10}, std::move(writer)};
  }

  RespondFuture<DeleteMsg> Delete(DeleteMsg request,
                                  UnaryWriter<DeleteMsg> writer) {
    return {DeleteMsg{.value = request.value + 2}, std::move(writer)};
  }

  using NestedFuture = AddFuture<InnerMsg, InnerMsg, 3>;

  int pong_calls() const { return pong_calls_; }

 private:
  int pong_calls_ = 0;
};

// Implements every kind of method.
class KindsService : public names_pwpb::Kinds::Service<KindsService> {
 public:
  using UnaryFuture = AddFuture<SharedMsg, CommonInnerMsg, 4>;

  using ServerStreamFuture = IdleFuture<SharedMsg, Writer<SharedMsg>>;

  IdleFuture<Reader<SharedMsg>, UnaryWriter<SharedMsg>> ClientStream(
      Reader<SharedMsg> reader, UnaryWriter<SharedMsg> writer) {
    return {std::move(reader), std::move(writer)};
  }

  using BidiFuture = IdleFuture<Reader<SharedMsg>, Writer<SharedMsg>>;
};

class OtherService : public other_pwpb::Other::Service<OtherService> {
 public:
  using GetFuture = AddFuture<SharedMsg, SharedMsg, 5>;
};

class NoPackageService : public no_package_pwpb::Service<NoPackageService> {
 public:
  RespondFuture<CommonInnerMsg> Unary(SharedMsg request,
                                      UnaryWriter<CommonInnerMsg> writer) {
    return {CommonInnerMsg{.value = request.value + 6}, std::move(writer)};
  }

  using ServerStreamFuture = IdleFuture<SharedMsg, Writer<SharedMsg>>;

  using ClientStreamFuture =
      IdleFuture<Reader<SharedMsg>, UnaryWriter<CommonInnerMsg>>;

  IdleFuture<Reader<SharedMsg>, Writer<SharedMsg>> Bidi(
      Reader<SharedMsg> reader, Writer<SharedMsg> writer) {
    return {std::move(reader), std::move(writer)};
  }
};

// Raw implementations, which echo requests back.
class RawNamesService : public names_raw::Names::Service<RawNamesService> {
 public:
  using PingFuture = RawEchoFuture;

  RawEchoFuture Pong(pw::ConstBuf request, RawUnaryWriter writer) {
    return {std::move(request), std::move(writer)};
  }

  using DeleteFuture = RawEchoFuture;
  using NestedFuture = RawEchoFuture;
};

class RawKindsService : public names_raw::Kinds::Service<RawKindsService> {
 public:
  using UnaryFuture = RawEchoFuture;

  IdleFuture<pw::ConstBuf, RawWriter> ServerStream(pw::ConstBuf request,
                                                   RawWriter writer) {
    return {std::move(request), std::move(writer)};
  }

  using ClientStreamFuture = IdleFuture<RawReader, RawUnaryWriter>;
  using BidiFuture = IdleFuture<RawReader, RawWriter>;
};

class RawNoPackageService
    : public no_package_raw::Service<RawNoPackageService> {
 public:
  using UnaryFuture = RawEchoFuture;
  using ServerStreamFuture = IdleFuture<pw::ConstBuf, RawWriter>;
  using ClientStreamFuture = IdleFuture<RawReader, RawUnaryWriter>;

  IdleFuture<RawReader, RawWriter> Bidi(RawReader reader, RawWriter writer) {
    return {std::move(reader), std::move(writer)};
  }
};

class RawOtherService : public other_raw::Other::Service<RawOtherService> {
 public:
  using GetFuture = RawEchoFuture;
};

// =============================================================================
// Clients
// =============================================================================

// Starts `future`, answers the call as `Method`, and checks the request the
// peer received and the response the client received. Returns false if the
// call did not complete as expected.
template <typename Method, typename Fut>
[[nodiscard]] bool UnaryRoundTrip(test::MockPeer& peer,
                                  async2::DispatcherForTest& dispatcher,
                                  Fut future,
                                  uint32_t request_value,
                                  uint32_t response_value) {
  async2::FutureTask task(std::move(future));
  dispatcher.Post(task);

  auto invocation = peer.ExpectInvocation<Method>();
  auto request = invocation.request();
  EXPECT_EQ(request.status(), pw::OkStatus());
  if (!request.ok()) {
    return false;
  }
  EXPECT_EQ(request->value, request_value);
  EXPECT_FALSE(task.has_value());

  invocation.Finish(
      typename internal::MethodInfo<Method>::Response{.value = response_value});
  EXPECT_TRUE(task.has_value());
  if (!task.has_value()) {
    return false;
  }
  EXPECT_EQ(task.value().status(), pw::OkStatus());
  if (!task.value().ok()) {
    return false;
  }
  EXPECT_EQ(task.value()->value, response_value);
  return task.value()->value == response_value;
}

TEST(CodegenClient, MethodsNamedAfterMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Names::Client client(peer.client());

  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Names::Ping>(
      peer, dispatcher, client.Ping(PingMsg{.value = 1}), 1, 2));
  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Names::Pong>(
      peer, dispatcher, client.Pong(PingMsg{.value = 3}), 3, 4));
}

TEST(CodegenClient, KeywordAndNestedMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Names::Client client(peer.client());

  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Names::Delete>(
      peer, dispatcher, client.Delete(DeleteMsg{.value = 5}), 5, 6));
  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Names::Nested>(
      peer, dispatcher, client.Nested(InnerMsg{.value = 7}), 7, 8));
}

TEST(CodegenClient, ImportedMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Kinds::Client kinds(peer.client());
  const other_pwpb::Other::Client other(peer.client());

  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Kinds::Unary>(
      peer, dispatcher, kinds.Unary(SharedMsg{.value = 9}), 9, 10));
  ASSERT_TRUE(UnaryRoundTrip<other_pwpb::Other::Get>(
      peer, dispatcher, other.Get(SharedMsg{.value = 11}), 11, 12));
}

TEST(CodegenClient, NoPackage) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const no_package_pwpb::Client client(peer.client());

  ASSERT_TRUE(UnaryRoundTrip<no_package_pwpb::Unary>(
      peer, dispatcher, client.Unary(SharedMsg{.value = 13}), 13, 14));
}

TEST(CodegenClient, DefaultConstructedClientCanBeAssigned) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);

  names_pwpb::Names::Client client;
  EXPECT_FALSE(client.is_open());
  client = names_pwpb::Names::Client(peer.client());
  EXPECT_TRUE(client.is_open());

  ASSERT_TRUE(UnaryRoundTrip<names_pwpb::Names::Ping>(
      peer, dispatcher, client.Ping(PingMsg{.value = 17}), 17, 18));
}

TEST(CodegenClient, PwpbStreamingMethodsAreRouted) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Kinds::Client client(peer.client());

  async2::FutureTask server_stream(client.ServerStream(SharedMsg{.value = 15}));
  dispatcher.Post(server_stream);
  auto invocation = peer.ExpectInvocation<names_pwpb::Kinds::ServerStream>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->value, 15u);

  async2::FutureTask client_stream(client.ClientStream());
  dispatcher.Post(client_stream);
  peer.ExpectInvocation<names_pwpb::Kinds::ClientStream>();

  async2::FutureTask bidi(client.Bidi());
  dispatcher.Post(bidi);
  peer.ExpectInvocation<names_pwpb::Kinds::Bidi>();
}

TEST(CodegenClient, NoPackageStreamingMethodsAreRouted) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const no_package_pwpb::Client client(peer.client());

  async2::FutureTask server_stream(client.ServerStream(SharedMsg{.value = 16}));
  dispatcher.Post(server_stream);
  auto invocation = peer.ExpectInvocation<no_package_pwpb::ServerStream>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->value, 16u);

  async2::FutureTask client_stream(client.ClientStream());
  dispatcher.Post(client_stream);
  peer.ExpectInvocation<no_package_pwpb::ClientStream>();

  async2::FutureTask bidi(client.Bidi());
  dispatcher.Post(bidi);
  peer.ExpectInvocation<no_package_pwpb::Bidi>();
}

TEST(CodegenClient, RawUnary) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_raw::Names::Client client(peer.client());

  const std::byte request_bytes[] = {std::byte{0xAB}, std::byte{0x12}};
  async2::FutureTask response(
      client.Delete::Copy(pw::ConstBuf::Unowned(request_bytes)));
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();

  auto invocation = peer.ExpectInvocation<names_raw::Names::Delete>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  test::ExpectBytes(*request, request_bytes);

  const std::byte response_bytes[] = {std::byte{0xCD}, std::byte{0xEF}};
  invocation.Finish(pw::ConstBuf::Unowned(response_bytes));
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  test::ExpectBytes(*response.value(), response_bytes);
}

TEST(CodegenClient, RawUnaryEmptyRequest) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_raw::Names::Client client(peer.client());

  async2::FutureTask response(client.Ping::Copy({}));
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();

  auto invocation = peer.ExpectInvocation<names_raw::Names::Ping>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_TRUE(request->empty());

  invocation.Finish(pw::ConstBuf());
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  EXPECT_TRUE(response.value()->empty());
}

TEST(CodegenClient, RawUnaryReserve) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_raw::Names::Client client(peer.client());

  async2::FutureTask reservation(client.Delete(/*max_message_size=*/4));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  reservation.value()->data()[0] = std::byte{0xAB};
  pw::Result<RawResponseFuture> response_future =
      reservation.value()->Commit(1);
  PW_TEST_ASSERT_OK(response_future);

  async2::FutureTask response(std::move(*response_future));
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();

  auto invocation = peer.ExpectInvocation<names_raw::Names::Delete>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  ASSERT_EQ(request->size(), 1u);
  EXPECT_EQ(request->data()[0], std::byte{0xAB});

  const std::byte response_bytes[] = {std::byte{0xCD}, std::byte{0xEF}};
  invocation.Finish(pw::ConstBuf::Unowned(response_bytes));
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  ASSERT_EQ(response.value()->size(), 2u);
  EXPECT_EQ(response.value()->data()[1], std::byte{0xEF});
}

TEST(CodegenClient, RawStreamingMethodsAreRouted) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_raw::Kinds::Client client(peer.client());

  const std::byte request_bytes[] = {std::byte{0x02}};
  async2::FutureTask server_stream(
      client.ServerStream::Copy(pw::ConstBuf::Unowned(request_bytes)));
  dispatcher.Post(server_stream);
  auto invocation = peer.ExpectInvocation<names_raw::Kinds::ServerStream>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  test::ExpectBytes(*request, request_bytes);

  async2::FutureTask reservation(client.ServerStream(4));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  reservation.value()->data()[0] = std::byte{0x01};
  pw::Result<RawReader> reader = reservation.value()->Commit(1);
  PW_TEST_ASSERT_OK(reader);
  peer.ExpectInvocation<names_raw::Kinds::ServerStream>();

  async2::FutureTask client_stream(client.ClientStream());
  dispatcher.Post(client_stream);
  peer.ExpectInvocation<names_raw::Kinds::ClientStream>();

  async2::FutureTask bidi(client.Bidi());
  dispatcher.Post(bidi);
  peer.ExpectInvocation<names_raw::Kinds::Bidi>();
}

TEST(CodegenClient, PwpbUnaryReserve) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Names::Client client(peer.client());

  async2::FutureTask reservation(client.Ping(/*max_message_size=*/8));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());

  // Encode Ping{.value = 5} in place: field 1, varint 5.
  reservation.value()->data()[0] = std::byte{0x08};
  reservation.value()->data()[1] = std::byte{0x05};
  pw::Result<ResponseFuture<PongMsg>> response_future =
      reservation.value()->Commit(2);
  PW_TEST_ASSERT_OK(response_future);

  async2::FutureTask response(std::move(*response_future));
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();

  auto invocation = peer.ExpectInvocation<names_pwpb::Names::Ping>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->value, 5u);

  invocation.Finish(PongMsg{.value = 6});
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  EXPECT_EQ(response.value()->value, 6u);
}

TEST(CodegenClient, PwpbServerStreamReserve) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto peer = test::MakeMockPeer(dispatcher, allocator);
  const names_pwpb::Kinds::Client client(peer.client());

  async2::FutureTask reservation(client.ServerStream(8));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());

  reservation.value()->data()[0] = std::byte{0x08};
  reservation.value()->data()[1] = std::byte{0x07};
  pw::Result<Reader<SharedMsg>> reader = reservation.value()->Commit(2);
  PW_TEST_ASSERT_OK(reader);

  auto invocation = peer.ExpectInvocation<names_pwpb::Kinds::ServerStream>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->value, 7u);
}

// =============================================================================
// Services
// =============================================================================

// A server with every generated service, and a client connected to it. Tears
// both down when it goes out of scope, even if a test assertion fails.
class TestServer {
 public:
  TestServer() : server_(allocator_, dispatcher_) {}

  ~TestServer() {
    client_.reset();
    static_cast<void>(server_.Close());
    transport_.ResolveAccept(pw::Status::Cancelled());
    dispatcher_.RunUntilStalled();
    EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
  }

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  // Registers `services`, starts the server, and connects `client()` to it.
  // Returns false on failure.
  template <typename... Services>
  [[nodiscard]] bool Start(Services&... services) {
    if (!(server_.RegisterService(services).ok() && ...)) {
      return false;
    }
    if (!server_.RegisterListenerBlocking(transport_).ok()) {
      return false;
    }
    server_.Start();

    auto paired = test::MakePairedConnections(allocator_);
    transport_.ResolveAccept(paired.second);

    async2::FutureTask connect(
        Client::Connect(dispatcher_, allocator_, paired.first));
    dispatcher_.Post(connect);
    dispatcher_.RunUntilStalled();
    if (!connect.has_value() || !connect.value().ok()) {
      return false;
    }
    client_.emplace(std::move(*connect.value()), dispatcher_);
    return true;
  }

  // Runs `future` until it stalls and returns its result, if it completed.
  template <typename Fut>
  std::optional<async2::FutureValue<Fut>> Run(Fut future) {
    async2::FutureTask task(std::move(future));
    dispatcher_.Post(task);
    dispatcher_.RunUntilStalled();
    if (!task.has_value()) {
      return std::nullopt;
    }
    return std::move(task.value());
  }

  const Client& client() const { return *client_; }

  NamesService& name_service() { return name_service_; }
  KindsService& kinds_service() { return kinds_service_; }
  OtherService& other_service() { return other_service_; }
  NoPackageService& no_package_service() { return no_package_service_; }
  RawNamesService& raw_name_service() { return raw_name_service_; }
  RawKindsService& raw_kinds_service() { return raw_kinds_service_; }
  RawOtherService& raw_other_service() { return raw_other_service_; }
  RawNoPackageService& raw_no_package_service() {
    return raw_no_package_service_;
  }

 private:
  allocator::test::AllocatorForTest<32768> allocator_;
  async2::DispatcherForTest dispatcher_;
  test::MockTransport transport_;

  NamesService name_service_;
  KindsService kinds_service_;
  OtherService other_service_;
  NoPackageService no_package_service_;
  RawNamesService raw_name_service_;
  RawKindsService raw_kinds_service_;
  RawOtherService raw_other_service_;
  RawNoPackageService raw_no_package_service_;

  Server server_;
  std::optional<test::ScopedClient> client_;
};

TEST(CodegenService, PwpbServices) {
  TestServer test;
  ASSERT_TRUE(test.Start(test.name_service(),
                         test.kinds_service(),
                         test.other_service(),
                         test.no_package_service()));

  const names_pwpb::Names::Client names(test.client());
  const names_pwpb::Kinds::Client kinds(test.client());
  const other_pwpb::Other::Client other(test.client());
  const no_package_pwpb::Client no_package(test.client());

  // Future type.
  auto ping = test.Run(names.Ping(PingMsg{.value = 1}));
  ASSERT_TRUE(ping.has_value());
  PW_TEST_ASSERT_OK(*ping);
  EXPECT_EQ((*ping)->value, 2u);

  // Member function.
  auto pong = test.Run(names.Pong(PingMsg{.value = 3}));
  ASSERT_TRUE(pong.has_value());
  PW_TEST_ASSERT_OK(*pong);
  EXPECT_EQ((*pong)->value, 30u);
  EXPECT_EQ(test.name_service().pong_calls(), 1);

  auto deleted = test.Run(names.Delete(DeleteMsg{.value = 4}));
  ASSERT_TRUE(deleted.has_value());
  PW_TEST_ASSERT_OK(*deleted);
  EXPECT_EQ((*deleted)->value, 6u);

  auto nested = test.Run(names.Nested(InnerMsg{.value = 5}));
  ASSERT_TRUE(nested.has_value());
  PW_TEST_ASSERT_OK(*nested);
  EXPECT_EQ((*nested)->value, 8u);

  auto unary = test.Run(kinds.Unary(SharedMsg{.value = 6}));
  ASSERT_TRUE(unary.has_value());
  PW_TEST_ASSERT_OK(*unary);
  EXPECT_EQ((*unary)->value, 10u);

  auto get = test.Run(other.Get(SharedMsg{.value = 7}));
  ASSERT_TRUE(get.has_value());
  PW_TEST_ASSERT_OK(*get);
  EXPECT_EQ((*get)->value, 12u);

  auto no_package_unary = test.Run(no_package.Unary(SharedMsg{.value = 8}));
  ASSERT_TRUE(no_package_unary.has_value());
  PW_TEST_ASSERT_OK(*no_package_unary);
  EXPECT_EQ((*no_package_unary)->value, 14u);
}

TEST(CodegenService, RawServicesServePwpbClients) {
  TestServer test;
  ASSERT_TRUE(test.Start(test.raw_name_service(),
                         test.raw_kinds_service(),
                         test.raw_other_service(),
                         test.raw_no_package_service()));

  // The raw services echo the request. Every message used here has a single
  // uint32 field 1, so the pwpb clients receive their request values back.
  const names_pwpb::Names::Client names(test.client());
  auto ping = test.Run(names.Ping(PingMsg{.value = 21}));
  ASSERT_TRUE(ping.has_value());
  PW_TEST_ASSERT_OK(*ping);
  EXPECT_EQ((*ping)->value, 21u);

  auto pong = test.Run(names.Pong(PingMsg{.value = 22}));
  ASSERT_TRUE(pong.has_value());
  PW_TEST_ASSERT_OK(*pong);
  EXPECT_EQ((*pong)->value, 22u);

  const other_pwpb::Other::Client other(test.client());
  auto get = test.Run(other.Get(SharedMsg{.value = 23}));
  ASSERT_TRUE(get.has_value());
  PW_TEST_ASSERT_OK(*get);
  EXPECT_EQ((*get)->value, 23u);
}

// Writes `request` into `call`, and checks that the service echoes it back.
[[nodiscard]] bool RawEchoRoundTrip(TestServer& test,
                                    RawUnaryReserveFuture call,
                                    pw::ConstByteSpan request) {
  auto reservation = test.Run(std::move(call));
  EXPECT_TRUE(reservation.has_value());
  if (!reservation.has_value() || !reservation->ok()) {
    ADD_FAILURE();
    return false;
  }
  pw::Result<RawResponseFuture> response_future =
      test::CommitCopy(**reservation, request);
  PW_TEST_EXPECT_OK(response_future);
  if (!response_future.ok()) {
    return false;
  }

  auto response = test.Run(std::move(*response_future));
  EXPECT_TRUE(response.has_value());
  if (!response.has_value() || !response->ok()) {
    ADD_FAILURE();
    return false;
  }
  test::ExpectBytes(**response, request);
  return (**response).size() == request.size() &&
         std::memcmp((**response).data(), request.data(), request.size()) == 0;
}

TEST(CodegenService, RawClientAndService) {
  TestServer test;
  ASSERT_TRUE(test.Start(test.raw_name_service(),
                         test.raw_kinds_service(),
                         test.raw_no_package_service()));

  const std::byte request[] = {std::byte{0x08}, std::byte{0x2A}};

  const names_raw::Names::Client names(test.client());
  EXPECT_TRUE(RawEchoRoundTrip(test, names.Nested(sizeof(request)), request));

  auto nested = test.Run(names.Nested::Copy(pw::ConstBuf::Unowned(request)));
  ASSERT_TRUE(nested.has_value());
  PW_TEST_ASSERT_OK(*nested);
  test::ExpectBytes(**nested, request);

  const names_raw::Kinds::Client kinds(test.client());
  EXPECT_TRUE(RawEchoRoundTrip(test, kinds.Unary(sizeof(request)), request));

  const no_package_raw::Client no_package(test.client());
  EXPECT_TRUE(
      RawEchoRoundTrip(test, no_package.Unary(sizeof(request)), request));
}

TEST(CodegenService, PwpbClientReservesRequest) {
  TestServer test;
  ASSERT_TRUE(test.Start(test.raw_name_service()));

  // The raw service echoes the request, which is written in place as
  // Ping{.value = 42}: field 1, varint 42.
  const std::byte request[] = {std::byte{0x08}, std::byte{0x2A}};
  const names_pwpb::Names::Client names(test.client());

  auto reservation = test.Run(names.Ping(sizeof(request)));
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(*reservation);
  pw::Result<ResponseFuture<PongMsg>> response_future =
      test::CommitCopy(**reservation, request);
  PW_TEST_ASSERT_OK(response_future);

  auto response = test.Run(std::move(*response_future));
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(*response);
  EXPECT_EQ((*response)->value, 42u);
}

}  // namespace
