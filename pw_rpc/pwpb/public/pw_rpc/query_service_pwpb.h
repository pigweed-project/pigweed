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

#include "pw_rpc/pwpb/server_reader_writer.h"
#include "pw_rpc/query.rpc.pwpb.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"

namespace pw::rpc {

class QueryService final : public pw_rpc::pwpb::Query::Service<QueryService> {
 public:
  constexpr QueryService() = default;
  explicit QueryService(pw::rpc::Server& server) { Init(server); }

  void Init(pw::rpc::Server& server);

  void ListServices(
      const pwpb::ListServicesRequest::Message& request,
      PwpbServerWriter<pwpb::ListServicesResponse::Message>& writer);

  void ListMethods(
      const pwpb::ListMethodsRequest::Message& request,
      PwpbServerWriter<pwpb::ListMethodsResponse::Message>& writer);

  Status IsServiceAvailable(
      const pwpb::IsServiceAvailableRequest::Message& request,
      pwpb::IsAvailableResponse::Message& response);

  Status IsMethodAvailable(
      const pwpb::IsMethodAvailableRequest::Message& request,
      pwpb::IsAvailableResponse::Message& response);

 private:
  pw::rpc::Server* rpc_server_ = nullptr;
};

}  // namespace pw::rpc
