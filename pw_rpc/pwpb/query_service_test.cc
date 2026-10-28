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

#include <algorithm>
#include <array>
#include <utility>

#include "pw_log/log.h"
#include "pw_rpc/internal/method.h"
#include "pw_rpc/internal/method_union.h"
#include "pw_rpc/internal/test_utils.h"
#include "pw_rpc/method_id.h"
#include "pw_rpc/pwpb/test_method_context.h"
#include "pw_rpc/query_service_pwpb.h"
#include "pw_rpc/service.h"
#include "pw_rpc/service_id.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc {
namespace {

void FakeInvoker(const internal::CallContext&, const internal::Packet&) {}

class FakeMethod : public internal::Method {
 public:
  constexpr FakeMethod(uint32_t id = 1)
      : internal::Method(id, FakeInvoker, MethodType::kUnary) {}
};

class FakeMethodUnion : public internal::MethodUnion {
 public:
  constexpr FakeMethodUnion(uint32_t id = 1)
      : impl_({.method = FakeMethod(id)}) {}

  constexpr const internal::Method& method() const { return impl_.method; }

 private:
  union {
    FakeMethod method;
  } impl_;
};

class FakeService : public Service {
 public:
  constexpr FakeService(uint32_t id = 1) : Service(id, kMethods) {}

 private:
  static constexpr std::array<FakeMethodUnion, 0> kMethods = {};
};

template <size_t kNumMethods>
class ManyMethodsService : public Service {
 public:
  constexpr ManyMethodsService(uint32_t id)
      : Service(id, methods_),
        methods_(MakeMethods(std::make_index_sequence<kNumMethods>{})) {}

 private:
  template <size_t... kIndices>
  static constexpr std::array<FakeMethodUnion, kNumMethods> MakeMethods(
      std::index_sequence<kIndices...>) {
    return {FakeMethodUnion(static_cast<uint32_t>(kIndices + 1))...};
  }

  std::array<FakeMethodUnion, kNumMethods> methods_;
};

using ListServicesMethod =
    pw::rpc::internal::MethodInfo<pw::rpc::pw_rpc::pwpb::Query::ListServices>;
using ListMethodsMethod =
    pw::rpc::internal::MethodInfo<pw::rpc::pw_rpc::pwpb::Query::ListMethods>;
using IsServiceAvailableMethod = pw::rpc::internal::MethodInfo<
    pw::rpc::pw_rpc::pwpb::Query::IsServiceAvailable>;
using IsMethodAvailableMethod = pw::rpc::internal::MethodInfo<
    pw::rpc::pw_rpc::pwpb::Query::IsMethodAvailable>;

TEST(QueryService, ListServices_FailsWithNoServerSet) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListServices) context;
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), Status::FailedPrecondition());
}

TEST(QueryService, ListMethods_FailsWithNoServerSet) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListMethods) context;
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), Status::FailedPrecondition());
}

TEST(QueryService, IsServiceAvailable_FailsWithNoServerSet) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsServiceAvailable) context;
  EXPECT_EQ(context.call({}), Status::FailedPrecondition());
}

TEST(QueryService, IsMethodAvailable_FailsWithNoServerSet) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsMethodAvailable) context;
  EXPECT_EQ(context.call({}), Status::FailedPrecondition());
}

TEST(QueryService, ConstructorWithServer) {
  RawFakeChannelOutput<1> output;
  std::array<Channel, 1> channels = {Channel::Create<1>(&output)};
  Server server(channels);
  QueryService service(server);
  EXPECT_TRUE(server.IsServiceRegistered(service));
}

TEST(QueryService, Init_Idempotent) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListServices) context;
  context.service().Init(context.server());
  context.service().Init(context.server());  // idempotent with same server
}

TEST(QueryService, ListServices_Success) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListServices) context;
  context.service().Init(context.server());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), OkStatus());
}

TEST(QueryService, ListServices_Success_ListServices) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListServices) context;
  context.service().Init(context.server());

  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), OkStatus());
  ASSERT_EQ(context.responses().size(), 1u);
  EXPECT_EQ(context.responses()[0].service_ids.size(), 1u);
  EXPECT_EQ(context.responses()[0].service_ids.front(),
            ::pw::rpc::internal::UnwrapServiceId(
                ::pw::rpc::pw_rpc::pwpb::Query::service_id()));
}

TEST(QueryService, ListServices_MultiplePackets) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListServices, 4, 1024) context;
  context.service().Init(context.server());

  std::array<FakeService, 70> services;
  for (FakeService& service : services) {
    context.server().RegisterService(service);
  }

  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), OkStatus());
  ASSERT_EQ(context.responses().size(), 2u);
  EXPECT_EQ(context.responses()[0].service_ids.size(), 64u);
  EXPECT_EQ(context.responses()[1].service_ids.size(), 7u);
}

TEST(QueryService, ListMethods_Success_ListMethods) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListMethods) context;
  context.service().Init(context.server());
  context.call({.service_id = ::pw::rpc::internal::UnwrapServiceId(
                    ::pw::rpc::pw_rpc::pwpb::Query::service_id())});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), OkStatus());
  ASSERT_EQ(context.responses().size(), 1u);
  EXPECT_EQ(context.responses()[0].method_ids.size(), 4u);
  std::array<uint32_t, 4> expected_method_ids = {
      ListServicesMethod::kMethodId,
      ListMethodsMethod::kMethodId,
      IsServiceAvailableMethod::kMethodId,
      IsMethodAvailableMethod::kMethodId,
  };
  for (auto id : expected_method_ids) {
    EXPECT_TRUE(std::find(context.responses()[0].method_ids.begin(),
                          context.responses()[0].method_ids.end(),
                          id) != context.responses()[0].method_ids.end());
  }
}

TEST(QueryService, ListMethods_MultiplePackets) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListMethods, 4, 1024) context;
  context.service().Init(context.server());

  constexpr uint32_t kManyMethodsServiceId = 0x1234;
  ManyMethodsService<70> many_methods_service(kManyMethodsServiceId);
  context.server().RegisterService(many_methods_service);

  context.call({.service_id = kManyMethodsServiceId});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), OkStatus());
  ASSERT_EQ(context.responses().size(), 2u);
  EXPECT_EQ(context.responses()[0].method_ids.size(), 64u);
  EXPECT_EQ(context.responses()[1].method_ids.size(), 6u);
}

TEST(QueryService, ListMethods_Fail) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, ListMethods) context;
  context.service().Init(context.server());
  context.call({.service_id = 0});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(context.status(), Status::NotFound());
}

TEST(QueryService, IsServiceAvailable_Success) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsServiceAvailable) context;
  context.service().Init(context.server());
  EXPECT_EQ(context.call({.service_id = ::pw::rpc::internal::UnwrapServiceId(
                              ::pw::rpc::pw_rpc::pwpb::Query::service_id())}),
            OkStatus());
  EXPECT_TRUE(context.response().is_available);
}

TEST(QueryService, IsServiceAvailable_Fail) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsServiceAvailable) context;
  context.service().Init(context.server());
  EXPECT_EQ(context.call({.service_id = 0}), OkStatus());
  EXPECT_FALSE(context.response().is_available);
}

TEST(QueryService, IsMethodAvailable_Success) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsMethodAvailable) context;
  context.service().Init(context.server());
  EXPECT_EQ(context.call({.service_id = ::pw::rpc::internal::UnwrapServiceId(
                              ::pw::rpc::pw_rpc::pwpb::Query::service_id()),
                          .method_id = ListServicesMethod::kMethodId}),
            OkStatus());
  EXPECT_TRUE(context.response().is_available);
}

TEST(QueryService, IsMethodAvailable_FailInvalidMethod) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsMethodAvailable) context;
  context.service().Init(context.server());
  EXPECT_EQ(context.call({.service_id = ::pw::rpc::internal::UnwrapServiceId(
                              ::pw::rpc::pw_rpc::pwpb::Query::service_id()),
                          .method_id = 0}),
            OkStatus());
  EXPECT_FALSE(context.response().is_available);
}

TEST(QueryService, IsMethodAvailable_FailInvalidService) {
  PW_PWPB_TEST_METHOD_CONTEXT(QueryService, IsMethodAvailable) context;
  context.service().Init(context.server());
  EXPECT_EQ(context.call(
                {.service_id = 0, .method_id = ListServicesMethod::kMethodId}),
            OkStatus());
  EXPECT_FALSE(context.response().is_available);
}

}  // namespace
}  // namespace pw::rpc
