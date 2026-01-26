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

#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"

#include <lib/fit/function.h>
#include <pw_log/log.h>

namespace bt::l2cap {
namespace internal {
namespace {
using pw::bluetooth::emboss::AclConnectionMode;

const char* AclModeString(pw::bluetooth::emboss::AclConnectionMode mode) {
  switch (mode) {
    case pw::bluetooth::emboss::AclConnectionMode::ACTIVE:
      return "ACTIVE";
    case pw::bluetooth::emboss::AclConnectionMode::HOLD:
      return "HOLD";
    case pw::bluetooth::emboss::AclConnectionMode::SNIFF:
      return "SNIFF";
    default:
      return "RESERVED";
  }
}

const char* kInspectSuppressionPrefix = "suppress_";
const char* kInspectCurrentModePropertyName = "sniff_mode";

}  // namespace

Autosniff::Autosniff(SniffModeParams params,
                     hci::CommandChannel* channel,
                     hci_spec::ConnectionHandle handle,
                     pw::async::Dispatcher* dispatcher,
                     pw::chrono::SystemClock::duration idle_timeout)
    : params_(params),
      autosniff_timeout_(*dispatcher, idle_timeout),
      cmd_channel_(channel),
      handle_(handle),
      weak_self_(this) {
  bt_log(INFO, "autosniff", "Initializing autosniff timer");
  autosniff_timeout_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          return OnTimeout();
        } else {
          return RecurringDisposition::kRecur;
        }
      });

  auto event = cmd_channel_->AddOwnedEventHandler(
      hci_spec::kModeChangeEventCode,
      fit::bind_member<&Autosniff::OnModeChange>(this));
  PW_CHECK(event.has_value());
  mode_change_event_ = std::move(event.value());
  ResetTimeout();
}

void Autosniff::MarkPacketRx() { ResetTimeout(); }
void Autosniff::MarkPacketTx() { ResetTimeout(); }

auto Autosniff::ChangeModesCallback(Autosniff::WeakPtr self_ptr,
                                    AclConnectionMode new_mode) {
  return [self = std::move(self_ptr), new_mode](auto,
                                                const hci::EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }
    if (bt_is_error(event.ToResult(),
                    DEBUG,
                    "autosniff",
                    "Failed to enter mode (%s): (handle %#x)",
                    AclModeString(new_mode),
                    self->handle_)) {
      self->mode_transition_ = false;
    } else {
      bt_log(DEBUG,
             "autosniff",
             "%s Mode accepted by controller: (handle %#x)",
             AclModeString(new_mode),
             self->handle_);
    }
  };
}

void Autosniff::ResetTimeout() {
  if (suppression_count_ == 0) {
    autosniff_timeout_.ResetTimeout();
  }
  if (connection_mode_ == AclConnectionMode::ACTIVE || mode_transition_) {
    PW_LOG_EVERY_N(
        PW_LOG_LEVEL_DEBUG,
        300,
        "Autosniff timer reset, but we are not in sniff mode - ignoring...");
    return;
  }
  bt_log(DEBUG, "autosniff", "Connection active, resetting autosniff timer");

  auto exit_sniff_mode_cmd = hci::CommandPacket::New<
      pw::bluetooth::emboss::ExitSniffModeCommandWriter>(
      hci_spec::kExitSniffMode);
  auto view = exit_sniff_mode_cmd.view_t();
  view.connection_handle().Write(handle_);
  mode_transition_ = true;
  cmd_channel_
      ->SendCommand(
          std::move(exit_sniff_mode_cmd),
          ChangeModesCallback(GetWeakPtr(), AclConnectionMode::ACTIVE))
      .IgnoreError();
}

hci::CommandChannel::EventCallbackResult Autosniff::OnModeChange(
    const hci::EventPacket& event) {
  const auto view = event.view<pw::bluetooth::emboss::ModeChangeEventView>();
  const hci_spec::ConnectionHandle handle = view.connection_handle().Read();
  if (handle != this->handle_) {
    // We are not handling this event.
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  if (bt_is_error(event.ToResult(),
                  WARN,
                  "autosniff",
                  "Mode Change FAILED (handle %#x)",
                  handle_)) {
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  const AclConnectionMode new_mode = view.current_mode().Read();
  mode_transition_ = false;
  connection_mode_ = new_mode;
  inspect_current_mode_.Set(AclModeString(connection_mode_));
  if (connection_mode_ != AclConnectionMode::SNIFF) {
    bt_log(DEBUG,
           "autosniff",
           "Connection is active. Reenabling autosniff timeout (handle %#x)",
           handle_);
    autosniff_timeout_.Reenable();
  }
  bt_log(DEBUG,
         "autosniff",
         "Mode Change SUCCESS. New mode: %s (handle %#x)",
         AclModeString(new_mode),
         handle);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

RecurringDisposition Autosniff::OnTimeout() {
  if (suppression_count_ > 0) {
    bt_log(INFO,
           "autosniff",
           "Autosniff defang based on %hhu suppressions",
           suppression_count_);
    return RecurringDisposition::kFinish;
  }
  if (mode_transition_) {
    bt_log(DEBUG,
           "autosniff",
           "Connection is currently transitioning to or from sniff mode "
           "(handle %#x). Ignoring...",
           handle_);
    return RecurringDisposition::kRecur;
  }
  if (connection_mode_ == AclConnectionMode::SNIFF) {
    bt_log(DEBUG,
           "autosniff",
           "Autosniff timer expired, but we're already in sniff mode - "
           "ignoring...");
    // We are already in sniff mode, let's cancel the timer to save some
    // cycles.
    return RecurringDisposition::kFinish;
  }
  bt_log(DEBUG, "autosniff", "Entering sniff mode (handle %#x)", handle_);

  auto sniff_mode_cmd =
      hci::CommandPacket::New<pw::bluetooth::emboss::SniffModeCommandWriter>(
          hci_spec::kSniffMode);
  auto view = sniff_mode_cmd.view_t();
  view.connection_handle().Write(handle_);
  view.sniff_max_interval().Write(params_.max_interval);
  view.sniff_min_interval().Write(params_.min_interval);
  view.sniff_attempt().Write(params_.sniff_attempt);
  view.sniff_timeout().Write(params_.sniff_timeout);
  mode_transition_ = true;
  cmd_channel_
      ->SendCommand(std::move(sniff_mode_cmd),
                    ChangeModesCallback(GetWeakPtr(), AclConnectionMode::SNIFF))
      .IgnoreError();

  // Note, we don't disarm here, we disarm once we come back here and know for a
  // fact we are in sniff mode as the HCI command may have failed.
  return RecurringDisposition::kRecur;
}

std::unique_ptr<AutosniffSuppressInterest> Autosniff::Suppress(
    const char* reason) {
  if (suppression_count_ == 0xFF) {
    return nullptr;
  }
  suppression_count_ += 1;
  ResetTimeout();
  // Cannot use make_unique here since AutosniffSuppressInterest has a private
  // constructor.
  std::unique_ptr<AutosniffSuppressInterest> interest(
      new AutosniffSuppressInterest(weak_self_.GetWeakPtr(), reason));
  interest->AttachInspect(inspect_node_,
                          inspect_node_.UniqueName(kInspectSuppressionPrefix));
  return interest;
}

void Autosniff::AttachInspect(inspect::Node& parent, std::string name) {
  if (!parent) {
    return;
  }
  inspect_node_ = parent.CreateChild(name);
  inspect_current_mode_ = inspect_node_.CreateString(
      kInspectCurrentModePropertyName, AclModeString(connection_mode_));
}

void Autosniff::RemoveSuppression() {
  PW_DCHECK(suppression_count_ > 0,
            "Trying to remove a suppression but we have zero");
  suppression_count_ -= 1;
  if (suppression_count_ == 0) {
    bt_log(INFO,
           "autosniff",
           "No more suppression interest, re-enabling autosniff (handle %#x)",
           handle_);
    autosniff_timeout_.Reenable();
  }
}

}  // namespace internal
AutosniffSuppressInterest::AutosniffSuppressInterest(
    internal::Autosniff::WeakPtr autosniff, const char* reason)
    : reason_(reason), autosniff_(std::move(autosniff)) {}

void AutosniffSuppressInterest::AttachInspect(inspect::Node& parent,
                                              std::string name) {
  if (!parent) {
    return;
  }
  inspect_reason_ = parent.CreateString(name, reason_);
}

void AutosniffSuppressInterest::Release() {
  if (autosniff_.is_alive()) {
    autosniff_->RemoveSuppression();
    autosniff_.reset();
  }
}

AutosniffSuppressInterest::~AutosniffSuppressInterest() { Release(); }

}  // namespace bt::l2cap
