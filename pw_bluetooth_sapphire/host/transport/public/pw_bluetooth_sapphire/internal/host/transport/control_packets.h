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
#include <pw_bluetooth/hci_common.emb.h>

#include <utility>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_multibuf/v2/multibuf.h"

namespace bt::hci {

template <class ViewT>
class CommandPacketT;

// CommandPacket is the HCI Command packet specialization of
// DynamicPacket.
class CommandPacket : public DynamicPacket {
 public:
  // Construct an HCI Command packet from an Emboss view T and initialize its
  // header with the |opcode| and size.
  template <typename T>
  static CommandPacketT<T> New(pw::bluetooth::emboss::OpCode opcode) {
    return New<T>(opcode, T::IntrinsicSizeInBytes().Read());
  }

  template <typename T>
  static CommandPacketT<T> New(hci_spec::OpCode opcode) {
    return New<T>(opcode, T::IntrinsicSizeInBytes().Read());
  }

  // Construct an HCI Command packet from an Emboss view T of |packet_size|
  // total bytes (header + payload) and initialize its header with the |opcode|
  // and size. This constructor is meant for variable size packets, for which
  // clients must calculate packet size manually.
  template <typename T>
  static CommandPacketT<T> New(pw::bluetooth::emboss::OpCode opcode,
                               size_t packet_size) {
    CommandPacketT<T> packet(opcode, packet_size);
    return packet;
  }

  template <typename T>
  static CommandPacketT<T> New(hci_spec::OpCode opcode, size_t packet_size) {
    CommandPacketT<T> packet(opcode, packet_size);
    return packet;
  }

  hci_spec::OpCode opcode() const;
  // Returns the OGF (OpCode Group Field) which occupies the upper 6-bits of the
  // opcode.
  uint8_t ogf() const;
  // Returns the OCF (OpCode Command Field) which occupies the lower 10-bits of
  // the opcode.
  uint16_t ocf() const;

 protected:
  explicit CommandPacket(pw::bluetooth::emboss::OpCode opcode,
                         size_t packet_size);
  explicit CommandPacket(hci_spec::OpCode opcode, size_t packet_size);

 private:
  pw::bluetooth::emboss::CommandHeaderView header_view() const;
};

// Helper subclass that remembers the view type it was constructed with. It is
// safe to slice a CommandPacketT into a CommandPacket.
template <class ViewT>
class CommandPacketT : public CommandPacket {
 public:
  ViewT view_t() { return view<ViewT>(); }
  ViewT view_t(size_t size) { return view<ViewT>(size); }

 private:
  friend class CommandPacket;

  CommandPacketT(pw::bluetooth::emboss::OpCode opcode, size_t packet_size)
      : CommandPacket(opcode, packet_size) {}

  CommandPacketT(hci_spec::OpCode opcode, size_t packet_size)
      : CommandPacket(opcode, packet_size) {}
};

template <class ViewT>
class EventPacketT;

// EventPacket is the HCI Event packet specialization of DynamicPacket.
class EventPacket : public DynamicPacket {
 public:
  // Construct an HCI Event packet of |packet_size| total bytes (header +
  // payload).
  static EventPacket New(size_t packet_size) {
    EventPacket packet(packet_size);
    return packet;
  }

  static EventPacket New(pw::multibuf::v2::MultiBuf::Instance&& buf) {
    EventPacket packet(std::move(buf));
    return packet;
  }

  // Construct an HCI Event packet from an Emboss view T and initialize its
  // header with the |event_code| and size.
  template <typename T>
  static EventPacketT<T> New(pw::bluetooth::emboss::EventCode event_code) {
    EventPacketT<T> packet(T::IntrinsicSizeInBytes().Read());
    auto header =
        packet.template view<pw::bluetooth::emboss::EventHeaderWriter>();
    header.event_code().Write(event_code);
    header.parameter_total_size().Write(
        T::IntrinsicSizeInBytes().Read() -
        pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes());
    return packet;
  }

  template <typename T>
  static EventPacketT<T> New(hci_spec::EventCode event_code) {
    EventPacketT<T> packet(T::IntrinsicSizeInBytes().Read());
    auto header =
        packet.template view<pw::bluetooth::emboss::EventHeaderWriter>();
    header.event_code_uint().Write(event_code);
    header.parameter_total_size().Write(
        T::IntrinsicSizeInBytes().Read() -
        pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes());
    return packet;
  }

  // Construct an HCI Event packet from an Emboss view T of |packet_size| total
  // bytes (header + payload) and initialize its header with the |event_code|
  // and size. This constructor is meant for variable size packets, for which
  // clients must calculate packet size manually.
  template <typename T>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  static EventPacketT<T> New(hci_spec::EventCode event_code,
                             size_t packet_size) {
    EventPacketT<T> packet(packet_size);
    auto header =
        packet.template view<pw::bluetooth::emboss::EventHeaderWriter>();
    header.event_code_uint().Write(event_code);
    header.parameter_total_size().Write(
        packet_size -
        pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes());
    return packet;
  }

  hci_spec::EventCode event_code() const;

  // If this event packet contains a StatusCode field, this method returns the
  // status. Not all events contain a StatusCode and not all of those that do
  // are supported by this method. Returns std::nullopt for such events.
  //
  // NOTE: If you intend to use this with a new event code, make sure to add an
  // entry to the implementation in control_packets.cc.
  std::optional<pw::bluetooth::emboss::StatusCode> StatusCode() const;

  // Returns a status if this event represents the result of an operation. See
  // the documentation on StatusCode() as the same conditions apply to this
  // method. Returns a default status of type HostError::kMalformedPacket on
  // error.
  hci::Result<> ToResult() const;

 protected:
  explicit EventPacket(size_t packet_size);

  explicit EventPacket(pw::multibuf::v2::MultiBuf::Instance&& buf)
      : DynamicPacket(std::move(buf)) {}

 private:
  // From an Emboss view T containing a StatusCode field named "status", returns
  // the status. Returns std::nullopt on error.
  template <typename T>
  std::optional<pw::bluetooth::emboss::StatusCode> StatusCodeFromView() const {
    // Don't use view(), which asserts on IsComplete().
    T packet_view(data().data(), size());
    if (!packet_view.status().Ok()) {
      return std::nullopt;
    }
    return packet_view.status().UncheckedRead();
  }
};

// Helper subclass that remembers the view type it was constructed with. It is
// safe to slice an EventPacketT into an EventPacket.
template <class ViewT>
class EventPacketT : public EventPacket {
 public:
  template <typename... Args>
  ViewT view_t(Args... args) {
    return view<ViewT>(args...);
  }

  template <typename... Args>
  ViewT unchecked_view_t(Args... args) {
    return unchecked_view<ViewT>(args...);
  }

 private:
  friend class EventPacket;

  explicit EventPacketT(size_t packet_size) : EventPacket(packet_size) {}
};

}  // namespace bt::hci

// Convenience macro to check and log any non-Success status of an event.
// Evaluate to true if the event status is not success.
#define HCI_IS_ERROR(event, flag, tag, /*fmt*/...) \
  bt_is_error(event.ToResult(), flag, tag, __VA_ARGS__)
