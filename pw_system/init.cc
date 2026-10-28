// Copyright 2025 The Pigweed Authors
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
#define PW_LOG_MODULE_NAME "pw_system"

#include "pw_system/init.h"

#include "pw_log/log.h"
#include "pw_metric/global.h"
#include "pw_metric/metric_service_pwpb.h"
#include "pw_rpc/echo_service_pwpb.h"
#include "pw_system/config.h"
#include "pw_system/device_service.h"
#include "pw_system/log.h"
#include "pw_system/rpc_server.h"
#include "pw_system/target_hooks.h"
#include "pw_system/work_queue.h"
#include "pw_thread/detached_thread.h"

#if PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE
#include "pw_system/benchmark_service.h"
#endif  // PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE

#if PW_SYSTEM_ENABLE_TRANSFER_SERVICE
#include "pw_system/file_service.h"
#include "pw_system/transfer_service.h"
#endif  // PW_SYSTEM_ENABLE_TRANSFER_SERVICE

#if PW_SYSTEM_ENABLE_TRACE_SERVICE
#include "pw_system/trace_service.h"
#include "pw_trace/trace.h"
#endif  // PW_SYSTEM_ENABLE_TRACE_SERVICE

#include "pw_system/file_manager.h"

#if PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE
#include "pw_system/thread_snapshot_service.h"
#endif  // PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE

#if PW_SYSTEM_ENABLE_CRASH_HANDLER
#include "pw_system/crash_handler.h"
#include "pw_system/crash_snapshot.h"
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

#if PW_SYSTEM_ENABLE_RPC_QUERY_SERVICE
#include "pw_system/query_service.h"
#endif  // PW_SYSTEM_ENABLE_RPC_QUERY_SERVICE

namespace pw::system {
namespace {
metric::MetricService metric_service(metric::global_metrics,
                                     metric::global_groups);

rpc::EchoService echo_service;

void InitImpl() {
#if PW_SYSTEM_ENABLE_TRACE_SERVICE
  // tracing is off by default, requring a user to enable it through
  // the trace service
  PW_TRACE_SET_ENABLED(false);
#endif

  PW_LOG_INFO("System init");

  // Setup logging.
  const Status status = GetLogThread().OpenUnrequestedLogStream(
      kLoggingRpcChannelId, GetRpcServer(), GetLogService());
  if (!status.ok()) {
    PW_LOG_ERROR("Error opening unrequested log streams %d",
                 static_cast<int>(status.code()));
  }

  PW_LOG_INFO("Registering RPC services");
  GetRpcServer().RegisterService(echo_service);
  GetRpcServer().RegisterService(GetLogService());
  GetRpcServer().RegisterService(metric_service);
  RegisterDeviceService(GetRpcServer());

#if PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE
  RegisterBenchmarkService(GetRpcServer());
#endif  // PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE

#if PW_SYSTEM_ENABLE_TRANSFER_SERVICE
  RegisterTransferService(GetRpcServer());
  RegisterFileService(GetRpcServer());
#endif  // PW_SYSTEM_ENABLE_TRANSFER_SERVICE

#if PW_SYSTEM_ENABLE_TRACE_SERVICE
  RegisterTraceService(GetRpcServer(), FileManager::kTraceTransferHandlerId);
#endif  // PW_SYSTEM_ENABLE_TRACE_SERVICE

#if PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE
  RegisterThreadSnapshotService(GetRpcServer());
#endif  // PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE

#if PW_SYSTEM_ENABLE_RPC_QUERY_SERVICE
  RegisterQueryService(GetRpcServer());
#endif  // PW_SYSTEM_ENABLE_RPC_QUERY_SERVICE

  PW_LOG_INFO("Starting threads");
  // Start threads.
  thread::DetachedThread(system::LogThreadOptions(), GetLogThread());
  thread::DetachedThread(system::RpcThreadOptions(), GetRpcDispatchThread());

#if PW_SYSTEM_ENABLE_TRANSFER_SERVICE
  thread::DetachedThread(system::TransferThreadOptions(), GetTransferThread());
  InitTransferService();
#endif  // PW_SYSTEM_ENABLE_TRANSFER_SERVICE

  GetWorkQueue().CheckPushWork(UserAppInit);
}

}  // namespace

void Init() {
#if PW_SYSTEM_ENABLE_CRASH_HANDLER
  RegisterCrashHandler();

  if (HasCrashSnapshot()) {
    PW_LOG_ERROR("==========================");
    PW_LOG_ERROR("======CRASH DETECTED======");
    PW_LOG_ERROR("==========================");
    PW_LOG_ERROR("Crash snapshots available.");
    PW_LOG_ERROR(
        "Run `device.get_crash_snapshots()` to download and clear the "
        "snapshots.");
  } else {
    PW_LOG_DEBUG("No crash snapshot");
  }
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

  thread::DetachedThread(system::WorkQueueThreadOptions(), GetWorkQueue());
  GetWorkQueue().CheckPushWork(InitImpl);
}

}  // namespace pw::system
