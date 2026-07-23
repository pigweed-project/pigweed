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

#include "pw_bluetooth_sapphire/internal/host/l2cap/a2dp_offload_manager.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_android.emb.h>
#include <pw_preprocessor/compiler.h>

#include <cstdint>
#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

namespace bt::l2cap {
namespace android_emb = pw::bluetooth::vendor::android_hci;

void A2dpOffloadManager::StartA2dpOffload(
    const Configuration& config,
    ChannelId local_id,
    ChannelId remote_id,
    hci_spec::ConnectionHandle link_handle,
    uint16_t max_tx_sdu_size,
    hci::ResultCallback<> callback) {
  PW_DCHECK(cmd_channel_.is_alive());

  switch (a2dp_offload_status_) {
    case A2dpOffloadStatus::kStarted: {
      bt_log(WARN,
             "l2cap",
             "Only one channel can offload A2DP at a time; already offloaded "
             "(handle: %#.4x, local id: %#.4x",
             *offloaded_link_handle_,
             *offloaded_channel_id_);
      callback(ToResult(HostError::kInProgress));
      return;
    }
    case A2dpOffloadStatus::kStarting: {
      bt_log(WARN,
             "l2cap",
             "A2DP offload is currently starting (status: %hhu)",
             static_cast<unsigned char>(a2dp_offload_status_));
      callback(ToResult(HostError::kInProgress));
      return;
    }
    case A2dpOffloadStatus::kStopping: {
      bt_log(WARN,
             "l2cap",
             "A2DP offload is stopping... wait until stopped before starting "
             "(status: %hhu)",
             static_cast<unsigned char>(a2dp_offload_status_));
      callback(ToResult(HostError::kInProgress));
      return;
    }
    case A2dpOffloadStatus::kStopped:
      break;
  }

  offloaded_link_handle_ = link_handle;
  offloaded_channel_id_ = local_id;
  a2dp_offload_status_ = A2dpOffloadStatus::kStarting;

  constexpr size_t kPacketSize =
      android_emb::StartA2dpOffloadCommand::MaxSizeInBytes();
  auto packet =
      hci::CommandPacket::New<android_emb::StartA2dpOffloadCommandWriter>(
          pw::bluetooth::emboss::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
          kPacketSize);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      static_cast<uint8_t>(android_emb::A2dpOffloadSubOpcode::START_LEGACY));
  view.codec_type().Write(config.codec);
  view.max_latency().Write(config.max_latency);
  view.scms_t_enable().CopyFrom(
      const_cast<Configuration&>(config).scms_t_enable.view());
  view.sampling_frequency().Write(config.sampling_frequency);
  view.bits_per_sample().Write(config.bits_per_sample);
  view.channel_mode().Write(config.channel_mode);
  view.encoded_audio_bitrate().Write(config.encoded_audio_bit_rate);
  view.connection_handle().Write(link_handle);
  view.l2cap_channel_id().Write(remote_id);
  view.l2cap_mtu_size().Write(max_tx_sdu_size);

  // kAptx and kAptxhd codecs not yet handled
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (config.codec) {
    case android_emb::A2dpCodecType::SBC:
      view.sbc_codec_information().CopyFrom(
          const_cast<Configuration&>(config).sbc_configuration.view());
      break;
    case android_emb::A2dpCodecType::AAC:
      view.aac_codec_information().CopyFrom(
          const_cast<Configuration&>(config).aac_configuration.view());
      break;
    case android_emb::A2dpCodecType::LDAC:
      view.ldac_codec_information().CopyFrom(
          const_cast<Configuration&>(config).ldac_configuration.view());
      break;
    case android_emb::A2dpCodecType::APTX:
    case android_emb::A2dpCodecType::APTX_HD:
    default:
      bt_log(ERROR,
             "l2cap",
             "a2dp offload codec type (%hhu) not supported",
             static_cast<uint8_t>(config.codec));
      callback(ToResult(HostError::kNotSupported));
      return;
  }
  PW_MODIFY_DIAGNOSTICS_POP();

  auto iter = sniff_suppress_cbs_.find(link_handle);
  if (iter != sniff_suppress_cbs_.end() && iter->second) {
    autosniff_suppress_ = iter->second("A2DP Offload");
  }

  cmd_channel_
      ->SendCommand(
          std::move(packet),
          [cb = std::move(callback),
           id = local_id,
           handle = link_handle,
           self = weak_self_.GetWeakPtr(),
           this](auto /*transaction_id*/,
                 const hci::EventPacket& event) mutable {
            if (!self.is_alive()) {
              return;
            }

            if (event.ToResult().is_error()) {
              bt_log(WARN,
                     "l2cap",
                     "Start A2DP offload command failed (result: %s, handle: "
                     "%#.4x, local id: %#.4x)",
                     bt_str(event.ToResult()),
                     handle,
                     id);
              a2dp_offload_status_ = A2dpOffloadStatus::kStopped;
            } else {
              bt_log(INFO,
                     "l2cap",
                     "A2DP offload started (handle: %#.4x, local id: %#.4x",
                     handle,
                     id);
              a2dp_offload_status_ = A2dpOffloadStatus::kStarted;
            }
            cb(event.ToResult());

            // If we tried to stop while A2DP was still starting, perform the
            // stop command now
            if (pending_stop_a2dp_offload_request_.has_value()) {
              auto pending_request_callback =
                  std::move(pending_stop_a2dp_offload_request_.value());
              pending_stop_a2dp_offload_request_.reset();

              RequestStopA2dpOffload(
                  id, handle, std::move(pending_request_callback));
            }
          })
      .IgnoreError();
}

void A2dpOffloadManager::RequestStopA2dpOffload(
    ChannelId local_id,
    hci_spec::ConnectionHandle link_handle,
    hci::ResultCallback<> callback) {
  PW_DCHECK(cmd_channel_.is_alive());

  if (!IsChannelOffloaded(local_id, link_handle)) {
    callback(fit::success());
    return;
  }

  switch (a2dp_offload_status_) {
    case A2dpOffloadStatus::kStopped: {
      bt_log(DEBUG,
             "l2cap",
             "No channels are offloading A2DP (status: %hhu)",
             static_cast<unsigned char>(a2dp_offload_status_));
      callback(fit::success());
      return;
    }
    case A2dpOffloadStatus::kStopping: {
      bt_log(INFO,
             "l2cap",
             "A2DP offload stop while stopping (status: %hhu)",
             static_cast<unsigned char>(a2dp_offload_status_));
      callback(ToResult(HostError::kInProgress));
      return;
    }
    case A2dpOffloadStatus::kStarting:
    case A2dpOffloadStatus::kStarted:
      break;
  }

  // Wait until offloading status is |kStarted| before sending stop command
  if (a2dp_offload_status_ == A2dpOffloadStatus::kStarting) {
    pending_stop_a2dp_offload_request_ = std::move(callback);
    return;
  }

  a2dp_offload_status_ = A2dpOffloadStatus::kStopping;

  auto packet =
      hci::CommandPacket::New<android_emb::StopA2dpOffloadCommandWriter>(
          pw::bluetooth::emboss::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD);
  auto packet_view = packet.view_t();

  packet_view.vendor_command().sub_opcode().Write(
      static_cast<uint8_t>(android_emb::A2dpOffloadSubOpcode::STOP_LEGACY));

  cmd_channel_
      ->SendCommand(
          std::move(packet),
          [cb = std::move(callback),
           self = weak_self_.GetWeakPtr(),
           id = local_id,
           handle = link_handle,
           this](auto /*transaction_id*/,
                 const hci::EventPacket& event) mutable {
            if (!self.is_alive()) {
              return;
            }

            if (event.ToResult().is_error()) {
              bt_log(WARN,
                     "l2cap",
                     "Stop A2DP offload command failed (result: %s, handle: "
                     "%#.4x, "
                     "local id: %#.4x)",
                     bt_str(event.ToResult()),
                     handle,
                     id);
            } else {
              bt_log(INFO,
                     "l2cap",
                     "A2DP offload stopped (handle: %#.4x, local id: %#.4x",
                     handle,
                     id);
            }
            cb(event.ToResult());

            a2dp_offload_status_ = A2dpOffloadStatus::kStopped;
            autosniff_suppress_.reset();
          })
      .IgnoreError();
}

bool A2dpOffloadManager::IsChannelOffloaded(
    ChannelId id, hci_spec::ConnectionHandle link_handle) const {
  if (!offloaded_channel_id_.has_value() ||
      !offloaded_link_handle_.has_value()) {
    bt_log(DEBUG,
           "l2cap",
           "Channel is not offloaded (handle: %#.4x, local id: %#.4x) ",
           link_handle,
           id);
    return false;
  }

  // Same channel that requested start A2DP offloading must request stop
  // offloading
  if (id != *offloaded_channel_id_ || link_handle != *offloaded_link_handle_) {
    bt_log(DEBUG,
           "l2cap",
           "Offloaded channel (handle: %#.4x, local id: %#.4x) does not match "
           "(handle: %#.4x, id: %#.4x)",
           *offloaded_link_handle_,
           *offloaded_channel_id_,
           link_handle,
           id);
    return false;
  }

  return id == *offloaded_channel_id_ &&
         link_handle == *offloaded_link_handle_ &&
         (a2dp_offload_status_ == A2dpOffloadStatus::kStarted ||
          a2dp_offload_status_ == A2dpOffloadStatus::kStarting ||
          a2dp_offload_status_ == A2dpOffloadStatus::kStopping);
}

void A2dpOffloadManager::RegisterLink(hci_spec::ConnectionHandle link_handle,
                                      SniffSuppressCallback cb) {
  sniff_suppress_cbs_[link_handle] = std::move(cb);
}

void A2dpOffloadManager::UnregisterLink(
    hci_spec::ConnectionHandle link_handle) {
  sniff_suppress_cbs_.erase(link_handle);
  if (offloaded_link_handle_ == link_handle) {
    autosniff_suppress_.reset();
  }
}

}  // namespace bt::l2cap
