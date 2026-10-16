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

// Generated code specializes `ServiceInfo` on each generated `Service<Impl>`
// base class. Each header forward declares the other flavor's `Client`, so a
// client type is complete only if its generated header is included.
//
//   ServiceInfo<MyEchoService>::RawClient
//   ServiceInfo<MyEchoService>::PwpbClient

namespace pw::rpc2::internal {

/// Reads the private alias with which a generated `Service<Impl>` base class
/// identifies itself. Generated base classes befriend this class.
class ServiceInfoAccess {
 private:
  // Access to the private alias is checked here, in a member of the friend, so
  // that an inaccessible or missing alias is a substitution failure.
  template <typename T, typename = typename T::PwInternal_GeneratedService>
  static constexpr bool IsService(int) {
    return true;
  }

  template <typename>
  static constexpr bool IsService(...) {
    return false;
  }

 public:
  /// True if `T` is a generated `Service<Impl>` base class or derives from
  /// one.
  template <typename T>
  static constexpr bool kIsService = IsService<T>(0);

  /// The generated `Service<Impl>` base class of `T`.
  ///
  /// This is a member class rather than an alias template: GCC checks access
  /// in an alias template's substitution from the point of use.
  template <typename T>
  struct GeneratedService {
    using type = typename T::PwInternal_GeneratedService;
  };
};

/// Compile-time metadata for a generated RPC service, looked up by a service
/// implementation or its generated `Service<Impl>` base class.
template <typename ServiceImpl,
          bool = ServiceInfoAccess::kIsService<ServiceImpl>>
struct ServiceInfo {
  static_assert(ServiceInfoAccess::kIsService<ServiceImpl>,
                "Template argument must be a pw_rpc2 service implementation, "
                "which derives from a generated Service<Impl> base class such "
                "as my_pkg::pw_rpc2::pwpb::MyService::Service<Impl>.");
};

// Resolves a service implementation to its generated base class, which
// generated code specializes `ServiceInfo` on. Generated specializations spell
// out `true`, since a partial specialization's arguments may not depend on its
// parameters through the default argument's expression.
template <typename ServiceImpl>
struct ServiceInfo<ServiceImpl, true>
    : ServiceInfo<
          typename ServiceInfoAccess::GeneratedService<ServiceImpl>::type> {};

}  // namespace pw::rpc2::internal
