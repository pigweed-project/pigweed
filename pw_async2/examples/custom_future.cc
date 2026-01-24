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

#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future.h"
#include "pw_digital_io/digital_io.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_unit_test/framework.h"

namespace {

// DOCSTAG: [pw_async2-examples-custom-future]
class ButtonReceiver;

class ButtonFuture {
 public:
  // Provide a descriptive reason which can be used to debug blocked tasks.
  static constexpr const char kWaitReason[] = "Waiting for button press";

  // FutureCore is movable and handles list management automatically.
  ButtonFuture(ButtonFuture&&) = default;
  ButtonFuture& operator=(ButtonFuture&&) = default;

  // Polls the future to see if the button has been pressed.
  pw::async2::Poll<> Pend(pw::async2::Context& cx) {
    return core_.DoPend(*this, cx);
  }

 private:
  friend class ButtonReceiver;
  friend class pw::async2::FutureCore;

  // Private constructor used by ButtonReceiver.
  explicit ButtonFuture() : core_(pw::async2::FutureState::kPending) {}

  // Callback invoked by FutureCore::DoPend.
  pw::async2::Poll<> DoPend(pw::async2::Context&) {
    if (core_.is_ready()) {
      return pw::async2::Ready();
    }
    return pw::async2::Pending();
  }

  pw::async2::FutureCore core_;
};

class ButtonReceiver {
 public:
  explicit ButtonReceiver(pw::digital_io::DigitalInterrupt& line)
      : line_(line) {
    PW_CHECK_OK(line_.SetInterruptHandler(
        pw::digital_io::InterruptTrigger::kActivatingEdge,
        [this](pw::digital_io::State) { HandleInterrupt(); }));
    PW_CHECK_OK(line_.EnableInterruptHandler());
  }

  // Returns a future that completes when the button is pressed.
  ButtonFuture WaitForPress() {
    std::lock_guard lock(lock_);
    ButtonFuture future;
    // Only allow one waiter at a time.
    list_.PushRequireEmpty(future);
    return future;
  }

 private:
  // Executed in interrupt context.
  void HandleInterrupt() {
    std::lock_guard lock(lock_);
    list_.ResolveAll();
  }

  pw::digital_io::DigitalInterrupt& line_;
  pw::sync::InterruptSpinLock lock_;
  pw::async2::FutureList<&ButtonFuture::core_> list_ PW_GUARDED_BY(lock_);
};
// DOCSTAG: [pw_async2-examples-custom-future]

// DOCSTAG: [pw_async2-examples-future-list]
class MyFuture {
 public:
  // Future API: value_type, Pend(), is_completed()

 private:
  friend class MyFutureProvider;

  void Resolve(int) { /* ... */ }

  // The FutureCore is a member of the future.
  pw::async2::FutureCore core_;
};

class MyFutureProvider {
 public:
  MyFuture Get() {
    MyFuture future;
    std::lock_guard lock(lock_);
    futures_.Push(future);
    return future;
  }

  void ResolveOne() {
    std::lock_guard lock(lock_);
    // Pop the future from the list as a MyFuture&.
    futures_.Pop().Resolve(123);
  }

 private:
  pw::sync::InterruptSpinLock lock_;

  // FutureList is declared with a pointer to the future type's FutureCore.
  pw::async2::FutureList<&MyFuture::core_> futures_ PW_GUARDED_BY(lock_);
};
// DOCSTAG: [pw_async2-examples-future-list]

class DigitalInterruptMock : public pw::digital_io::DigitalInterrupt {
 public:
  DigitalInterruptMock() = default;

  void Trigger() {
    if (handler_) {
      handler_(pw::digital_io::State::kActive);
    }
  }

 private:
  pw::Status DoEnable(bool) override { return pw::OkStatus(); }

  pw::Status DoSetInterruptHandler(
      pw::digital_io::InterruptTrigger,
      pw::digital_io::InterruptHandler&& handler) override {
    handler_ = std::move(handler);
    return pw::OkStatus();
  }

  pw::Status DoEnableInterruptHandler(bool) override { return pw::OkStatus(); }

  pw::digital_io::InterruptHandler handler_;
};

TEST(CustomFuture, CompilesAndRuns) {
  pw::async2::BasicDispatcher dispatcher;
  DigitalInterruptMock line;

  ButtonReceiver receiver(line);
  ButtonFuture future = receiver.WaitForPress();

  pw::async2::FuncTask function_task([&future](pw::async2::Context& cx) {
    return future.Pend(cx).Readiness();
  });

  dispatcher.Post(function_task);

  dispatcher.RunUntilStalled();

  // Simulate button press.
  line.Trigger();

  dispatcher.RunToCompletion();
}

TEST(FutureList, Compiles) {
  MyFutureProvider provider;
  MyFuture future = provider.Get();
  provider.ResolveOne();
}

}  // namespace
