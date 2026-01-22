// Copyright 2024 The Pigweed Authors
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

#include <cstdint>
#include <utility>

#include "pw_allocator/libc_allocator.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_span/span.h"

namespace pw::bluetooth::proxy {

namespace {

void UsePassthroughProxy() {
  // Populate H4 buffer to send towards controller.
  std::array<uint8_t, 20> h4_array_from_host{0};
  H4PacketWithH4 h4_span_from_host = {pw::span(h4_array_from_host)};

  // Populate H4 buffer to send towards host.
  std::array<uint8_t, 19> hci_array_from_controller{0};
  H4PacketWithHci h4_span_from_controller = {
      emboss::H4PacketType::COMMAND, pw::span(hci_array_from_controller)};

  pw::Function<void(H4PacketWithHci && packet)> container_send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci packet) {});

  pw::Function<void(H4PacketWithH4 && packet)> container_send_to_controller_fn(
      ([]([[maybe_unused]] H4PacketWithH4&& packet) {}));

  ProxyHost proxy = ProxyHost(std::move(container_send_to_host_fn),
                              std::move(container_send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator::GetLibCAllocator());

  proxy.HandleH4HciFromHost(std::move(h4_span_from_host));

  proxy.HandleH4HciFromController(std::move(h4_span_from_controller));
}

}  // namespace
}  // namespace pw::bluetooth::proxy

int main() {
  pw::bloat::BloatThisBinary();
  pw::bluetooth::proxy::UsePassthroughProxy();
  return 0;
}
