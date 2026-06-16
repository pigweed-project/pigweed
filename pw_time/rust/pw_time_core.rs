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
#![no_std]

//! `pw_time_core` provides clock aware time and duration types.
//!
//! `pw_time_core` contains the basic types for
//!  <a href="../pw_time/index.html"><code>pw_time</code></a> without providing
//! a default system clock.  This allows it to be used by:
//! * [`Clock`] implementations.
//! * systems which do not support a system clock.
//!
//! For more information see <a href="../pw_time/index.html"><code>pw_time</code></a>.
use core::marker::PhantomData;
use core::ops::{Add, Sub};

#[cfg(test)]
#[unsafe(no_mangle)]
unsafe extern "C-unwind" fn pw_assert_HandleFailure() -> ! {
    panic!("pw_assert failed");
}

/// A trait for retrieving the current system or hardware time.
pub trait Clock: Sized {
    /// The number of clock ticks per second.
    const TICKS_PER_SEC: u64;

    /// Returns the current time [`Instant`] according to this clock.
    fn now() -> Instant<Self>;
}

/// A measurement of a monotonically non-decreasing clock.
///
/// An `Instant` is generic over a [`Clock`], preventing arithmetic operations
/// between instants of different clocks at compile-time.
#[derive(Debug)]
pub struct Instant<Clock: crate::Clock> {
    ticks: u64,
    _phantom: PhantomData<Clock>,
}

impl<Clock: crate::Clock> Instant<Clock> {
    /// The maximum possible value for an `Instant`.
    pub const MAX: Self = Self::from_ticks(u64::MAX);
    /// The minimum possible value for an `Instant`.
    pub const MIN: Self = Self::from_ticks(u64::MIN);

    /// Creates a new `Instant` from a raw tick count.
    #[must_use]
    pub const fn from_ticks(ticks: u64) -> Self {
        Self {
            ticks,
            _phantom: PhantomData,
        }
    }

    /// Returns the raw tick count of this `Instant`.
    #[must_use]
    pub const fn ticks(&self) -> u64 {
        self.ticks
    }

    /// Returns the `Instant` resulting from adding `Duration`, or `None` if overflow occurred.
    #[must_use]
    pub const fn checked_add_duration(self, duration: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add_signed(duration.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }

    /// Returns the `Instant` resulting from subtracting `Duration`, or `None` if underflow occurred.
    #[must_use]
    pub const fn checked_sub_duration(self, duration: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add_signed(-duration.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }
}

// Manually implement Copy so that we don't require `Clock` to be Copy
impl<Clock: crate::Clock> Copy for Instant<Clock> {}

// Manually implement Clone so that we don't require `Clock` to be Clone
impl<Clock: crate::Clock> Clone for Instant<Clock> {
    fn clone(&self) -> Self {
        *self
    }
}

// Manually implement Ord so that we don't require `Clock` to be Ord
impl<Clock: crate::Clock> Ord for Instant<Clock> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.ticks.cmp(&other.ticks)
    }
}

// Manually implement PartialOrd so that we don't require `Clock` to be PartialOrd
impl<Clock: crate::Clock> PartialOrd for Instant<Clock> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

// Manually implement Eq so that we don't require `Clock` to be Eq
impl<Clock: crate::Clock> Eq for Instant<Clock> {}

// Manually implement PartialEq so that we don't require `Clock` to be PartialEq
impl<Clock: crate::Clock> PartialEq for Instant<Clock> {
    fn eq(&self, other: &Self) -> bool {
        self.ticks == other.ticks
    }
}

impl<Clock: crate::Clock> Sub<Instant<Clock>> for Instant<Clock> {
    type Output = Duration<Clock>;

    fn sub(self, rhs: Instant<Clock>) -> Self::Output {
        Self::Output {
            // Use a wrapping_sub then conversion to i64 to avoid losing
            // resolution for large values of ticks.
            ticks: self.ticks.wrapping_sub(rhs.ticks).cast_signed(),
            _phantom: PhantomData,
        }
    }
}

impl<Clock: crate::Clock> Add<Duration<Clock>> for Instant<Clock> {
    type Output = Instant<Clock>;

    fn add(self, rhs: Duration<Clock>) -> Self::Output {
        let time = self.checked_add_duration(rhs);
        if time.is_none() {
            pw_assert::panic!("Instant - Duration overflow");
        }
        time.unwrap()
    }
}

impl<Clock: crate::Clock> Sub<Duration<Clock>> for Instant<Clock> {
    type Output = Instant<Clock>;

    fn sub(self, rhs: Duration<Clock>) -> Self::Output {
        let time = self.checked_sub_duration(rhs);
        if time.is_none() {
            pw_assert::panic!("Instant - Duration overflow")
        }
        time.unwrap()
    }
}

/// A span of time represented by a signed tick count.
///
/// A `Duration` is generic over a [`Clock`], preventing arithmetic operations
/// between durations of different clocks at compile-time.
#[derive(Debug)]
pub struct Duration<Clock: crate::Clock> {
    ticks: i64,
    _phantom: PhantomData<Clock>,
}

impl<Clock: crate::Clock> Duration<Clock> {
    /// The maximum possible value for a `Duration`.
    pub const MAX: Self = Self {
        ticks: i64::MAX,
        _phantom: PhantomData,
    };

    /// The minimum possible value for a `Duration`.
    pub const MIN: Self = Self {
        ticks: i64::MIN,
        _phantom: PhantomData,
    };

    /// Returns the raw tick count of this `Duration`.
    #[must_use]
    pub const fn ticks(self) -> i64 {
        self.ticks
    }

    /// Creates a `Duration` from a number of seconds.
    #[must_use]
    pub const fn from_secs(secs: i64) -> Self {
        Self {
            ticks: secs * (Clock::TICKS_PER_SEC.cast_signed()),
            _phantom: PhantomData,
        }
    }

    /// Creates a `Duration` from a number of milliseconds.
    #[must_use]
    pub const fn from_millis(millis: i64) -> Self {
        Self {
            ticks: millis * (Clock::TICKS_PER_SEC.cast_signed()) / 1000,
            _phantom: PhantomData,
        }
    }

    /// Creates a `Duration` from a number of microseconds.
    #[must_use]
    pub const fn from_micros(micros: i64) -> Self {
        Self {
            ticks: micros * (Clock::TICKS_PER_SEC.cast_signed()) / 1_000_000,
            _phantom: PhantomData,
        }
    }

    /// Creates a `Duration` from a number of nanoseconds.
    #[must_use]
    pub const fn from_nanos(nanos: i64) -> Self {
        Self {
            ticks: nanos * (Clock::TICKS_PER_SEC.cast_signed()) / 1_000_000_000,
            _phantom: PhantomData,
        }
    }

    /// Adds another `Duration`, returning `None` if overflow occurred.
    #[must_use]
    pub const fn checked_add(self, rhs: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add(rhs.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }

    /// Subtracts another `Duration`, returning `None` if underflow occurred.
    #[must_use]
    pub const fn checked_sub(self, rhs: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_sub(rhs.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }
}

// Manually implement Copy so that we don't require `Duration` to be Copy
impl<Clock: crate::Clock> Copy for Duration<Clock> {}

// Manually implement Clone so that we don't require `Duration` to be Clone
impl<Clock: crate::Clock> Clone for Duration<Clock> {
    fn clone(&self) -> Self {
        *self
    }
}

// Manually implement Ord so that we don't require `Clock` to be Ord
impl<Clock: crate::Clock> Ord for Duration<Clock> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.ticks.cmp(&other.ticks)
    }
}

// Manually implement PartialOrd so that we don't require `Clock` to be PartialOrd
impl<Clock: crate::Clock> PartialOrd for Duration<Clock> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

// Manually implement Eq so that we don't require `Clock` to be Eq
impl<Clock: crate::Clock> Eq for Duration<Clock> {}

// Manually implement PartialEq so that we don't require `Clock` to be PartialEq
impl<Clock: crate::Clock> PartialEq for Duration<Clock> {
    fn eq(&self, other: &Self) -> bool {
        self.ticks == other.ticks
    }
}

impl<Clock: crate::Clock> Sub<Duration<Clock>> for Duration<Clock> {
    type Output = Duration<Clock>;

    fn sub(self, rhs: Duration<Clock>) -> Self::Output {
        let time = self.checked_sub(rhs);
        if time.is_none() {
            pw_assert::panic!("Duration subtraction overflow")
        }
        time.unwrap()
    }
}

impl<Clock: crate::Clock> Add<Duration<Clock>> for Duration<Clock> {
    type Output = Duration<Clock>;

    fn add(self, rhs: Duration<Clock>) -> Self::Output {
        let time = self.checked_add(rhs);
        if time.is_none() {
            pw_assert::panic!("Duration addition overflow")
        }
        time.unwrap()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug)]
    struct TestClock;

    impl Clock for TestClock {
        const TICKS_PER_SEC: u64 = 1_000;
        fn now() -> Instant<Self> {
            Instant::from_ticks(0)
        }
    }

    #[derive(Debug)]
    struct HighResTestClock;

    impl Clock for HighResTestClock {
        const TICKS_PER_SEC: u64 = 1_000_000_000;
        fn now() -> Instant<Self> {
            Instant::from_ticks(0)
        }
    }

    #[test]
    fn duration_constructors_return_correct_values() {
        assert_eq!(Duration::<TestClock>::from_secs(1234).ticks(), 1_234_000);
        assert_eq!(Duration::<TestClock>::from_millis(1234).ticks(), 1_234);
        assert_eq!(Duration::<TestClock>::from_micros(1234).ticks(), 1);
        assert_eq!(Duration::<TestClock>::from_nanos(1234).ticks(), 0);

        assert_eq!(Duration::<HighResTestClock>::from_nanos(1234).ticks(), 1234);
    }

    #[test]
    fn duration_checked_addition_returns_correct_values() {
        let ten_ms = Duration::<TestClock>::from_millis(10);
        let one_ms = Duration::<TestClock>::from_millis(1);

        assert_eq!(
            ten_ms.checked_add(one_ms),
            Some(Duration::<TestClock>::from_millis(11))
        );

        assert_eq!(
            one_ms.checked_add(ten_ms),
            Some(Duration::<TestClock>::from_millis(11))
        );

        assert_eq!(Duration::<TestClock>::MAX.checked_add(one_ms), None);

        assert_eq!(
            Duration::<TestClock>::MIN.checked_add(Duration::from_millis(-1)),
            None
        );
    }

    #[test]
    fn duration_checked_subtraction_returns_correct_values() {
        let ten_ms = Duration::<TestClock>::from_millis(10);
        let one_ms = Duration::<TestClock>::from_millis(1);

        assert_eq!(
            ten_ms.checked_sub(one_ms),
            Some(Duration::<TestClock>::from_millis(9))
        );

        assert_eq!(
            one_ms.checked_sub(ten_ms),
            Some(Duration::<TestClock>::from_millis(-9))
        );

        assert_eq!(
            Duration::<TestClock>::MAX.checked_sub(Duration::from_millis(-1)),
            None
        );

        assert_eq!(
            Duration::<TestClock>::MIN.checked_sub(Duration::from_millis(1)),
            None
        );
    }

    #[test]
    fn instant_subtraction_returns_correct_values() {
        let ten_ms = Instant::from_ticks(10 * <TestClock as Clock>::TICKS_PER_SEC / 1000);
        let one_ms = Instant::from_ticks(<TestClock as Clock>::TICKS_PER_SEC / 1000);

        assert_eq!(ten_ms - one_ms, Duration::<TestClock>::from_millis(9));
        assert_eq!(one_ms - ten_ms, Duration::<TestClock>::from_millis(-9));
    }

    #[test]
    fn instant_checked_duration_addition_returns_correct_values() {
        let instant_eleven_ms =
            Instant::<TestClock>::from_ticks(11 * <TestClock as Clock>::TICKS_PER_SEC / 1000);
        let instant_ten_ms =
            Instant::<TestClock>::from_ticks(10 * <TestClock as Clock>::TICKS_PER_SEC / 1000);
        let instant_nine_ms =
            Instant::<TestClock>::from_ticks(9 * <TestClock as Clock>::TICKS_PER_SEC / 1000);

        let duration_one_ms = Duration::<TestClock>::from_millis(1);
        let duration_minus_one_ms = Duration::<TestClock>::from_millis(-1);

        assert_eq!(
            instant_ten_ms.checked_add_duration(duration_one_ms),
            Some(instant_eleven_ms)
        );

        assert_eq!(
            instant_ten_ms.checked_add_duration(duration_minus_one_ms),
            Some(instant_nine_ms)
        );

        assert_eq!(
            Instant::<TestClock>::MAX.checked_add_duration(duration_one_ms),
            None
        );

        assert_eq!(
            Instant::<TestClock>::MIN.checked_add_duration(duration_minus_one_ms),
            None
        );
    }

    #[test]
    fn instant_checked_duration_subtraction_returns_correct_values() {
        let instant_eleven_ms =
            Instant::<TestClock>::from_ticks(11 * <TestClock as Clock>::TICKS_PER_SEC / 1000);
        let instant_ten_ms =
            Instant::<TestClock>::from_ticks(10 * <TestClock as Clock>::TICKS_PER_SEC / 1000);
        let instant_nine_ms =
            Instant::<TestClock>::from_ticks(9 * <TestClock as Clock>::TICKS_PER_SEC / 1000);

        let duration_one_ms = Duration::<TestClock>::from_millis(1);
        let duration_minus_one_ms = Duration::<TestClock>::from_millis(-1);

        assert_eq!(
            instant_ten_ms.checked_sub_duration(duration_one_ms),
            Some(instant_nine_ms)
        );

        assert_eq!(
            instant_ten_ms.checked_sub_duration(duration_minus_one_ms),
            Some(instant_eleven_ms)
        );

        assert_eq!(
            Instant::<TestClock>::MAX.checked_sub_duration(duration_minus_one_ms),
            None
        );

        assert_eq!(
            Instant::<TestClock>::MIN.checked_sub_duration(duration_one_ms),
            None
        );
    }
}
