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
  WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

  // Impl-specific.
  Fsm fsm_{State::kNotCreated};

  // Keep last, must be destroyed before any other member.
  WeakSelf<IsoGroupImpl> weak_self_;
};

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
