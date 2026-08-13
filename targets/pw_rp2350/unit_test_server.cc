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

#include "targets/pw_rp2350/unit_test_server.h"

#include "pw_log/log.h"
#include "pw_rpc_system_server/rpc_server.h"
#include "pw_thread/detached_thread.h"
#include "pw_unit_test/unit_test_service.h"

namespace pw::unit_test {

UnitTestThread unit_test_thread;

void StartUnittestServer(const thread::Options& thread_options) {
  rpc::system_server::Init();
  rpc::system_server::Server().RegisterService(unit_test_thread.service());

  // Start the unit test thread.
  thread::DetachedThread(thread_options, unit_test_thread);

  // Start the RPC server. This will block.
  Status status = rpc::system_server::Start();
  if (!status.ok()) {
    PW_LOG_ERROR("RPC server failed to start: %s", status.str());
  }
}

}  // namespace pw::unit_test
