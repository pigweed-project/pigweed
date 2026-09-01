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

// DOCSTAG: [pw_bluetooth_proxy-examples-crash-recovery]
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/hci/command_multiplexer.h"
#include "pw_bluetooth_proxy/hci/sniff_offload_manager.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_manager.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace {

namespace emboss = ::pw::bluetooth::emboss;
using namespace pw::bluetooth::proxy;

// Example platform transport controller managing host and controller queues.
class TransportController {
 public:
  void Pause() {}
  void Resume() {}

  void SendToHost(H4PacketWithHci&&) {}
  void SendToHost(pw::multibuf::v2::MultiBuf::Instance&&) {}
  void SendToController(H4PacketWithH4&&) {}
  void SendToController(pw::multibuf::v2::MultiBuf::Instance&&) {}

  pw::Status SendCommand(pw::multibuf::v2::MultiBuf::Instance&&,
                         hci::SniffOffloadManager::CompletionEvent) {
    return pw::OkStatus();
  }

  pw::Status SendEvent(pw::multibuf::v2::MultiBuf::Instance&&) {
    return pw::OkStatus();
  }
};

// Persistent snapshot storage maintained across reboots by container.
// Applications only include snapshot records for active subsystems.
struct PersistentStorage {
  // Core proxy snapshot (mandatory when recovery is enabled)
  ProxyHostSnapshot proxy_snapshot;

  // Optional subsystem snapshots (included only if used)
  hci::CommandMultiplexerSnapshot cm_snapshot;
  hci::SniffSnapshot sniff_snapshot;
  rfcomm::RfcommSnapshot rfcomm_snapshot;
} g_storage;

// 1. Define state update delegates using ApplyStateUpdate helpers.
void OnProxyStateUpdate(const ProxyHostStateUpdate& update) {
  static_cast<void>(g_storage.proxy_snapshot.ApplyStateUpdate(update));
}

// Optional delegates (define only for active subsystems):
void OnCommandMultiplexerStateUpdate(
    const hci::CommandMultiplexerStateUpdate& update) {
  static_cast<void>(g_storage.cm_snapshot.ApplyStateUpdate(update));
}

void OnSniffStateUpdate(const hci::SniffStateUpdate& update) {
  static_cast<void>(g_storage.sniff_snapshot.ApplyStateUpdate(update));
}

void OnRfcommStateUpdate(const rfcomm::RfcommStateUpdate& update) {
  static_cast<void>(g_storage.rfcomm_snapshot.ApplyStateUpdate(update));
}

void RebindClientChannels(ProxyHost& /*proxy_host*/,
                          rfcomm::RfcommManager& /*rfcomm_manager*/) {
  // Application re-acquires active L2CAP and RFCOMM channels during the
  // recovery window.
}

// 2. Container Recovery Routine
pw::Status PerformCrashRecovery(pw::Allocator& allocator,
                                pw::async2::Dispatcher& dispatcher,
                                TransportController& transport) {
  // STEP 1: Pause traffic.
  transport.Pause();

  // STEP 2: Reconstruct active subsystem managers with injected delegates.
  // (Omit any managers not used by your application)
  hci::CommandMultiplexer command_multiplexer(
      allocator,
      [&](pw::multibuf::v2::MultiBuf::Instance&& p) {
        transport.SendToHost(std::move(p));
      },
      [&](pw::multibuf::v2::MultiBuf::Instance&& p) {
        transport.SendToController(std::move(p));
      },
      OnCommandMultiplexerStateUpdate);

  ProxyHost proxy_host(
      [&](H4PacketWithHci&& p) { transport.SendToHost(std::move(p)); },
      [&](H4PacketWithH4&& p) { transport.SendToController(std::move(p)); },
      allocator,
      OnProxyStateUpdate);

  hci::SniffOffloadManager sniff_manager(
      allocator,
      dispatcher,
      [&](pw::multibuf::v2::MultiBuf::Instance&& p,
          hci::SniffOffloadManager::CompletionEvent completion) {
        return transport.SendCommand(std::move(p), completion);
      },
      [&](pw::multibuf::v2::MultiBuf::Instance&& p) {
        return transport.SendEvent(std::move(p));
      },
      /*on_error=*/nullptr,
      OnSniffStateUpdate);

  rfcomm::RfcommManager rfcomm_manager(
      proxy_host, allocator, OnRfcommStateUpdate);

  // STEP 3: RecoverFromSnapshot (bottom-up dependency order).
  // Decouple the pre-crash baseline snapshot from the live container storage.
  // ProxyHost and RfcommManager retain raw snapshot pointers until
  // CompleteRecovery() finishes.
  ProxyHostSnapshot baseline_proxy_snapshot = g_storage.proxy_snapshot;
  rfcomm::RfcommSnapshot baseline_rfcomm_snapshot = g_storage.rfcomm_snapshot;

  // Only restore the subsystems present in your application.
  PW_TRY(command_multiplexer.RecoverFromSnapshot(g_storage.cm_snapshot));
  PW_TRY(proxy_host.RecoverFromSnapshot(&baseline_proxy_snapshot));
  PW_TRY(sniff_manager.RecoverFromSnapshot(g_storage.sniff_snapshot));
  PW_TRY(rfcomm_manager.RecoverFromSnapshot(&baseline_rfcomm_snapshot));

  // STEP 4: Re-registration window.
  // Re-apply required HCI event filters (these are client-driven and not
  // restored automatically by ProxyHost):
  proxy_host.SetEventBlocked(emboss::EventCode::INQUIRY_COMPLETE, true);
  proxy_host.SetLeSubeventBlocked(emboss::LeSubEventCode::CONNECTION_COMPLETE,
                                  false);

  // Clients re-acquire active channels for the protocols in use.
  RebindClientChannels(proxy_host, rfcomm_manager);

  // STEP 5: CompleteRecovery (top-down dependency order).
  // Sweeps must be called in top-down order for active managers.
  rfcomm_manager.CompleteRecovery();
  proxy_host.CompleteRecovery();

  // STEP 6: Resume traffic.
  transport.Resume();

  // STEP 7: Post-resumption dispatch resynchronization.
  // Trigger resynchronization only for active subsystems.
  proxy_host.InitiateAclCreditResynchronization();
  sniff_manager.InitiateHardwareResynchronization();

  return pw::OkStatus();
}

}  // namespace
// DOCSTAG: [pw_bluetooth_proxy-examples-crash-recovery]

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_unit_test/framework.h"

namespace {

TEST(CrashRecoveryExampleTest, ExecutesRecoverySequence) {
  pw::allocator::test::AllocatorForTest<4096> allocator;
  pw::async2::DispatcherForTest dispatcher;
  TransportController transport;

  PW_TEST_EXPECT_OK(PerformCrashRecovery(allocator, dispatcher, transport));
}

}  // namespace
