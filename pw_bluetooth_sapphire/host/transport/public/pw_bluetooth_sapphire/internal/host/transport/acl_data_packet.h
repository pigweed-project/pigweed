// Copyright 2023 The Pigweed Authors
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
#include <lib/fit/function.h>

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/packet_view.h"
#include "pw_bluetooth_sapphire/internal/host/common/trace.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/packet.h"

namespace bt::hci {

// Packet template specialization for ACL data packets. This cannot be directly
// instantiated. Represents a HCI ACL data packet.
using ACLDataPacket = Packet<hci_spec::ACLDataHeader>;
using ACLDataPacketPtr = std::unique_ptr<ACLDataPacket>;
using ACLPacketHandler = fit::function<void(ACLDataPacketPtr data_packet)>;

template <>
class Packet<hci_spec::ACLDataHeader>
    : public PacketBase<hci_spec::ACLDataHeader, ACLDataPacket> {
 public:
  // Slab-allocates a new ACLDataPacket with the given payload size without
  // initializing its contents.
  static ACLDataPacketPtr New(uint16_t payload_size);

  // Slab-allocates a new ACLDataPacket with the given payload size and
  // initializes the packet's header field with the given data.
  static ACLDataPacketPtr New(
      hci_spec::ConnectionHandle connection_handle,
      hci_spec::ACLPacketBoundaryFlag packet_boundary_flag,
      hci_spec::ACLBroadcastFlag broadcast_flag,
      uint16_t payload_size = 0u);

  // Getters for the header fields.
  hci_spec::ConnectionHandle connection_handle() const;
  hci_spec::ACLPacketBoundaryFlag packet_boundary_flag() const;
  hci_spec::ACLBroadcastFlag broadcast_flag() const;

  // Initializes the internal PacketView by reading the header portion of the
  // underlying buffer.
  void InitializeFromBuffer();

  // ID to trace packet flow
  void set_trace_id(trace_flow_id_t id) { async_id_ = id; }
  trace_flow_id_t trace_id() { return async_id_; }

 protected:
  using PacketBase<hci_spec::ACLDataHeader, ACLDataPacket>::PacketBase;

 private:
  // Writes the given header fields into the underlying buffer.
  void WriteHeader(hci_spec::ConnectionHandle connection_handle,
                   hci_spec::ACLPacketBoundaryFlag packet_boundary_flag,
                   hci_spec::ACLBroadcastFlag broadcast_flag);

  trace_flow_id_t async_id_ = 0;
};

}  // namespace bt::hci
