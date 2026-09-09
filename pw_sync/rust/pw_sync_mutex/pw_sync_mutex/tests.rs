// Copyright 2026 The Pigweed Authors
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

use pw_time::Duration;

use crate::{
    Mutex, RawMutex, SingleThreadMutex, SingleThreadTimedMutex, SystemMutex,
    SystemSingleThreadMutex, SystemSingleThreadTimedMutex, SystemTimedMutex,
};

#[unsafe(no_mangle)]
unsafe extern "C-unwind" fn pw_assert_HandleFailure() -> ! {
    panic!("pw_assert failed");
}

#[test]
fn test_mutex_lock_unlock() {
    let lock = SystemMutex::new(42);
    {
        let mut guard = lock.lock();
        assert_eq!(*guard, 42);
        *guard = 43;
    }
    {
        let guard = lock.lock();
        assert_eq!(*guard, 43);
    }
}

#[test]
fn test_timed_mutex_lock_unlock() {
    let lock = SystemTimedMutex::new(42);
    {
        let mut guard = lock.lock();
        assert_eq!(*guard, 42);
        *guard = 43;
    }
    {
        let guard = lock.lock();
        assert_eq!(*guard, 43);
    }
}

#[test]
fn test_mutex_try_lock() {
    let lock = SystemMutex::new(10);
    let guard1 = lock.try_lock();
    assert!(guard1.is_some());

    let guard2 = lock.try_lock();
    assert!(guard2.is_none());

    drop(guard1);

    let guard3 = lock.try_lock();
    assert!(guard3.is_some());
}

#[test]
fn test_timed_mutex_try_lock() {
    let lock = SystemTimedMutex::new(10);
    let guard1 = lock.try_lock();
    assert!(guard1.is_some());

    let guard2 = lock.try_lock();
    assert!(guard2.is_none());

    drop(guard1);

    let guard3 = lock.try_lock();
    assert!(guard3.is_some());
}

#[test]
fn test_timed_mutex_try_lock_for() {
    let lock = SystemTimedMutex::new(100);
    let guard1 = lock.try_lock_for(Duration::from_millis(10));
    assert!(guard1.is_some());

    let guard2 = lock.try_lock_for(Duration::from_millis(10));
    assert!(guard2.is_none());

    drop(guard1);

    let guard3 = lock.try_lock_for(Duration::from_millis(10));
    assert!(guard3.is_some());
}

#[test]
fn test_timed_mutex_try_lock_until() {
    use pw_time::{Clock, SystemClock};

    let lock = SystemTimedMutex::new(100);
    let deadline = SystemClock::now() + Duration::from_millis(50);
    let guard1 = lock.try_lock_until(deadline);
    assert!(guard1.is_some());

    let deadline = SystemClock::now() + Duration::from_millis(10);
    let guard2 = lock.try_lock_until(deadline);
    assert!(guard2.is_none());

    drop(guard1);

    let deadline = SystemClock::now() + Duration::from_millis(50);
    let guard3 = lock.try_lock_until(deadline);
    assert!(guard3.is_some());
}

#[test]
fn test_generic_mutex_new() {
    use core::sync::atomic::{AtomicBool, Ordering};

    #[derive(Default)]
    struct TestRawMutex(AtomicBool);
    unsafe impl RawMutex for TestRawMutex {
        fn try_lock(&self) -> bool {
            !self.0.swap(true, Ordering::Acquire)
        }
        fn lock(&self) {
            while self.0.swap(true, Ordering::Acquire) {
                core::hint::spin_loop();
            }
        }
        unsafe fn unlock(&self) {
            self.0.store(false, Ordering::Release);
        }
    }

    let lock = Mutex::<u32, TestRawMutex>::new(99);
    let guard = lock.lock();
    assert_eq!(*guard, 99);
}

#[test]
fn test_send_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<SystemMutex<u32>>();
    assert_send_sync::<SystemTimedMutex<u32>>();
}

#[test]
fn test_mutex_default() {
    let mutex: SystemMutex<u32> = Default::default();
    assert_eq!(*mutex.lock(), 0);

    let timed_mutex: SystemTimedMutex<u32> = Default::default();
    assert_eq!(*timed_mutex.lock(), 0);
}

#[test]
fn test_single_thread_mutex() {
    let lock = SingleThreadMutex::new(10);
    {
        let mut guard = lock.lock();
        assert_eq!(*guard, 10);
        *guard = 20;
    }
    assert_eq!(*lock.lock(), 20);

    let guard1 = lock.try_lock();
    assert!(guard1.is_some());
    let guard2 = lock.try_lock();
    assert!(guard2.is_none());
    drop(guard1);
    assert!(lock.try_lock().is_some());
}

#[test]
fn test_single_thread_timed_mutex() {
    use pw_time::Clock;

    let lock = SingleThreadTimedMutex::new(10);
    let timeout = pw_time::Duration::from_millis(100);
    let deadline = pw_time::SystemClock::now() + timeout;

    let guard1 = lock.try_lock_for(timeout);
    assert!(guard1.is_some());
    let guard2 = lock.try_lock_for(timeout);
    assert!(guard2.is_none());
    let guard3 = lock.try_lock_until(deadline);
    assert!(guard3.is_none());
    drop(guard1);
    assert!(lock.try_lock_for(timeout).is_some());
}

#[test]
fn test_single_thread_mutex_send() {
    fn assert_send<T: Send>() {}
    assert_send::<SingleThreadMutex<u32>>();

    let mutex = SingleThreadMutex::new(42);
    let handle = std::thread::spawn(move || {
        assert_eq!(*mutex.lock(), 42);
    });
    handle.join().unwrap();
}

#[test]
fn test_single_thread_timed_mutex_custom_clock() {
    use pw_time::{Clock, Instant};

    struct MockClock;
    impl Clock for MockClock {
        const TICKS_PER_SEC: u64 = 1000;
        fn now() -> Instant<Self> {
            Instant::from_ticks(100)
        }
    }

    let lock = SingleThreadTimedMutex::<u32, MockClock>::new(42);
    let timeout = Duration::<MockClock>::from_millis(10);
    assert!(lock.try_lock_for(timeout).is_some());
    let deadline = MockClock::now() + timeout;
    assert!(lock.try_lock_until(deadline).is_some());
}

#[test]
fn test_system_single_thread_mutex_alias() {
    let lock = SystemSingleThreadMutex::new(55);
    assert_eq!(*lock.lock(), 55);
}

#[test]
fn test_system_single_thread_timed_mutex_alias() {
    let lock = SystemSingleThreadTimedMutex::new(77);
    assert_eq!(*lock.lock(), 77);
}

#[cfg(panic = "unwind")]
#[test]
fn test_single_thread_mutex_recursive_lock_panics() {
    let lock = SingleThreadMutex::new(10);
    let _guard = lock.lock();
    let result = std::panic::catch_unwind(core::panic::AssertUnwindSafe(|| {
        let _guard2 = lock.lock();
    }));
    assert!(result.is_err());
}

#[cfg(panic = "unwind")]
#[test]
fn test_mutex_not_unlocked_on_panic() {
    let lock = SystemMutex::new(42);
    let result = std::panic::catch_unwind(core::panic::AssertUnwindSafe(|| {
        let _guard = lock.lock();
        panic!("test panic unwind");
    }));
    assert!(result.is_err());
    // Lock was not unlocked on panic unwind
    assert!(lock.try_lock().is_none());
}

#[cfg(panic = "unwind")]
#[test]
fn test_timed_mutex_not_unlocked_on_panic() {
    let lock = SystemTimedMutex::new(42);
    let result = std::panic::catch_unwind(core::panic::AssertUnwindSafe(|| {
        let _guard = lock.lock();
        panic!("test panic unwind");
    }));
    assert!(result.is_err());
    // Lock was not unlocked on panic unwind
    assert!(lock.try_lock().is_none());
}
