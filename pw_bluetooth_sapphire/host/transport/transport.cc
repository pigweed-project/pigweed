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

#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"

namespace bt::hci {

using FeaturesBits = pw::bluetooth::Controller::FeaturesBits;

Transport::Transport(std::unique_ptr<pw::bluetooth::Controller> controller,
                     pw::async::Dispatcher& dispatcher,
                     pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
                     pw::chrono::SystemClock::duration command_timeout)
    : WeakSelf(this),
      dispatcher_(dispatcher),
      controller_(std::move(controller)),
      wake_lease_provider_(wake_lease_provider),
      command_timeout_(command_timeout) {
  PW_CHECK(controller_);
}

Transport::~Transport() { bt_log(INFO, "hci", "Transport shutting down"); }

void Transport::Initialize(
    fit::callback<void(bool /*success*/)> complete_callback) {
  PW_CHECK(!command_channel_);

  bt_log(DEBUG, "hci", "initializing Transport");
  auto self = GetWeakPtr();
  auto complete_cb_wrapper = [self, complete_cb = std::move(complete_callback)](
                                 pw::Status status) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (!status.ok()) {
      complete_cb(/*success=*/false);
      return;
    }

    self->command_channel_ =
        std::make_unique<CommandChannel>(self->controller_.get(),
                                         self->dispatcher_,
                                         self->wake_lease_provider_,
                                         self->command_timeout_);
    self->command_channel_->set_channel_timeout_cb([self] {
      if (self.is_alive()) {
        self->OnChannelError();
      }
    });

    self->controller_->GetFeatures(
        [self, cb = std::move(complete_cb)](FeaturesBits features) mutable {
          if (!self.is_alive()) {
            return;
          }
          self->features_ = features;

          bt_log(INFO, "hci", "Transport initialized");
          cb(/*success=*/true);
        });
  };

  auto error_cb = [self](pw::Status) {
    if (self.is_alive()) {
      self->OnChannelError();
    }
  };

  controller_->Initialize(std::move(complete_cb_wrapper), std::move(error_cb));
}

bool Transport::InitializeACLDataChannel(
    const DataBufferInfo& bredr_buffer_info,
    const DataBufferInfo& le_buffer_info) {
  acl_data_channel_ = AclDataChannel::Create(this,
                                             controller_.get(),
                                             dispatcher_,
                                             bredr_buffer_info,
                                             le_buffer_info,
                                             wake_lease_provider_);

  if (hci_node_) {
    acl_data_channel_->AttachInspect(hci_node_,
                                     AclDataChannel::kInspectNodeName);
  }

  return true;
}

bool Transport::InitializeScoDataChannel(const DataBufferInfo& buffer_info) {
  if (!buffer_info.IsAvailable()) {
    bt_log(WARN,
           "hci",
           "failed to initialize SCO data channel: buffer info is not "
           "available");
    return false;
  }

  if (static_cast<uint32_t>(*features_ & FeaturesBits::kHciSco) == 0) {
    bt_log(WARN, "hci", "HCI SCO not supported");
    return false;
  }

  sco_data_channel_ = ScoDataChannel::Create(
      buffer_info, command_channel_.get(), controller_.get(), dispatcher_);
  return true;
}

bool Transport::InitializeIsoDataChannel(const DataBufferInfo& buffer_info) {
  if (!buffer_info.IsAvailable() ||
      buffer_info.max_data_length() < hci_spec::kMinIsoDataPacketLength) {
    return false;
  }

  iso_data_channel_ = IsoDataChannel::Create(buffer_info,
                                             command_channel_.get(),
                                             controller_.get(),
                                             wake_lease_provider_);
  return true;
}

FeaturesBits Transport::GetFeatures() {
  PW_CHECK(features_);
  return features_.value();
}

void Transport::SetTransportErrorCallback(fit::closure callback) {
  PW_CHECK(callback);
  PW_CHECK(!error_cb_);
  error_cb_ = std::move(callback);
}

void Transport::OnChannelError() {
  bt_log(ERROR, "hci", "channel error, calling Transport error callback");
  // The channels should not be shut down yet. That should be left to higher
  // layers so dependent objects can be destroyed first.
  if (error_cb_) {
    error_cb_();
  }
}

void Transport::AttachInspect(inspect::Node& parent, const std::string& name) {
  PW_CHECK(acl_data_channel_);
  hci_node_ = parent.CreateChild(name);

  if (command_channel_) {
    command_channel_->AttachInspect(hci_node_);
  }

  if (acl_data_channel_) {
    acl_data_channel_->AttachInspect(hci_node_,
                                     AclDataChannel::kInspectNodeName);
  }
}

}  // namespace bt::hci
