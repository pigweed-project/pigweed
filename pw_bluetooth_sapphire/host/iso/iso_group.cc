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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"

#include <pw_assert/check.h>

#include "pw_bluetooth/hci_commands.emb.h"

namespace bt::iso {
namespace {
constexpr uint8_t kCisRetransmissionCount = 2;

pw::bluetooth::emboss::LECISPacking ConvertPacking(CigPacking packing) {
  switch (packing) {
    case CigPacking::kSequential:
      return pw::bluetooth::emboss::LECISPacking::SEQUENTIAL;
    case CigPacking::kInterleaved:
      return pw::bluetooth::emboss::LECISPacking::INTERLEAVED;
  }
  PW_CRASH("Unhandled packing value: %#02x", static_cast<uint8_t>(packing));
}

pw::bluetooth::emboss::LECISFraming ConvertFraming(CigFraming framing) {
  switch (framing) {
    case CigFraming::kUnframed:
      return pw::bluetooth::emboss::LECISFraming::UNFRAMED;
    case CigFraming::kFramed:
      return pw::bluetooth::emboss::LECISFraming::FRAMED;
  }
  PW_CRASH("Unhandled framing value: %#02x", static_cast<uint8_t>(framing));
}

template <typename... WriterTraits>
bool AllowAllPhys(
    typename pw::bluetooth::emboss::GenericLECISPHYOptionsView<WriterTraits...>
        phys) {
  return phys.le_1m().TryToWrite(true) && phys.le_2m().TryToWrite(true) &&
         phys.le_coded().TryToWrite(true);
}

class IsoGroupImpl final : public IsoGroup {
 public:
  IsoGroupImpl(hci_spec::CigIdentifier id,
               hci::Transport::WeakPtr hci,
               CigStreamCreator::WeakPtr cig_stream_creator,
               OnClosedCallback on_closed_callback)
      : IsoGroup(id,
                 std::move(hci),
                 std::move(cig_stream_creator),
                 std::move(on_closed_callback)),
        weak_self_(this) {}

 private:
  // A finite state machine that enforces only valid transitions by preventing
  // any value updates besides those defined in the spec.
  //
  // See BT Core spec v6.0 | Vol 6, Part B, Sec 4.5.14.3 - States of a CIG.
  class Fsm {
   public:
    // Enum-like state tag.
    class State {
     public:
      // States must be independent bit shifts, as `IsOneOf` is a bitwise-or of
      // all candidate states.
      enum /* !class */ Value : uint8_t {
        kNotCreated = 1 << 0,
        kConfigurable = 1 << 1,
        kActive = 1 << 2,
        kInactive = 1 << 3,
      };

      constexpr /* !explicit */ State(Value value) : value_(value) {}
      constexpr State(const State&) = default;
      constexpr State(State&&) = default;
      constexpr State& operator=(const State&) = default;
      constexpr State& operator=(State&&) = default;

      [[nodiscard]] constexpr bool operator==(const State& other) const {
        return value() == other.value();
      }
      [[nodiscard]] constexpr bool operator!=(const State& other) const {
        return !(*this == other);
      }

      [[nodiscard]] constexpr Value value() const { return value_; }

      // Determine if the current state is any of the provided states.
      template <typename... Args>
      [[nodiscard]] constexpr bool IsOneOf(Args... values) const {
        return (value() & (0 | ... | State(values).value())) != 0;
      }

      // Produce a string representation of the state.
      constexpr const char* ToString() const {
        switch (value()) {
          case kNotCreated:
            return "Not Created";
          case kConfigurable:
            return "Configurable";
          case kActive:
            return "Active";
          case kInactive:
            return "Inactive";
        }
      }

     private:
      Value value_;
    };

    constexpr Fsm() = default;
    constexpr Fsm(State initial) : current_(initial) {}

    // Assign an invalid state a new state value.
    // Precondition: `!is_valid()`. Thus, one can only move into a state that is
    // invalid, ensuring states can only be updated via `TransitionTo`.
    constexpr Fsm& operator=(Fsm&& other) {
      PW_CHECK(
          !is_valid(), "Attempt to overwrite valid state (%s).", ToString());
      std::swap(current_, other.current_);
      return *this;
    }
    constexpr Fsm& operator=(const Fsm& other) {
      PW_CHECK(
          !is_valid(), "Attempt to overwrite valid state (%s).", ToString());
      current_ = other.current_;
      return *this;
    }

    // Get the current state value.
    // Precondition: `is_valid()`
    [[nodiscard]] constexpr State current() const {
      PW_CHECK(is_valid(), "Invalid state access.");
      return *current_;
    }

    // Transition to a new state.
    // Returns `FAILED_PRECONDITION` if the transition is invalid, or the
    // previous state value if it is successful.
    [[nodiscard]] constexpr pw::Result<State> TransitionTo(State update) {
      if (!IsTransitionValid(update)) {
        return pw::Status::FailedPrecondition();
      }

      auto previous = *current_;
      current_ = update;
      return previous;
    }

    // Transition to a new state when the transition is expected to succeed.
    constexpr void CheckedTransitionTo(State update) {
      PW_CHECK_OK(TransitionTo(update),
                  "Invalid state transition (%s -> %s).",
                  ToString(),
                  update.ToString());
    }

    // Check if transitioning to a new state would be valid.
    [[nodiscard]] constexpr bool IsTransitionValid(State update) const {
      constexpr auto kNotCreated = State::kNotCreated;
      constexpr auto kConfigurable = State::kConfigurable;
      constexpr auto kActive = State::kActive;
      constexpr auto kInactive = State::kInactive;

      if (!is_valid()) {
        return false;
      }

      switch (current().value()) {
        case kNotCreated:
          return update.IsOneOf(kConfigurable);
        case kConfigurable:
          return update.IsOneOf(kNotCreated, kConfigurable, kActive);
        case kActive:
          return update.IsOneOf(kActive, kInactive);
        case kInactive:
          return update.IsOneOf(kActive, kNotCreated);
      }
    }

    [[nodiscard]] constexpr const char* ToString() const {
      if (!is_valid()) {
        return "[Invalid state]";
      }

      return current_->ToString();
    }

    // Explicitly invalidate a state.
    void Invalidate() {
      if (is_valid() && current() != State::kNotCreated) {
        PW_LOG_WARN("Invalidating a state (%s) that is not `kNotCreated`.",
                    ToString());
      }
      current_.reset();
    }

    [[nodiscard]] constexpr bool is_valid() const {
      return current_.has_value();
    }

    [[nodiscard]] constexpr bool CanSetParams() const {
      return is_valid() &&
             current().IsOneOf(State::kNotCreated, State::kConfigurable);
    }

    [[nodiscard]] constexpr bool CanRemoveCig() const {
      return is_valid() &&
             current().IsOneOf(State::kConfigurable, State::kInactive);
    }

    [[nodiscard]] constexpr bool CanCreateCis() const {
      return is_valid() &&
             current().IsOneOf(
                 State::kConfigurable, State::kActive, State::kInactive);
    }

    [[nodiscard]] constexpr bool CanDisconnectCis() const {
      return is_valid() && current().IsOneOf(State::kActive);
    }

   private:
    std::optional<State> current_;
  };

  using State = Fsm::State;

  // IsoGroup overrides.
  void SetParams(CigParams cig_params,
                 std::vector<CigCisParams> cis_params,
                 SetParamsCallback callback) override;
  pw::Status CreateCises(pw::span<CreateCisData> establish_data) override;
  WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

  // Impl-specific.
  void HandleSetCIGParametersCommandCompleteEvent(
      const hci::EventPacket& cmd_complete,
      std::vector<CigCisParams> cis_params,
      SetParamsCallback callback);

  Fsm fsm_{State::kNotCreated};
  std::optional<pw::bluetooth::emboss::LESleepClockAccuracyRange>
      worst_case_sca_;

  // Keep last, must be destroyed before any other member.
  WeakSelf<IsoGroupImpl> weak_self_;
};

void IsoGroupImpl::SetParams(CigParams cig_params,
                             std::vector<CigCisParams> cis_params,
                             IsoGroup::SetParamsCallback callback) {
  if (!fsm_.CanSetParams()) {
    bt_log(ERROR, "iso", "Invalid state (%s) for SetParams.", fsm_.ToString());
    callback(pw::unexpected(HostError::kFailed));
    return;
  }

  std::unordered_set<hci_spec::CisIdentifier> cis_ids;
  for (const auto& cis : cis_params) {
    if (streams_.count(cis.config.cis_id) > 0 ||
        cis_ids.count(cis.config.cis_id) > 0) {
      bt_log(ERROR,
             "iso",
             "Attempt to configure existing or duplicate CIS ID %#02x",
             cis.config.cis_id);
      callback(pw::unexpected(HostError::kInvalidParameters));
      return;
    }
    cis_ids.insert(cis.config.cis_id);
  }

  const size_t packet_size =
      pw::bluetooth::emboss::LESetCIGParametersCommand::MinSizeInBytes() +
      (pw::bluetooth::emboss::LESetCIGParametersCISOptions::MinSizeInBytes() *
       cis_params.size());
  auto cmd_packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LESetCIGParametersCommandWriter>(
      hci_spec::kLESetCIGParameters, packet_size);

  // From Bluetooth Core Spec v6.2, Vol 4, Part E, 7.8.97:
  // If the Host cannot get the sleep clock accuracy from all the Peripherals,
  // it shall set the Worst_Case_SCA parameter to zero.
  worst_case_sca_ = cig_params.worst_case_sca;
  auto sca = worst_case_sca_.value_or(
      static_cast<pw::bluetooth::emboss::LESleepClockAccuracyRange>(0));

  auto cmd_view = cmd_packet.view_t();
  PW_CHECK(cmd_view.IsComplete());
  PW_CHECK(cmd_view.cig_id().TryToWrite(id()));
  PW_CHECK(cmd_view.sdu_interval_c_to_p().TryToWrite(
      cig_params.sdu_interval_c_to_p));
  PW_CHECK(cmd_view.sdu_interval_p_to_c().TryToWrite(
      cig_params.sdu_interval_p_to_c));
  PW_CHECK(cmd_view.worst_case_sca().TryToWrite(sca));
  PW_CHECK(cmd_view.packing().TryToWrite(ConvertPacking(cig_params.packing)));
  PW_CHECK(cmd_view.framing().TryToWrite(ConvertFraming(cig_params.framing)));
  PW_CHECK(cmd_view.max_transport_latency_c_to_p().TryToWrite(
      cig_params.max_transport_latency_c_to_p));
  PW_CHECK(cmd_view.max_transport_latency_p_to_c().TryToWrite(
      cig_params.max_transport_latency_p_to_c));
  PW_CHECK(cmd_view.cis_count().TryToWrite(cis_params.size()));

  PW_CHECK(cmd_view.IsComplete());
  for (size_t i = 0; i < cis_params.size(); ++i) {
    auto cis_options = cmd_view.cis_options()[i];
    PW_CHECK(cis_options.cis_id().TryToWrite(cis_params[i].config.cis_id));
    PW_CHECK(cis_options.max_sdu_c_to_p().TryToWrite(
        cis_params[i].config.max_sdu_c_to_p));
    PW_CHECK(cis_options.max_sdu_p_to_c().TryToWrite(
        cis_params[i].config.max_sdu_p_to_c));
    PW_CHECK(AllowAllPhys(cis_options.phy_c_to_p()));
    PW_CHECK(AllowAllPhys(cis_options.phy_p_to_c()));
    PW_CHECK(cis_options.rtn_c_to_p().TryToWrite(kCisRetransmissionCount));
    PW_CHECK(cis_options.rtn_p_to_c().TryToWrite(kCisRetransmissionCount));
  }

  PW_CHECK(cmd_view.Ok());

  hci_->command_channel()
      ->SendCommand(
          std::move(cmd_packet),
          [self = weak_self_.GetWeakPtr(),
           cis_params_capture = std::move(cis_params),
           callback_capture = std::move(callback)](
              auto, const hci::EventPacket& cmd_complete) mutable {
            if (!self.is_alive()) {
              bt_log(WARN,
                     "iso",
                     "Object destroyed during set CIG parameters command");
              callback_capture(pw::unexpected(HostError::kFailed));
              return;
            }

            self->HandleSetCIGParametersCommandCompleteEvent(
                std::move(cmd_complete),
                std::move(cis_params_capture),
                std::move(callback_capture));
          })
      .IgnoreError();
}

pw::Status IsoGroupImpl::CreateCises(pw::span<CreateCisData> establish_data) {
  if (!fsm_.CanCreateCis()) {
    bt_log(
        ERROR, "iso", "Invalid state (%s). for CreateCises.", fsm_.ToString());
    return pw::Status::FailedPrecondition();
  }

  if (establish_data.empty()) {
    return pw::OkStatus();
  }

  const size_t packet_size =
      pw::bluetooth::emboss::LECreateCISCommand::MinSizeInBytes() +
      (pw::bluetooth::emboss::LECreateCISCommand::ConnectionInfo::
           IntrinsicSizeInBytes() *
       establish_data.size());
  auto cmd_packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::LECreateCISCommandWriter>(
          hci_spec::kLECreateCIS, packet_size);

  // Emboss does not like `requires: 0x01 <= this` for the cis_count when the
  // packet is initialized to all zeroes, even in `IsComplete`. This is because
  // the count is technically invalid.
  // See https://github.com/google/emboss/issues/240
  auto cmd_view =
      cmd_packet
          .unchecked_view<pw::bluetooth::emboss::LECreateCISCommandWriter>();
  cmd_view.cis_count().Write(establish_data.size());
  PW_CHECK(cmd_view.IsComplete());

  for (size_t i = 0; i < establish_data.size(); ++i) {
    auto cis_iter = streams_.find(establish_data[i].cis_id);
    if (cis_iter == streams_.end()) {
      bt_log(WARN,
             "iso",
             "Attempted to establish CIS that is not part of this CIG");
      return pw::Status::NotFound();
    }
    if (!cis_iter->second.is_alive()) {
      bt_log(WARN, "iso", "Attempted to establish CIS that has been destroyed");
      return pw::Status::FailedPrecondition();
    }

    if (establish_data[i].sca.has_value() && worst_case_sca_.has_value()) {
      if (establish_data[i].sca.value() < worst_case_sca_.value()) {
        bt_log(WARN,
               "iso",
               "Attempted to establish CIS to peer with worse SCA than CIG "
               "worst case");
        return pw::Status::OutOfRange();
      }
    }

    cmd_view.cis_connection_info()[i].cis_connection_handle().Write(
        cis_iter->second->cis_handle());
    cmd_view.cis_connection_info()[i].acl_connection_handle().Write(
        establish_data[i].acl_handle);
  }

  PW_CHECK(cmd_view.Ok());

  // Note: The callback is nullptr because the command status does not tell us
  // anything about establishing the streams, instead those streams will handle
  // their own established event and call their `OnEstablishedCallback`s.
  hci_->command_channel()
      ->SendCommand(
          std::move(cmd_packet), nullptr, hci_spec::kCommandStatusEventCode)
      .IgnoreError();
  return pw::OkStatus();
}

void IsoGroupImpl::HandleSetCIGParametersCommandCompleteEvent(
    const hci::EventPacket& cmd_complete,
    std::vector<CigCisParams> cis_params,
    IsoGroup::SetParamsCallback callback) {
  auto result = cmd_complete.unchecked_view<
      pw::bluetooth::emboss::LESetCIGParametersCommandCompleteEventView>();
  if (!result.Ok()) {
    bt_log(ERROR, "iso", "Invalid set CIG parameters return");
    callback(pw::unexpected(HostError::kPacketMalformed));
    return;
  }

  if (pw::bluetooth::emboss::StatusCode status = result.status().Read();
      status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    bt_log(WARN,
           "iso",
           "Failed set CIG parameters command (%#02x)",
           static_cast<uint8_t>(status));
    callback(pw::unexpected(HostError::kFailed));
    return;
  }

  if (!fsm_.TransitionTo(State::kConfigurable).ok()) {
    bt_log(ERROR,
           "iso",
           "Could not transition to 'Configurable' from '%s'.",
           fsm_.ToString());
    callback(pw::unexpected(HostError::kFailed));
    return;
  }

  if (result.cig_id().Read() != id()) {
    bt_log(ERROR,
           "iso",
           "Incorrect CIG ID in response (expected %#02x, got %#02x)",
           id(),
           result.cig_id().Read());
    callback(pw::unexpected(HostError::kFailed));
    return;
  }

  if (result.cis_count().Read() != cis_params.size()) {
    bt_log(ERROR,
           "iso",
           "Incorrect CIS count in response (expected %#02zx, got %#02x)",
           cis_params.size(),
           result.cis_count().Read());
    callback(pw::unexpected(HostError::kFailed));
    return;
  }

  for (size_t i = 0; i < result.cis_count().Read(); ++i) {
    hci_spec::CisIdentifier cis_id = cis_params[i].config.cis_id;
    auto cis_closed_callback = [self = weak_self_.GetWeakPtr(), cis_id] {
      if (!self.is_alive()) {
        return;
      }
      self->streams_.erase(cis_id);
    };

    if (!cig_stream_creator_.is_alive()) {
      bt_log(ERROR, "iso", "Dependency destroyed while setting up CIG");
      callback(pw::unexpected(HostError::kFailed));
      return;
    }

    auto [_, success] =
        streams_.emplace(cis_id,
                         cig_stream_creator_->CreateCisConfiguration(
                             {id(), cis_id},
                             result.connection_handles()[i].Read(),
                             std::move(cis_params[i].on_established_cb),
                             cis_closed_callback));
    if (!success) {
      bt_log(ERROR,
             "iso",
             "Failed to emplace stream with CIS ID %#02x (already exists)",
             cis_id);
      callback(pw::unexpected(HostError::kInvalidParameters));
      return;
    }
  }

  callback({});
}

}  // namespace

std::unique_ptr<IsoGroup> IsoGroup::CreateCig(
    hci_spec::CigIdentifier id,
    hci::Transport::WeakPtr hci,
    CigStreamCreator::WeakPtr cig_stream_creator,
    IsoGroup::OnClosedCallback on_closed_callback) {
  return std::make_unique<IsoGroupImpl>(id,
                                        std::move(hci),
                                        std::move(cig_stream_creator),
                                        std::move(on_closed_callback));
}

}  // namespace bt::iso
