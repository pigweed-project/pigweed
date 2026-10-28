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

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_rpc/query.rpc.pwpb.h"
#include "pw_rpc/query_service_pwpb.h"
#include "pw_rpc/server.h"

namespace pw::rpc {

void QueryService::Init(pw::rpc::Server& server) {
  rpc_server_ = &server;
  if (!rpc_server_->IsServiceRegistered(*this)) {
    server.RegisterService(*this);
  }
}

void QueryService::ListServices(
    const pwpb::ListServicesRequest::Message& request,
    PwpbServerWriter<pwpb::ListServicesResponse::Message>& writer) {
  PW_LOG_DEBUG("QueryService: ListServices");
  static_cast<void>(request);
  if (rpc_server_ == nullptr) {
    PW_LOG_ERROR("QueryService: rpc_server_ is not set");
    PW_CHECK_OK(writer.Finish(Status::FailedPrecondition()));
    return;
  }

  pwpb::ListServicesResponse::Message response{};
  size_t start_index = 0;
  do {
    response.service_ids.clear();
    size_t current_index = 0;
    rpc_server_->ForEachService([&](const ::pw::rpc::Service& service) {
      if (current_index++ < start_index) {
        return;
      }
      if (!response.service_ids.full()) {
        response.service_ids.push_back(
            internal::UnwrapServiceId(service.service_id()));
        ++start_index;
      }
    });
    if (!response.service_ids.empty()) {
      PW_CHECK_OK(writer.Write(response));
    }
  } while (response.service_ids.full());
  PW_CHECK_OK(writer.Finish(OkStatus()));
}

void QueryService::ListMethods(
    const pwpb::ListMethodsRequest::Message& request,
    PwpbServerWriter<pwpb::ListMethodsResponse::Message>& writer) {
  PW_LOG_DEBUG("QueryService: ListMethods");
  if (rpc_server_ == nullptr) {
    PW_LOG_ERROR("QueryService: rpc_server_ is not set");
    PW_CHECK_OK(writer.Finish(Status::FailedPrecondition()));
    return;
  }

  const auto* service =
      rpc_server_->FindServiceIf([&request](const ::pw::rpc::Service& s) {
        return internal::UnwrapServiceId(s.service_id()) == request.service_id;
      });
  if (service == nullptr) {
    PW_CHECK_OK(writer.Finish(Status::NotFound()));
    return;
  }

  pwpb::ListMethodsResponse::Message response{};
  size_t start_index = 0;
  do {
    response.method_ids.clear();
    size_t current_index = 0;
    service->ForEachMethod([&](const internal::Method& method) {
      if (current_index++ < start_index) {
        return;
      }
      if (!response.method_ids.full()) {
        response.method_ids.push_back(method.id());
        ++start_index;
      }
    });
    if (!response.method_ids.empty()) {
      PW_CHECK_OK(writer.Write(response));
    }
  } while (response.method_ids.full());
  PW_CHECK_OK(writer.Finish(OkStatus()));
}

Status QueryService::IsServiceAvailable(
    const pwpb::IsServiceAvailableRequest::Message& request,
    pwpb::IsAvailableResponse::Message& response) {
  PW_LOG_DEBUG("QueryService: IsServiceAvailable");
  if (rpc_server_ == nullptr) {
    PW_LOG_ERROR("QueryService: rpc_server_ is not set");
    return Status::FailedPrecondition();
  }

  const auto* service =
      rpc_server_->FindServiceIf([&request](const ::pw::rpc::Service& s) {
        return internal::UnwrapServiceId(s.service_id()) == request.service_id;
      });
  response.is_available = (service != nullptr);
  return OkStatus();
}

Status QueryService::IsMethodAvailable(
    const pwpb::IsMethodAvailableRequest::Message& request,
    pwpb::IsAvailableResponse::Message& response) {
  PW_LOG_DEBUG("QueryService: IsMethodAvailable");
  if (rpc_server_ == nullptr) {
    PW_LOG_ERROR("QueryService: rpc_server_ is not set");
    return Status::FailedPrecondition();
  }

  const auto* service =
      rpc_server_->FindServiceIf([&request](const ::pw::rpc::Service& s) {
        return internal::UnwrapServiceId(s.service_id()) == request.service_id;
      });
  if (service == nullptr) {
    response.is_available = false;
  } else {
    response.is_available = service->HasMethod(request.method_id);
  }
  return OkStatus();
}

}  // namespace pw::rpc
