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

#include "pw_bluetooth_proxy/internal/l2cap_logical_link.h"

#include <pw_bluetooth/emboss_util.h>

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_sync.h"
#include "pw_log/log.h"
#include "pw_span/cast.h"

namespace pw::bluetooth::proxy::internal {

namespace {

std::optional<LockedL2capChannel> GetLockedChannel(
    Direction direction,
    uint16_t handle,
    uint16_t l2cap_channel_id,
    L2capChannelManager& manager) {
  std::optional<LockedL2capChannel> channel;

  switch (direction) {
    case Direction::kFromController:
      return manager.FindChannelByLocalCid(handle, l2cap_channel_id);
    case Direction::kFromHost:
      return manager.FindChannelByRemoteCid(handle, l2cap_channel_id);
    default:
      PW_LOG_ERROR("Unrecognized Direction enumerator value %d.",
                   cpp23::to_underlying(direction));
      return std::nullopt;
  }
}

static constexpr std::uint8_t kH4PacketIndicatorSize = 1;
constexpr std::uint64_t kH4AclHeaderSize =
    emboss::AclDataFrameHeader::IntrinsicSizeInBytes() + kH4PacketIndicatorSize;
}  // namespace

L2capLogicalLink::L2capLogicalLink(uint16_t connection_handle,
                                   AclTransportType transport,
                                   L2capChannelManager& l2cap_channel_manager,
                                   AclDataChannel& acl_data_channel)
    : connection_handle_(connection_handle),
      transport_(transport),
      acl_data_channel_(acl_data_channel),
      channel_manager_(l2cap_channel_manager),
      signaling_channel_(l2cap_channel_manager) {
  Status status =
      acl_data_channel_.RegisterConnection(connection_handle_, *this);
  if (!status.ok()) {
    PW_LOG_ERROR(
        "%s: Error registering link (handle: "
        "%#x, status: %s)",
        __FUNCTION__,
        connection_handle,
        status.str());
  }
}

L2capLogicalLink::~L2capLogicalLink() {
  Status status = acl_data_channel_.UnregisterConnection(connection_handle_);
  if (!status.ok()) {
    // This can happen after a reset, so it's not a bug, just unlikely.
    PW_LOG_WARN(
        "%s: Error unregistering link (handle: "
        "%#x, status: %s)",
        __FUNCTION__,
        connection_handle_,
        status.str());
  }
}

Status L2capLogicalLink::Init() {
  return signaling_channel_.Init(connection_handle_, transport_);
}

AclDataChannel::ConnectionDelegate::HandleAclDataReturn
L2capLogicalLink::HandleAclData(Direction direction,
                                emboss::AclDataFrameWriter& acl) {
  // This function returns whether or not the frame was handled here.
  // * Return true if the frame was handled by the proxy and should _not_ be
  //   passed on to the other side (Host/Controller).
  // * Return false if the frame was _not_ handled by the proxy and should be
  //   passed on to the other side (Host/Controller).
  //
  // Special care needs to be taken when handling fragments. We don't want the
  // proxy to consume an initial fragment, and then decide to pass a subsequent
  // fragment because we didn't like it. That would cause the receiver to see
  // an unexpected CONTINUING_FRAGMENT.
  //
  // This ACL frame could contain
  // * A complete L2CAP PDU...
  //   * for an unrecognized channel    -> Pass
  //   * for a recognized channel       -> Handle and Consume
  //
  // * An initial fragment (w/ complete L2CAP header)...
  //   * while already recombining      -> Stop recombination and Pass(?)
  //   * for an unrecognized channel    -> Pass
  //   * for a recognized channel       -> Start recombination and Consume
  //
  // * A subsequent fragment (CONTINUING_FRAGMENT)...
  //   * while recombining              -> Recombine fragment and Consume
  //     (we know this must be for an L2CAP channel we care about)
  //   * while not recombining          -> Pass
  const uint16_t handle = acl.header().handle().Read();

  bool is_first = false;
  bool is_fragment = false;

  // Set once we know CID from the first packet or from recombiner.
  uint16_t local_cid;

  // TODO: https://pwbug.dev/392665312 - make this <const uint8_t>
  const pw::span<uint8_t> acl_payload{
      acl.payload().BackingStorage().data(),
      acl.payload().BackingStorage().SizeInBytes()};

  Recombiner& recombiner =
      direction == Direction::kFromController ? rx_recombiner_ : tx_recombiner_;

  // Is this a fragment?
  const emboss::AclDataPacketBoundaryFlag boundary_flag =
      acl.header().packet_boundary_flag().Read();
  switch (boundary_flag) {
    // A subsequent fragment of a fragmented PDU.
    case emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT: {
      // If recombination is not active, these are probably fragments for a
      // PDU that we previously chose not to recombine. Simply ignore them.
      //
      // TODO: https://pwbug.dev/393417198 - This could also be an erroneous
      // continuation of an already-recombined PDU, which would be better to
      // drop.
      if (!recombiner.IsActive()) {
        return {.handled = false, .recombined_buffer = std::nullopt};
      }

      local_cid = recombiner.local_cid();

      is_fragment = true;
      break;
    }
    // Non-fragment or the first fragment of a fragmented PDU.
    case emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE:
    case emboss::AclDataPacketBoundaryFlag::FIRST_FLUSHABLE: {
      is_first = true;

      // Ensure recombination is not already in progress
      if (recombiner.IsActive()) {
        PW_LOG_WARN(
            "Received non-continuation packet %s on connection %#x while "
            "recombination is active! Dropping previous partially-recombined "
            "PDU and handling this first packet normally.",
            DirectionToString(direction),
            handle);

        // Note this conditionally acquires channels_mutex_ which, if nested,
        // is expected to be acquired after/inside connection_mutex_.
        std::optional<LockedL2capChannel> channel = GetLockedChannel(
            direction, handle, recombiner.local_cid(), channel_manager_);
        recombiner.EndRecombination(channel);
      }  // recombiner.IsActive(). channel with channels_mutex_ released.

      // Currently, we require the full L2CAP header: We need the pdu_length
      // field so we know how much data to recombine, and we need the
      // channel_id field so we know whether or not this is a recognized L2CAP
      // channel and therefore whether or not we should recombine it.
      // TODO: https://pwbug.dev/437958454 - Handle fragments that are too
      // small to contain the L2CAP header.
      emboss::BasicL2capHeaderView l2cap_header =
          emboss::MakeBasicL2capHeaderView(acl_payload.data(),
                                           acl_payload.size());
      if (!l2cap_header.Ok()) {
        PW_LOG_ERROR(
            "ACL packet %s on connection %#x does not include full L2CAP "
            "header. Passing on.",
            DirectionToString(direction),
            handle);
        return {.handled = false, .recombined_buffer = std::nullopt};
      }

      local_cid = l2cap_header.channel_id().Read();

      // Is this a channel we care about?
      // Note this conditionally acquires channels_mutex_ which, if nested,
      // is expected to be acquired after/inside connection_mutex_.
      std::optional<LockedL2capChannel> channel =
          GetLockedChannel(direction, handle, local_cid, channel_manager_);
      if (!channel.has_value()) {
        return {.handled = false, .recombined_buffer = std::nullopt};
      }

      const uint16_t acl_payload_size = acl.data_total_length().Read();

      const uint16_t l2cap_frame_length =
          emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
          l2cap_header.pdu_length().Read();

      if (l2cap_frame_length < acl_payload_size) {
        PW_LOG_ERROR(
            "ACL packet %s for channel %#x on connection %#x has payload "
            "(%u bytes) larger than specified L2CAP PDU size (%u bytes). "
            "Dropping.",
            DirectionToString(direction),
            channel->channel().local_cid(),
            handle,
            acl_payload_size,
            l2cap_frame_length);
        return {.handled = true, .recombined_buffer = std::nullopt};
      }

      // Is this the first fragment of a fragmented PDU?
      // The first fragment is recognized when the L2CAP frame length exceeds
      // the ACL frame data_total_length.
      if (l2cap_frame_length > acl_payload_size) {
        is_fragment = true;

        // Start recombination
        pw::Status status = recombiner.StartRecombination(
            *channel,
            l2cap_frame_length,
            /*extra_header_size=*/kH4AclHeaderSize);
        if (!status.ok()) {
          // TODO: https://pwbug.dev/404275508 - This is an acquired channel,
          // so need to do something different than just pass on to AP.
          PW_LOG_ERROR(
              "Cannot start recombination for L2capChannel connection. Will "
              "passthrough."
              "channel: %#x, local_cid: %#x, status:%s",
              channel->channel().connection_handle(),
              channel->channel().local_cid(),
              status.str());
          return {.handled = false, .recombined_buffer = std::nullopt};
        }
      }
      break;
    }  // FIRST_FLUSHABLE. channel with channels_mutex_ released.

    default: {
      PW_LOG_ERROR(
          "Packet %s on connection %#x: Unexpected ACL boundary flag: %u",
          DirectionToString(direction),
          handle,
          cpp23::to_underlying(boundary_flag));
      return {.handled = false, .recombined_buffer = std::nullopt};
    }
  }

  if (is_fragment) {
    // Recombine this fragment

    // If value, includes channels_mutex_ unique_lock.
    std::optional<LockedL2capChannel> channel =
        GetLockedChannel(direction, handle, local_cid, channel_manager_);

    pw::Status recomb_status =
        recombiner.RecombineFragment(channel, acl_payload);

    if (!recomb_status.ok()) {
      if (recomb_status == Status::ResourceExhausted()) {
        PW_LOG_ERROR(
            "Memory exhausted when attempting to recombine packet %s for "
            "channel "
            "%#x on connection %#x. Dropping entire PDU.",
            DirectionToString(direction),
            local_cid,
            handle);
      } else {
        // Given that RecombinationActive is checked above, the only other way
        // this should fail is if the fragment is larger than expected, which
        // can only happen on a continuing fragment, because the first fragment
        // starts recombination above.
        PW_DCHECK(!is_first);

        PW_LOG_ERROR(
            "Received continuation packet %s for channel %#x on connection "
            "%#x over specified PDU length. Dropping entire PDU.",
            DirectionToString(direction),
            local_cid,
            handle);
      }

      recombiner.EndRecombination(channel);
      // We own the channel; drop.
      return {.handled = true, .recombined_buffer = std::nullopt};
    }

    if (!recombiner.IsComplete()) {
      // We are done with this packet and awaiting the remaining fragments.
      return {.handled = true, .recombined_buffer = std::nullopt};
    }

    // Recombination complete!
    // We will collect the recombination buffer from the channel below
    // (outside the connection mutex).

  }  // is_fragment. channel with channels_mutex_ released.

  // At this point we have recombined a valid L2CAP frame. It may be
  // from a single first ACL packet or a series of recombined ones (in which
  // case we should be handling the last continuing packet).
  PW_CHECK((is_first && !is_fragment) || (!is_first && is_fragment));

  // But note, our return value only controls the disposition of the current ACL
  // packet.

  // If value, includes channels_mutex_ unique_lock.
  // We need channel for handling PDU and for recombine buffers.
  // channels_mutex_ must be held as long as `recombined_mbuf` and
  // `send_l2cap_pdu` are accessed to ensure channel is not destroyed.
  std::optional<LockedL2capChannel> channel =
      GetLockedChannel(direction, handle, local_cid, channel_manager_);

  // If recombining, will be set with the recombined PDU. And must be held
  // as long as `send_l2cap_pdu` is accessed.
  std::optional<multibuf::MultiBuf> recombined_mbuf;

  // PDU we will actually send (will be set from first packet or from
  // recombination).
  pw::span<uint8_t> send_l2cap_pdu;

  if (!channel.has_value()) {
    // We don't have the channel anymore.  This indicates that the
    // channel instance that recombination was started with has since been
    // destroyed. So "drop" the PDU and handle the packet.
    PW_LOG_INFO(
        "Dropping first PDU %s original intended for channel %#x on connection "
        "%#x since channel instance was destroyed by client since first packet "
        "was received.",
        DirectionToString(direction),
        local_cid,
        handle);
    // TODO: https://pwbug.dev/402454277 - We might want to consider passing
    // kUnhandled for "signaling" channels, but since we don't have the channel
    // here we have no way to determine the channel type. Once we have shared
    // channel refs we should revisit.
    return {.handled = true, .recombined_buffer = std::nullopt};
  }

  if (is_first) {
    // We have whole PDU in first packet.
    send_l2cap_pdu = acl_payload;
  } else {
    // We are a fragment, so we need to collect the recombined PDU from the
    // channel.

    if (!Recombiner::HasBuf(channel, direction)) {
      // To get here we must have a `channel`, but now we have found `channel
      // doesn't have a recombination buf. This indicates `channel` is instance
      // other than the one we started recombination with. So "drop" the PDU and
      // handle the packet.
      PW_LOG_INFO(
          "Dropping recombined PDU %s originally intended for channel %#x on "
          "connection %#x since channel instance was destroyed by client since "
          "first packet was received.",
          DirectionToString(direction),
          local_cid,
          handle);
      // TODO: https://pwbug.dev/392663102 - Revisit what best behavior is here
      // when we work on support for rejecting a recombined L2CAP PDU.
      return {.handled = true, .recombined_buffer = std::nullopt};
    }

    // Store the recombined multibuf.
    recombined_mbuf = Recombiner::TakeBuf(channel, direction);
    send_l2cap_pdu =
        span_cast<uint8_t>(recombined_mbuf->ContiguousSpan().value());

  }  // is_first else

#if PW_BLUETOOTH_PROXY_ASYNC == 0
  // We borrow the channel and release the `LockedL2capChannel` (and thus
  // `channels_mutex`) before calling the client-provided `from_controller_fn_`
  // or `from_host_fn_`. This avoids a deadlock if the client calls `Write`
  // on the channel from within the callback, as `Write` also acquires
  // `channels_mutex`.
  // TODO: https://pwbug.dev/403567488 - Look at sending MultiBuf here rather
  // than span. Channels at next level will create MultiBuf to pass on their
  // payload anyway.
  BorrowedL2capChannel temp_channel = channel->channel().Borrow();
#else
  L2capChannel* temp_channel = &channel->channel();
#endif
  channel.reset();

  const bool handled =
      (direction == Direction::kFromController)
          ? temp_channel->HandlePduFromController(send_l2cap_pdu)
          : temp_channel->HandlePduFromHost(send_l2cap_pdu);

  if (!handled && recombined_mbuf.has_value()) {
    // Client rejected the entire PDU. So grab extra header for H4/ACL headers,
    // populate them, and pass that H4 packet on to the host.

    // Take back the extra header we reserved when starting the recombine.
    PW_CHECK(recombined_mbuf->ClaimPrefix(kH4AclHeaderSize));
    pw::span<uint8_t> h4_span =
        span_cast<uint8_t>(recombined_mbuf->ContiguousSpan().value());

    std::optional<uint16_t> max_acl_packet_length =
        channel_manager_.MaxDataPacketLengthForTransport(transport_);
    if (max_acl_packet_length.has_value()) {
      const size_t kMaxH4Length = kH4PacketIndicatorSize + kH4AclHeaderSize +
                                  max_acl_packet_length.value();
      if (h4_span.size() > kMaxH4Length) {
        //  TODO: https://pwbug.dev/438543613 - Re-frag in this case.
        PW_LOG_ERROR(
            "Recombined H4 length %zu is greater than allowed with "
            "max acl length of "
            "of %u for transport %u. Will still pass on single ACL packet.",
            h4_span.size(),
            *max_acl_packet_length,
            cpp23::to_underlying(transport_));
      }
    } else {
      PW_LOG_WARN(
          "max acl packet length not known, so unable to check H4 "
          "length.");
    }

    // Populate the H4 and ACL headers ahead of the recombined PDU.
    h4_span[0] = static_cast<uint8_t>(emboss::H4PacketType::ACL_DATA);
    pw::span<uint8_t> hci_span = h4_span.subspan(kH4PacketIndicatorSize);
    Result<emboss::AclDataFrameWriter> recombined_acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_span);
    PW_CHECK_OK(recombined_acl);
    recombined_acl->header().handle().Write(acl.header().handle().Read());
    // Controller to Host are always flushable (except for loopback), per
    // Volume 4, Part E, 5.4.2, Packet_Boundary_Flag table.
    recombined_acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_FLUSHABLE);
    recombined_acl->header().broadcast_flag().Write(
        acl.header().broadcast_flag().Read());
    recombined_acl->data_total_length().Write(h4_span.size() -
                                              kH4AclHeaderSize);

    // We still return handled = true here since the fragment ACL packet was
    // consumed to make the recombined packet.
    return {.handled = true,
            .recombined_buffer = std::move(recombined_mbuf.value())};
  }

  return {.handled = handled, .recombined_buffer = std::nullopt};
}

}  // namespace pw::bluetooth::proxy::internal
