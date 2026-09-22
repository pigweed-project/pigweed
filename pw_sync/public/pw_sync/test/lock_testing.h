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

/// This file provides types that meet C++'s lock-related named requirements,
/// but do no actual locking. They are only intended for use in tests.

#include "pw_chrono/system_clock.h"
#include "pw_sync/virtual_basic_lockable.h"

namespace pw::sync::test {

/// Fake lock that meet's C++'s \em BasicLockable named requirement.
class FakeBasicLockable : public VirtualBasicLockable {
 public:
  virtual ~FakeBasicLockable() = default;

  bool locked() const { return locked_; }

 protected:
  bool locked_ = false;

 private:
  void DoLockOperation(Operation operation) override;
};

/// Fake lock that meet's C++'s \em Lockable named requirement.
class FakeLockable : public FakeBasicLockable {
 public:
  bool try_lock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true);
};

/// Fake clock that merely provides the expected dependent types.
struct FakeClock {
  using duration = chrono::SystemClock::duration;
  using time_point = chrono::SystemClock::time_point;
};

/// Fake clock that provides invalid dependent types.
///
/// This clock is guaranteed to fail `is_lockable_until<Lock, NoClock>` for any
/// `Lock`.
struct NotAClock {
  using duration = void;
  using time_point = void;
};

/// Fake lock that meet's C++'s \em TimedLockable named requirement.
class FakeTimedLockable : public FakeLockable {
 public:
  bool try_lock_for(const FakeClock::duration&) { return try_lock(); }
  bool try_lock_until(const FakeClock::time_point&) { return try_lock(); }
};

}  // namespace pw::sync::test
