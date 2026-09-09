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

#include <pw_assert/check.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream.h"

namespace bt::iso::testing {

// Testing replacement for IsoStream with functionality built up as needed.
class FakeIsoStream : public IsoStream {
 public:
  FakeIsoStream(hci_spec::ConnectionHandle cis_handle = 0,
                CisEstablishedCallback cis_established_callback = nullptr,
                pw::Callback<void()> on_closed_callback = nullptr)
      : cis_handle_(cis_handle),
        cis_established_callback_(std::move(cis_established_callback)),
        on_closed_callback_(std::move(on_closed_callback)),
        weak_self_(this) {}

  // IsoStream overrides
  bool OnCisEstablished(const hci::EventPacket& /*event*/) override {
    TriggerEstablishedCallback();
    return is_established_;
  }

  void SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection /*direction*/,
      const bt::StaticPacket<
          pw::bluetooth::emboss::CodecIdWriter>& /*codec_id*/,
      const std::optional<std::vector<uint8_t>>& /*code_configuration*/,
      uint32_t /*controller_delay_usecs*/,
      SetupDataPathCallback&& on_complete_cb,
      IncomingDataHandler&& on_incoming_data_available_cb) override {
    on_incoming_data_available_cb_ = std::move(on_incoming_data_available_cb);
    on_complete_cb(setup_data_path_status_);
  }

  void ReceiveInboundPacket(
      const pw::span<const std::byte> /*packet*/) override {}

  std::optional<DynamicByteBuffer> GetNextOutboundPdu() override {
    return std::nullopt;
  }

  hci_spec::ConnectionHandle cis_handle() const override { return cis_handle_; }

  void Close() override {
    close_called_ = true;
    if (close_synchronously_) {
      is_established_ = false;
      if (on_closed_callback_) {
        on_closed_callback_();
      }
    }
  }

  void CompleteClose() {
    PW_CHECK(!close_synchronously_);
    is_established_ = false;
    if (on_closed_callback_) {
      on_closed_callback_();
    }
  }

  bool close_called() const { return close_called_; }
  void set_close_synchronously(bool sync) { close_synchronously_ = sync; }

  std::optional<IsoDataPacket> ReadNextQueuedIncomingPacket() override {
    if (incoming_packet_queue_.size() < 1) {
      return std::nullopt;
    }
    IsoDataPacket next_frame = std::move(incoming_packet_queue_.front());
    incoming_packet_queue_.pop();
    return next_frame;
  }

  void Send(pw::ConstByteSpan data) override {
    sent_data_queue_.emplace(data.begin(), data.end());
  }

  std::queue<std::vector<std::byte>>& GetSentDataQueue() {
    return sent_data_queue_;
  }

  IsoStream::WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

  // Testing functionality
  void TriggerEstablishedCallback(
      const std::optional<CisEstablishedParameters>& parameters,
      pw::bluetooth::emboss::StatusCode status_code =
          pw::bluetooth::emboss::StatusCode::SUCCESS) {
    if (cis_established_callback_) {
      cis_established_callback_(status_code, GetWeakPtr(), parameters);
    }
    is_established_ = true;
  }
  void TriggerEstablishedCallback(
      pw::bluetooth::emboss::StatusCode status_code =
          pw::bluetooth::emboss::StatusCode::SUCCESS) {
    std::optional<CisEstablishedParameters> parameters = std::nullopt;
    TriggerEstablishedCallback(parameters, status_code);
  }

  void SetSetupDataPathReturnStatus(IsoStream::SetupDataPathError status) {
    setup_data_path_status_ = status;
  }

  void QueueIncomingFrame(IsoDataPacket frame) {
    incoming_packet_queue_.push(std::move(frame));
  }

  size_t incoming_packet_requests() { return incoming_packet_requests_; }

  bool NotifyClientOfPacketReceived(pw::span<const std::byte> packet) {
    PW_CHECK(on_incoming_data_available_cb_.has_value());
    return (*on_incoming_data_available_cb_)(packet);
  }

  bool is_established() const override { return is_established_; }

 protected:
  IsoStream::SetupDataPathError setup_data_path_status_ =
      IsoStream::SetupDataPathError::kSuccess;

 private:
  hci_spec::ConnectionHandle cis_handle_;
  CisEstablishedCallback cis_established_callback_;
  pw::Callback<void()> on_closed_callback_;
  bool is_established_ = false;
  bool close_synchronously_ = true;
  bool close_called_ = false;

  std::optional<IncomingDataHandler> on_incoming_data_available_cb_;
  std::queue<IsoDataPacket> incoming_packet_queue_;
  size_t incoming_packet_requests_ = 0;
  std::queue<std::vector<std::byte>> sent_data_queue_;

  // Keep last, must be destroyed before any other member.
  WeakSelf<FakeIsoStream> weak_self_;
};

}  // namespace bt::iso::testing
