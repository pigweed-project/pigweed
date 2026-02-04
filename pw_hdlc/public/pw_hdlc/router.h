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
#pragma once

//         __      ___   ___ _  _ ___ _  _  ___
//         \ \    / /_\ | _ \ \| |_ _| \| |/ __|
//          \ \/\/ / _ \|   / .` || || .` | (_ |
//           \_/\_/_/ \_\_|_\_|\_|___|_|\_|\___|
//  _____  _____ ___ ___ ___ __  __ ___ _  _ _____ _   _
// | __\ \/ / _ \ __| _ \_ _|  \/  | __| \| |_   _/_\ | |
// | _| >  <|  _/ _||   /| || |\/| | _|| .` | | |/ _ \| |__
// |___/_/\_\_| |___|_|_\___|_|  |_|___|_|\_| |_/_/ \_\____|
//
// This module is in an early, experimental state. The APIs are in flux and may
// change without notice. Please do not rely on it in production code, but feel
// free to explore and share feedback with the Pigweed team!

#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_channel/channel.h"
#include "pw_containers/vector.h"
#include "pw_hdlc/decoder.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"

namespace pw::hdlc {

/// @module{pw_hdlc}

/// A router that multiplexes multiple datagram-oriented ``Channel`` s
/// over a single byte-oriented ``Channel`` using HDLC framing.
class Router final {
 public:
  class Registration;

  /// Constructs a ``Router``
  ///
  /// @param[in] io_channel  The channel on which to send and receive encoded
  ///  HDLC packets.
  ///
  /// @param[in] decode_buffer  The memory to use for storing partially-decoded
  ///  HDLC frames. This buffer should be at least
  ///  ``Decoder::RequiredBufferSizeForFrameSize(frame_size)`` bytes in order
  ///  to ensure that HDLC frames of size ``frame_size`` can be successfully
  ///  decoded.
  Router(pw::channel::ByteReaderWriter& io_channel, ByteSpan decode_buffer)
      : io_channel_(io_channel), decoder_(decode_buffer) {}

  // Router is not copyable or movable.
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;
  Router(Router&&) = delete;
  Router& operator=(Router&&) = delete;

  /// Registers a ``Channel`` tied to the provided addresses.
  ///
  /// All incoming HDLC messages received on ``io_channel`` with HDLC address
  /// ``receive_address`` will be decoded and routed to the provided
  /// ``channel``.
  ///
  /// Data read from ``channel`` will be HDLC-encoded and sent to
  /// ``io_channel``.
  ///
  /// Note that a non-writeable channel will exert backpressure on the entire
  /// router, so channels should strive to consume or discard incoming data as
  /// quickly as possible in order to prevent starvation of other channels.
  ///
  /// @param[in] receive_address    Incoming HDLC messages received on the
  ///  external ``io_channel`` with an address matching ``receive_address``
  ///  will be decoded and written to ``channel``.
  ///
  /// @param[in] send_address    Data read from ``channel`` will be written
  ///  to ``io_channel`` with the HDLC address ``send_address``.
  ///
  /// @returns
  /// * @OK: `Channel` was successfully registered.
  /// * @ALREADY_EXISTS: A registration already exists for either `channel`,
  ///   `receive_address`, or `send_address`. Channels may not be registered
  ///   with multiple addresses, nor may addresses be used with multiple
  ///   channels.
  Status AddChannel(pw::channel::DatagramReaderWriter& channel,
                    uint64_t receive_address,
                    uint64_t send_address);

  /// Removes a previously registered ``Channel`` tied to the provided
  /// addresses.
  ///
  /// @returns
  /// * @OK: The channel was successfully deregistered.
  /// * @NOT_FOUND: A registration of the channel for the provided addresses was
  ///   not found.
  Status RemoveChannel(pw::channel::DatagramReaderWriter& channel,
                       uint64_t receive_address,
                       uint64_t send_address);

  /// Progress the router as far as possible, waking the provided ``cx``
  /// when more progress can be made.
  ///
  /// This will only return ``Ready`` if ``io_channel`` has been observed as
  /// closed, after which all messages have been flushed to the remaining
  /// channels and the channels have been closed.
  pw::async2::Poll<> Pend(pw::async2::Context& cx);

  /// Closes all underlying channels, attempting to flush any data.
  pw::async2::Poll<> PendClose(pw::async2::Context& cx);

 private:
  // TODO: https://pwbug.dev/329902904 - Use allocator-based collections and
  // remove this arbitrary limit.
  constexpr static size_t kSomeNumberOfChannels = 16;

  /// A channel associated with an incoming and outgoing address.
  struct ChannelData {
    ChannelData(pw::channel::DatagramReaderWriter& channel_arg,
                uint64_t receive_address_arg,
                uint64_t send_address_arg)
        : channel(&channel_arg),
          receive_address(receive_address_arg),
          send_address(send_address_arg) {}

    ChannelData(const ChannelData&) = delete;
    ChannelData& operator=(const ChannelData&) = delete;
    ChannelData(ChannelData&&) = default;
    ChannelData& operator=(ChannelData&&) = default;

    /// A channel which reads and writes datagrams.
    pw::channel::DatagramReaderWriter* channel;

    /// Data received over HDLC with this address will be sent to ``channel``.
    uint64_t receive_address;

    /// Data read from ``channel`` will be sent out over HDLC with this
    /// address.
    uint64_t send_address;
  };

  /// Returns a pointer to the ``ChannelData`` corresponding to the provided
  /// ``receive_address`` or nullptr if no such entry is found.
  ChannelData* FindChannelForReceiveAddress(uint64_t receive_address);

  /// Decodes and writes buffers from ``io_channel_`` and writes them into
  /// the corresponding channel.
  void DecodeAndWriteIncoming(pw::async2::Context& cx);

  /// Attempts to send the decoded ``frame`` contents to the corresponding
  /// channel.
  pw::async2::Poll<> PollDeliverIncomingFrame(pw::async2::Context& cx,
                                              const Frame& frame);

  /// Searches channels for a ``buffer_to_encode_and_send_`` if there is none.
  void TryFillBufferToEncodeAndSend(pw::async2::Context& cx);

  /// Reads from ``channel_datas_``, HDLC encodes the packets, and sends them
  /// out over ``io_channel_``.
  void WriteOutgoingMessages(pw::async2::Context& cx);

  /// Removes any entries in ``channel_datas_`` that have closed.
  ///
  /// Consolidating this into one operation allows for a minimal amount of
  /// shifting of the various channel elements.
  void RemoveClosedChannels();

  //////////////////////////////////////////////////////////
  /// Channels used for both incoming and outgoing data. ///
  //////////////////////////////////////////////////////////

  /// The underlying channel over which HDLC-encoded messages are sent and
  /// received. This is frequently a low-level driver e.g. UART.
  pw::channel::ByteReaderWriter& io_channel_;

  /// The channels which send and receive unencoded data.
  pw::Vector<ChannelData, kSomeNumberOfChannels> channel_datas_;

  ///////////////////////////////////////////////////////////
  /// State associated with the incoming data being read. ///
  ///////////////////////////////////////////////////////////

  /// Incoming data that has not yet been processed by ``decoder_``.
  pw::multibuf::MultiBuf incoming_data_;

  /// An HDLC decoder.
  pw::hdlc::Decoder decoder_;

  /// The most recent frame returned by ``decoder_``.
  std::optional<pw::hdlc::Frame> decoded_frame_;

  ///////////////////////////////////////////////////////////
  /// State associated with the outgoing data being sent. ///
  ///////////////////////////////////////////////////////////

  struct OutgoingBuffer {
    pw::multibuf::MultiBuf buffer;
    size_t hdlc_encoded_size;
    uint64_t target_address;
  };
  /// The last buffer read from one of ``channel_datas_`` but not yet encoded
  /// and sent to ``io_channel_`.
  std::optional<OutgoingBuffer> buffer_to_encode_and_send_;

  /// The next index of ``channel_datas_`` to read an outgoing packet from.
  ///
  /// This is used to provide fairness between the channel outputs.
  size_t next_first_read_index_ = 0;
};

}  // namespace pw::hdlc
