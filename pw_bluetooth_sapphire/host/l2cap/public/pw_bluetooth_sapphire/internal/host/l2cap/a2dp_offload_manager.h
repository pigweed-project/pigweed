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
#include <pw_bluetooth/hci_android.emb.h>

#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::l2cap {

namespace android_emb = pw::bluetooth::vendor::android_hci;

// Provides an API surface to start and stop A2DP offloading. A2dpOffloadManager
// tracks the state of A2DP offloading and allows at most one channel to be
// offloaded at a given time
class A2dpOffloadManager {
 public:
  // Configuration received from the profile server that needs to be converted
  // to a command packet in order to send the StartA2dpOffload command
  struct Configuration {
    android_emb::A2dpCodecType codec;
    uint16_t max_latency;
    StaticPacket<android_emb::A2dpScmsTEnableWriter> scms_t_enable;
    android_emb::A2dpSamplingFrequency sampling_frequency;
    android_emb::A2dpBitsPerSample bits_per_sample;
    android_emb::A2dpChannelMode channel_mode;
    uint32_t encoded_audio_bit_rate;

    StaticPacket<android_emb::SbcCodecInformationWriter> sbc_configuration;
    StaticPacket<android_emb::AacCodecInformationWriter> aac_configuration;
    StaticPacket<android_emb::LdacCodecInformationWriter> ldac_configuration;
    StaticPacket<android_emb::AptxCodecInformationWriter> aptx_configuration;
  };

  explicit A2dpOffloadManager(hci::CommandChannel::WeakPtr cmd_channel)
      : cmd_channel_(std::move(cmd_channel)) {}

  using SniffSuppressCallback =
      fit::function<std::unique_ptr<AutosniffSuppressInterest>(
          const char* reason)>;

  void SetSniffSuppress(SniffSuppressCallback cb) {
    sniff_suppress_cb_ = std::move(cb);
  }

  // Request the start of A2DP source offloading. |callback| will be called with
  // the result of the request. If offloading is already started or still
  // starting/stopping, the request will fail and |kInProgress| error will be
  // reported synchronously.
  void StartA2dpOffload(const Configuration& config,
                        ChannelId local_id,
                        ChannelId remote_id,
                        hci_spec::ConnectionHandle link_handle,
                        uint16_t max_tx_sdu_size,
                        hci::ResultCallback<> callback);

  // Request the stop of A2DP source offloading on a specific channel.
  // |callback| will be called with the result of the request.
  // If offloading was not started or the channel requested is not offloaded,
  // report success. Returns kInProgress error if channel offloading is
  // currently in the process of stopping.
  void RequestStopA2dpOffload(ChannelId local_id,
                              hci_spec::ConnectionHandle link_handle,
                              hci::ResultCallback<> callback);

  // Returns true if channel with |id| and |link_handle| is starting/has started
  // A2DP offloading
  bool IsChannelOffloaded(ChannelId id,
                          hci_spec::ConnectionHandle link_handle) const;

  WeakPtr<A2dpOffloadManager> GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  hci::CommandChannel::WeakPtr cmd_channel_;

  A2dpOffloadStatus a2dp_offload_status_ = A2dpOffloadStatus::kStopped;

  // Identifier for offloaded channel's endpoint on this device
  std::optional<ChannelId> offloaded_channel_id_;

  // Connection handle of the offloaded channel's underlying logical link
  std::optional<hci_spec::ConnectionHandle> offloaded_link_handle_;

  // Contains a callback if stop command was requested before offload status was
  // |kStarted|
  std::optional<hci::ResultCallback<>> pending_stop_a2dp_offload_request_;

  // Pointer to the logical link we are responsible for A2DP offload of. Set via
  // the LogicalLink using ResisterLink
  SniffSuppressCallback sniff_suppress_cb_;

  // Pointer to the autosniff suppression interest.  This prevents entering
  // sniff mode while offload is active.
  std::unique_ptr<AutosniffSuppressInterest> autosniff_suppress_;

  WeakSelf<A2dpOffloadManager> weak_self_{this};
};

}  // namespace bt::l2cap
